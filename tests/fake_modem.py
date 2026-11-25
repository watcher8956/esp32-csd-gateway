#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
fake_modem.py — эмулятор SIM800L для испытательного стенда.

Подключается к псевдо-терминалу, «подключённому» вместо модема к
Linux-версии шлюза (linux_gw), и ведёт себя как настоящий SIM800L:
  * отвечает на AT / ATE0;
  * на ATD<номер> отвечает CONNECT после «времени установления» (можно
    задать ошибку: NO CARRIER / BUSY / NO ANSWER / NO DIALTONE / ERROR);
  * после CONNECT зеркалит пользовательские данные обратно со «скоростью
    модема» (маленькими рывками с паузами — как настоящий CSD-канал);
  * на +++ выходит в командный режим (OK) и слушает ATH;
  * поддерживает «кривой» режим slow_at: отвечает на AT только после
    длинной паузы и кусками (чтобы проверить, что логика AT-диалога
    не зависит от совпадения скоростей);
  * список dial_reject — номера, на которые модем отвечает NO CARRIER
    (для проверки перебора линий дирижёром).

Управление (stdin, по одной команде в строке):
  data <hex>       — выдавить байты в сторону шлюза (данные от удалённого
                     устройства, как будто их прислал «счётчик»);
  carrier_lost     — имитировать сброс несущей (обрыв CSD);
  quit             — выход.
"""

import os
import sys
import time
import select
import threading


class FakeModem:
    def __init__(self, tty_path: str, behavior: dict):
        self.tty = os.open(tty_path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        # выключаем эхо/построчную обработку на нашей стороне pty:
        # без этого pty сам возвращает отправленные команды и портит протокол
        import termios
        attrs = termios.tcgetattr(self.tty)
        attrs[3] &= ~termios.ECHO
        attrs[0] &= ~termios.ICRNL
        attrs[3] &= ~termios.ICANON
        termios.tcsetattr(self.tty, termios.TCSANOW, attrs)
        self.behavior = behavior          # dial_result, dial_delay, at_delay, chunk_gap
        self.state = "cmd"                # cmd | dialing | data
        self.dialed_number = ""
        self.rx = b""                     # накопитель принятого от шлюза
        self.lock = threading.Lock()
        self.running = True

    # ------------------------------ низкий уровень ------------------------
    def _write(self, data: bytes):
        try:
            os.write(self.tty, data)
        except OSError:
            pass

    def _read(self) -> bytes:
        out = b""
        while True:
            try:
                chunk = os.read(self.tty, 4096)
                if not chunk:
                    break
                out += chunk
            except BlockingIOError:
                break
            except OSError:
                break
        return out

    # ------------------------------ AT-машина -----------------------------
    def handle_input(self, data: bytes):
        """Данные от шлюза (команды или пользовательский трафик)."""
        self.rx += data
        if self.state == "cmd":
            # ждём полную строку команды
            while b"\r" in self.rx or b"\n" in self.rx:
                nl = -1
                for i, b in enumerate(self.rx):
                    if b in (0x0D, 0x0A):
                        nl = i
                        break
                if nl < 0:
                    break
                line = self.rx[:nl].decode("ascii", "replace").strip()
                self.rx = self.rx[nl + 1:]
                self.handle_line(line)
        elif self.state == "dialing":
            # во время дозвона ATD уже послали; прочий мусор игнорируем
            pass
        else:  # data
            # копим, ищем escape +++ в потоке; пользовательские данные
            # зеркалим обратно наружу (эхо-канал, как настоящий CSD-петлбэк)
            if b"+++" in self.rx:
                self.rx = b""
                self.state = "cmd"
                self._write(b"\r\nOK\r\n")
            else:
                out = self.rx
                self.rx = b""
                if out:
                    self._write(out)

    def handle_line(self, line: str):
        at_delay = self.behavior.get("at_delay", 0.0)
        if at_delay:
            time.sleep(at_delay)

        if line == "AT":
            resp = b"OK"
            # «кривой» режим: отдаём ответ двумя кусками с паузой
            if self.behavior.get("slow_at_chunks"):
                self._write(resp[:1])
                time.sleep(self.behavior["slow_at_chunks"])
                self._write(resp[1:] + b"\r\n")
            else:
                self._write(b"\r\n" + resp + b"\r\n")
        elif line == "ATE0":
            self._write(b"\r\nOK\r\n")
        elif line == "ATH":
            self._write(b"\r\nOK\r\n")
        elif line.startswith("ATD"):
            self.state = "dialing"
            self.dialed_number = line[3:]
            threading.Thread(target=self.do_dial, daemon=True).start()
        elif line == "":
            pass
        else:
            self._write(b"\r\nERROR\r\n")

    def do_dial(self):
        delay = self.behavior.get("dial_delay", 1.0)
        result = self.behavior.get("dial_result", "CONNECT")
        time.sleep(delay)
        if self.state != "dialing":
            return
        # номер из списка dial_reject отклоняется независимо от остального
        if self.dialed_number in self.behavior.get("dial_reject", []):
            result = "NO CARRIER"
        # номер, оканчивающийся на 9, «не доступен» — удобно для теста отказа
        if self.dialed_number.endswith("9") and result == "CONNECT":
            result = "NO CARRIER"
        if result == "CONNECT":
            self.state = "data"
            self._write(b"\r\nCONNECT 9600\r\n")
        else:
            self.state = "cmd"
            self._write(b"\r\n" + result.encode() + b"\r\n")

    # ------------------------------ внешний поток -------------------------
    def emit(self, data: bytes):
        """Данные от «удалённого устройства» в сторону шлюза, рывками."""
        gap = self.behavior.get("chunk_gap", 0.02)
        chunk = self.behavior.get("chunk_size", 32)
        for i in range(0, len(data), chunk):
            self._write(data[i:i + chunk])
            time.sleep(gap)

    def carrier_lost(self):
        if self.state == "data":
            self.state = "cmd"
            self._write(b"\r\nNO CARRIER\r\n")

    # ------------------------------ главный цикл --------------------------
    def loop(self):
        while self.running:
            r, _, _ = select.select([self.tty], [], [], 0.05)
            if r:
                data = self._read()
                if data:
                    self.handle_input(data)

    def run(self):
        t = threading.Thread(target=self.loop, daemon=True)
        t.start()
        # команды управления из stdin
        for raw in sys.stdin:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("data "):
                self.emit(bytes.fromhex(line[5:]))
            elif line == "carrier_lost":
                self.carrier_lost()
            elif line == "quit":
                break
        self.running = False


if __name__ == "__main__":
    # fake_modem.py <pty-path> <behavior-json>
    import json
    tty = sys.argv[1]
    beh = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    m = FakeModem(tty, beh)
    m.run()
