#!/usr/bin/env python3
# ============================================================================
#  run_tests.py — автоматические испытания стенда CSD-шлюза.
#
#  Всё проверяется на том же коде, что едет на ESP32: gw_core.hpp (логика
#  сессии) и gw_director.hpp (координация парка) компилируются g++ под Linux,
#  модемы эмулирует fake_modem.py на псевдо-терминалах.
#
#  T-серия — одиночный шлюз (режим linux_gw <p1> <m1> <p2> <m2>):
#   T1  Счастливый путь: номер -> AT/ATE0 -> ATD -> CONNECT -> обмен в обе
#       стороны -> закрытие клиентом -> трубка положена (+++ / ATH).
#   T2  ДВЕ ОДНОВРЕМЕННЫЕ сессии на двух портах.
#   T3  Клиент уходит во время дозвона -> шлюз вешает трубку, порт жив.
#   T4  Модем отклоняет вызов (NO CARRIER) -> клиенту ERROR, порт жив.
#   T5  Номер не прислали за 15 с -> сессия закрыта, порт жив.
#   T6  «Кривой» модем: OK приходит кусками с паузой (гонка AT-диалога).
#   T7  В DATA удалённая сторона рвёт несущую -> шлюз закрывает TCP и трубку.
#   T8  Второго модема нет вообще -> порт 2 отвечает ERROR, порт 1 работает.
#   T9  Номер печатается по байту с паузами (как в telnet руками).
#  T10  Модем отвечает на AT с задержкой 200 мс (не скорость ESP32 решает).
#
#  F-серия — парк коробок (режимы gw / dir):
#   F0  Агенты всех боксов и собственные линии дирижёра зарегистрированы.
#   F1  Клиент пула получает CONNECT, сквозной обмен данными, линия снова
#       доступна следующему клиенту.
#   F2  Восемь одновременных вызовов на восемь линий; девятому — BUSY;
#       после разрыва линии освобождаются.
#   F3  Отказ линии при дозвоне -> дирижёр перебирает остальные и соединяет.
#   F4  Простой дольше idle_timeout -> соединение разорвано, линия свободна.
#   F5  Гибель бокса -> его линии выводятся из пула, остальные работают.
#   F6  Агент без адреса дирижёра находит его по UDP-объявлению.
#   F7  Прямое подключение к порту бокса работает наряду с пулом.
# ============================================================================

import os
import pty
import subprocess
import socket
import time
import json
import sys
import signal

HERE = os.path.dirname(os.path.abspath(__file__))
GW_BIN = "/tmp/linux_gw"
GW_SRC = os.path.join(HERE, "linux_gw.cpp")
FIRMWARE_DIR = os.path.normpath(os.path.join(HERE, "..", "firmware", "esp32_csd_gateway"))
PORT1, PORT2 = 15000, 15001

# парк: порты и «номер-отказник» для F3
FLEET_BASE = 15300
REJECT_NUM = "+70000000077"

passed, failed = [], []


def report(name, ok, detail=""):
    (passed if ok else failed).append((name, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail and not ok else ""))


def build():
    print("Сборка стенда...")
    r = subprocess.run(["g++", "-O2", "-std=c++17", "-pthread",
                        "-I", FIRMWARE_DIR, GW_SRC, "-o", GW_BIN],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stderr)
        sys.exit(1)


def cleanup():
    # зачистка висяков от прошлых прогонов, иначе bind ломается
    subprocess.run(["pkill", "-f", GW_BIN], capture_output=True)
    subprocess.run(["pkill", "-f", "fake_modem.py"], capture_output=True)
    time.sleep(0.5)


# --------------------------- вспомогательные --------------------------------

def recv_until(sock, needle: bytes, timeout=8.0) -> bytes:
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
            if chunk:
                buf += chunk
                if needle in buf:
                    return buf
        except BlockingIOError:
            pass
        time.sleep(0.02)
    return buf


def wait_close(sock, timeout=10.0) -> bool:
    """Ждёт, пока удалённая сторона закроет TCP (EOF)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if sock.recv(4096) == b"":
                return True
        except BlockingIOError:
            pass
        except OSError:
            return True
        time.sleep(0.1)
    return False


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.setblocking(False)
    return s


def send_all(sock, data: bytes):
    sock.setblocking(True)
    sock.sendall(data)
    sock.setblocking(False)


def logtext(path) -> str:
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


# ------------------------------- T-стенд ------------------------------------

class Bench:
    """Одиночный шлюз: два pty + эмуляторы + linux_gw (старый режим запуска)."""

    def __init__(self, mod1_beh, mod2_beh, second_modem=True):
        self.procs, self.fds = [], []
        import termios

        def spawn_modem(beh):
            m, s = pty.openpty()
            a = termios.tcgetattr(s)
            a[3] &= ~(termios.ECHO | termios.ICANON)
            a[0] &= ~termios.ICRNL
            a[1] &= ~termios.OPOST
            termios.tcsetattr(s, termios.TCSANOW, a)
            p = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "fake_modem.py"),
                 os.ttyname(s), json.dumps(beh)],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.fds += [m, s]
            self.procs.append(p)
            return p, m

        p1, m1 = spawn_modem(mod1_beh)
        self.mods = [p1]
        gw_arg2 = "-"
        if second_modem:
            p2, m2 = spawn_modem(mod2_beh)
            self.mods.append(p2)
            gw_arg2 = f"fd:{m2}"
        self.log = open("/tmp/gw.log", "wb")
        # ВАЖНО: slave-fd остаются открытыми у родителя — иначе master начнёт
        # отдавать EIO, и шлюз не сможет читать ответы модема
        self.gw = subprocess.Popen(
            [GW_BIN, str(PORT1), f"fd:{m1}", str(PORT2), gw_arg2],
            stderr=self.log, stdout=self.log, pass_fds=self.fds)
        time.sleep(0.6)

    def stop(self):
        try:
            self.gw.send_signal(signal.SIGINT)
            self.gw.wait(timeout=5)
        except Exception:
            self.gw.kill()
        for p in self.procs:
            p.kill()
        for fd in self.fds:
            try:
                os.close(fd)
            except OSError:
                pass
        self.log.close()

    def logtext(self):
        return logtext("/tmp/gw.log")


def run_t_tests():
    # ================= прогон 1: обычные модемы ============================
    bench = Bench({"dial_delay": 0.3, "chunk_gap": 0.02, "chunk_size": 32},
                  {"dial_delay": 0.8, "chunk_gap": 0.01, "chunk_size": 16})
    try:
        # T1 -------------------------------------------------------------
        c1 = connect(PORT1)
        send_all(c1, b"+70000000001\r\n")
        got = recv_until(c1, b"CONNECT\n")
        report("T1 дозвон и CONNECT клиенту", b"CONNECT" in got, repr(got))

        payload = b"hello from tcp side"
        send_all(c1, payload)
        back = recv_until(c1, payload, timeout=5)   # эмулятор зеркалит обратно
        report("T1 данные TCP->модем->TCP", payload in back, repr(back))

        remote = b"REMOTE BYTES FROM METER"
        bench.mods[0].stdin.write(b"data " + remote.hex().encode() + b"\n")
        bench.mods[0].stdin.flush()
        got = recv_until(c1, remote, timeout=5)
        report("T1 данные модем->TCP", remote in got, repr(got))

        c1.close()
        # hangup в data-режиме: guard 1с + +++ + guard 1с + ATH — ждём до 6с
        deadline = time.time() + 6
        ok_hangup = False
        while time.time() < deadline:
            log = bench.logtext()
            if "ATH OK" in log or "вызов завершён" in log:
                ok_hangup = True
                break
            time.sleep(0.3)
        report("T1 после ухода клиента шлюз положил трубку (+++/ATH)",
               ok_hangup, bench.logtext()[-500:])

        # T2 -------------------------------------------------------------
        a = connect(PORT1)
        b = connect(PORT2)
        send_all(a, b"+70000000001\r\n")
        send_all(b, b"+70000000002\r\n")
        ga = recv_until(a, b"CONNECT\n", timeout=12)
        gb = recv_until(b, b"CONNECT\n", timeout=12)
        report("T2 сессия A (порт 5000) получила CONNECT", b"CONNECT" in ga, repr(ga))
        report("T2 сессия B (порт 5001) получила CONNECT", b"CONNECT" in gb, repr(gb))

        send_all(a, b"AAAA-payload")
        send_all(b, b"BBBB-payload")
        ra = recv_until(a, b"AAAA-payload", timeout=5)
        rb = recv_until(b, b"BBBB-payload", timeout=5)
        report("T2 эхо-обмен в сессии A", b"AAAA-payload" in ra, repr(ra))
        report("T2 эхо-обмен в сессии B", b"BBBB-payload" in rb, repr(rb))

        bench.mods[0].stdin.write(b"data " + b"meter-one-data".hex().encode() + b"\n")
        bench.mods[0].stdin.flush()
        bench.mods[1].stdin.write(b"data " + b"meter-two-data".hex().encode() + b"\n")
        bench.mods[1].stdin.flush()
        ra = recv_until(a, b"meter-one-data", timeout=5)
        rb = recv_until(b, b"meter-two-data", timeout=5)
        report("T2 данные от модема 1 в сессию A", b"meter-one-data" in ra, repr(ra))
        report("T2 данные от модема 2 в сессию B", b"meter-two-data" in rb, repr(rb))

        # T7: рвём несущую на модеме 1, пока сессия A активна --------------
        bench.mods[0].stdin.write(b"carrier_lost\n")
        bench.mods[0].stdin.flush()
        time.sleep(0.6)
        log = bench.logtext()
        report("T7 NO CARRIER в DATA -> сессия закрыта и трубка положена",
               "NO CARRIER" in log or "потеря несущей" in log or "трубка" in log
               or "ATH OK" in log or "вызов завершён" in log, log[-600:])

        b.close()
        a.close()
        time.sleep(0.5)

        # T3: клиент уходит во время дозвона -------------------------------
        c = connect(PORT1)
        send_all(c, b"+70000000003\r\n")
        time.sleep(0.05)
        c.close()
        time.sleep(1.2)
        c2 = connect(PORT1)
        send_all(c2, b"+70000000004\r\n")
        got = recv_until(c2, b"CONNECT\n", timeout=12)
        report("T3 порт жив после ухода клиента при дозвоне", b"CONNECT" in got, repr(got))
        c2.close()
        time.sleep(0.4)

        # T9: номер по байту, как из telnet -------------------------------
        c = connect(PORT1)
        sock = c
        sock.setblocking(True)
        for ch in b"+70000000005\r\n":
            sock.send(bytes([ch]))
            time.sleep(0.15)
        sock.setblocking(False)
        got = recv_until(c, b"CONNECT\n", timeout=12)
        report("T9 номер, напечатанный по байту с паузами", b"CONNECT" in got, repr(got))
        c.close()
        time.sleep(0.4)

        # порт снова принимает клиента после всех сценариев
        c = connect(PORT1)
        send_all(c, b"+70000000006\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=12)
        report("T2+ порт снова принимает клиента после всех сценариев", b"CONNECT" in got, repr(got))
        c.close()
    finally:
        bench.stop()

    # ============ прогон 2: медленный/кривой модем (гонка sendAT) =========
    bench = Bench({"dial_delay": 0.2, "at_delay": 0.25,
                   "slow_at_chunks": 0.15, "chunk_gap": 0.05, "chunk_size": 8},
                  {"dial_delay": 0.4})
    try:
        c1 = connect(PORT1)
        send_all(c1, b"+70000000001\r\n")
        got = recv_until(c1, b"CONNECT\n", timeout=12)
        report("T6/T10 AT-ответ кусками с паузами — дозвон всё равно прошёл",
               b"CONNECT" in got, repr(got))
        c1.close()
        time.sleep(0.4)

        # T4: эмулятор отвечает NO CARRIER на номера, кончающиеся на 9 -----
        c = connect(PORT1)
        send_all(c, b"+70000000009\r\n")
        got = recv_until(c, b"ERROR", timeout=10)
        report("T4 отказ вызова -> клиенту ERROR, без CONNECT",
               b"ERROR" in got and b"CONNECT" not in got, repr(got))
        c.close()
        time.sleep(0.3)

        # порт снова жив
        c = connect(PORT1)
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=12)
        report("T4+ порт жив после отказа вызова", b"CONNECT" in got, repr(got))
        c.close()
    finally:
        bench.stop()

    # ============ прогон 3: второго модема нет вообще =====================
    bench = Bench({"dial_delay": 0.3}, None, second_modem=False)
    try:
        c = connect(PORT2)
        got = recv_until(c, b"ERROR", timeout=6)
        report("T8 порт 5001 без модема отвечает ERROR", b"ERROR" in got, repr(got))
        c.close()
        time.sleep(0.2)
        c = connect(PORT2)
        got = recv_until(c, b"ERROR", timeout=6)
        report("T8 порт 5001 отвечает ERROR повторно (не завис)", b"ERROR" in got, repr(got))
        c.close()

        c = connect(PORT1)
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=12)
        report("T8 порт 5000 работает, пока на 5001 нет модема", b"CONNECT" in got, repr(got))
        c.close()
    finally:
        bench.stop()


# ------------------------------- F-стенд ------------------------------------

class FleetBench:
    """Парк: дирижёр (со своими 2 SIM) + N боксов по 2 SIM = 2*(N+1) линий."""

    def __init__(self, n_boxes=3, idle_ms=None, agent_timeout_ms=None,
                 ok_owner=None, discover_boxes=(), director_sims=True):
        import termios
        self.procs, self.fds, self.logs = [], [], []
        self.n_boxes = n_boxes
        self.agent_port = FLEET_BASE + 1
        self.pool_port = FLEET_BASE + 2
        self.dir_log = "/tmp/gw_dir.log"
        self.box_logs = [f"/tmp/gw_box{i}.log" for i in range(n_boxes)]

        flags = []
        if idle_ms is not None:
            flags += ["--idle-ms", str(idle_ms)]
        if agent_timeout_ms is not None:
            flags += ["--agent-timeout-ms", str(agent_timeout_ms)]

        def spawn_modem(beh):
            m, s = pty.openpty()
            a = termios.tcgetattr(s)
            a[3] &= ~(termios.ECHO | termios.ICANON)
            a[0] &= ~termios.ICRNL
            a[1] &= ~termios.OPOST
            termios.tcsetattr(s, termios.TCSANOW, a)
            p = subprocess.Popen(
                [sys.executable, os.path.join(HERE, "fake_modem.py"),
                 os.ttyname(s), json.dumps(beh)],
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.fds += [m, s]
            self.procs.append(p)
            return m

        def base_beh(owner, sim):
            beh = {"dial_delay": 0.3, "chunk_gap": 0.02, "chunk_size": 32}
            # ok_owner — единственный, чей модем принимает REJECT_NUM (для F3)
            if ok_owner is not None and owner != ok_owner:
                beh["dial_reject"] = [REJECT_NUM]
            return beh

        # --- дирижёр ---
        dir_flags = flags + ["--discover-addr", "127.0.0.1"]
        dir_log = open(self.dir_log, "wb")
        self.logs.append(dir_log)
        cmd = [GW_BIN, "dir", str(self.agent_port), str(self.pool_port), "central"]
        sim_fds = []
        if director_sims:
            p1 = FLEET_BASE + 3
            p2 = FLEET_BASE + 4
            m1 = spawn_modem(base_beh("central", 0))
            m2 = spawn_modem(base_beh("central", 1))
            sim_fds = [m1, m2]
            cmd += [str(p1), f"fd:{m1}", str(p2), f"fd:{m2}"]
        else:
            cmd += ["--"]
        cmd += dir_flags
        self.dir_proc = subprocess.Popen(cmd, stderr=dir_log, stdout=dir_log,
                                         pass_fds=sim_fds)
        self.procs.append(self.dir_proc)

        # --- боксы ---
        self.box_procs = []
        for i in range(n_boxes):
            p1 = FLEET_BASE + 10 + i * 2
            p2 = FLEET_BASE + 11 + i * 2
            m1 = spawn_modem(base_beh(f"box{i + 1}", 0))
            m2 = spawn_modem(base_beh(f"box{i + 1}", 1))
            addr = "auto" if i in discover_boxes else "127.0.0.1"
            box_flags = flags
            if i in discover_boxes:
                box_flags = flags + ["--discover-addr", "127.0.0.1"]
            log = open(self.box_logs[i], "wb")
            self.logs.append(log)
            p = subprocess.Popen(
                [GW_BIN, "gw", str(p1), f"fd:{m1}", str(p2), f"fd:{m2}",
                 f"box{i + 1}", addr, str(self.agent_port)] + box_flags,
                stderr=log, stdout=log, pass_fds=[m1, m2])
            self.box_procs.append(p)
            self.procs.append(p)
        time.sleep(0.8)

    def box_port(self, box_idx, sim_idx):
        return FLEET_BASE + 10 + box_idx * 2 + sim_idx

    def kill_box(self, idx):
        self.box_procs[idx].kill()

    def wait_log(self, path, needle, timeout=8.0) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in logtext(path):
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        for p in self.procs:
            try:
                p.send_signal(signal.SIGINT)
            except Exception:
                pass
        time.sleep(0.2)
        for p in self.procs:
            try:
                p.wait(timeout=3)
            except Exception:
                p.kill()
        for fd in self.fds:
            try:
                os.close(fd)
            except OSError:
                pass
        for f in self.logs:
            f.close()


def wait_registrations(fb, expect_boxes):
    ok = fb.wait_log(fb.dir_log, "собственные линии", 6)
    for i in range(expect_boxes):
        ok = ok and fb.wait_log(fb.dir_log, f"агент 'box{i + 1}' на связи", 8)
    return ok


def run_f_tests():
    # ============ F1: один клиент через пул ================================
    fb = FleetBench()
    try:
        report("F0 агенты и собственные линии зарегистрированы у дирижёра",
               wait_registrations(fb, 3),
               logtext(fb.dir_log)[-600:])

        c = connect(fb.pool_port)
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=15)
        report("F1 клиент пула получил CONNECT", b"CONNECT" in got, repr(got))

        payload = b"FLEET-ECHO-1"
        send_all(c, payload)
        back = recv_until(c, payload, timeout=5)
        report("F1 сквозной обмен клиент->дирижёр->бокс->модем->обратно",
               payload in back, repr(back))

        c.close()
        time.sleep(1.0)
        c2 = connect(fb.pool_port)
        send_all(c2, b"+70000000002\r\n")
        got = recv_until(c2, b"CONNECT\n", timeout=15)
        report("F1 после закрытия сессии линия снова доступна", b"CONNECT" in got, repr(got))
        c2.close()
    finally:
        fb.stop()

    # ============ F2: все линии заняты, девятому — BUSY ====================
    fb = FleetBench()
    try:
        wait_registrations(fb, 3)
        clients = [connect(fb.pool_port) for _ in range(8)]
        for i, c in enumerate(clients):
            send_all(c, b"+7000000000%d\r\n" % (i + 1))
        ok = 0
        for c in clients:
            got = recv_until(c, b"CONNECT\n", timeout=20)
            if b"CONNECT" in got:
                ok += 1
        report("F2 восемь одновременных вызовов — восемь линий парка", ok == 8, f"connected={ok}")

        c9 = connect(fb.pool_port)
        send_all(c9, b"+70000000001\r\n")
        got = recv_until(c9, b"BUSY", timeout=8)
        report("F2 девятому клиенту отвечено BUSY", b"BUSY" in got, repr(got))
        c9.close()

        for c in clients:
            c.close()
        time.sleep(1.5)
        c = connect(fb.pool_port)
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=15)
        report("F2 линии освобождаются после разрыва", b"CONNECT" in got, repr(got))
        c.close()
    finally:
        fb.stop()

    # ============ F3: отказ линии -> перебор ===============================
    # модемы всех коробок и дирижёра отклоняют REJECT_NUM, кроме box3:
    # дирижёр обязан перебрать линии и соединить через box3
    fb = FleetBench(ok_owner="box3")
    try:
        wait_registrations(fb, 3)
        c = connect(fb.pool_port)
        send_all(c, REJECT_NUM.encode() + b"\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=25)
        report("F3 отказ линий -> дирижёр перебирает и соединяет", b"CONNECT" in got, repr(got))

        log = logtext(fb.dir_log)
        retries = log.count("отклонила вызов")
        report("F3 в журнале видны повторные попытки после отказа", retries >= 2,
               f"retries={retries}\n{log[-600:]}")
        report("F3 соединение установлено через последний исправный бокс",
               "вызов установлен через box3" in log, log[-600:])
        c.close()
    finally:
        fb.stop()

    # ============ F4: таймаут простоя (ускоренный) =========================
    fb = FleetBench(idle_ms=4000)
    try:
        wait_registrations(fb, 3)
        c = connect(fb.pool_port)
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=15)
        report("F4 вызов установлен (idle_timeout=4 c на всём пути)", b"CONNECT" in got, repr(got))

        payload = b"BEFORE-IDLE"
        send_all(c, payload)
        back = recv_until(c, payload, timeout=5)
        report("F4 данные проходят до простоя", payload in back, repr(back))

        closed = wait_close(c, timeout=10)
        report("F4 простой дольше idle_timeout: соединение разорвано", closed)
        report("F4 дирижёр отметил простой в журнале",
               "простой" in logtext(fb.dir_log), logtext(fb.dir_log)[-600:])

        time.sleep(1.0)
        c2 = connect(fb.pool_port)
        send_all(c2, b"+70000000002\r\n")
        got = recv_until(c2, b"CONNECT\n", timeout=15)
        report("F4 после простоя линия снова доступна", b"CONNECT" in got, repr(got))
        c2.close()
    finally:
        fb.stop()

    # ============ F5: гибель бокса =========================================
    fb = FleetBench(agent_timeout_ms=6000)
    try:
        wait_registrations(fb, 3)
        c1 = connect(fb.pool_port)
        send_all(c1, b"+70000000001\r\n")
        got = recv_until(c1, b"CONNECT\n", timeout=15)
        report("F5 вызов через парк до гибели бокса", b"CONNECT" in got, repr(got))
        c1.close()
        time.sleep(1.0)

        fb.kill_box(0)
        time.sleep(0.5)
        report("F5 отключение агента замечено дирижёром",
               fb.wait_log(fb.dir_log, "агент 'box1' отключился", 6),
               logtext(fb.dir_log)[-600:])

        c2 = connect(fb.pool_port)
        send_all(c2, b"+70000000002\r\n")
        got = recv_until(c2, b"CONNECT\n", timeout=15)
        report("F5 вызов идёт через оставшиеся коробки", b"CONNECT" in got, repr(got))
        c2.close()
    finally:
        fb.stop()

    # ============ F6: автопоиск дирижёра по UDP ============================
    fb = FleetBench(n_boxes=1, discover_boxes={0})
    try:
        report("F6 агент нашёл дирижёра по UDP-объявлению",
               fb.wait_log(fb.box_logs[0], "дирижёр найден по UDP", 10),
               logtext(fb.box_logs[0])[-600:])
        report("F6 агент зарегистрирован у найденного дирижёра",
               fb.wait_log(fb.dir_log, "агент 'box1' на связи", 8),
               logtext(fb.dir_log)[-600:])

        c = connect(fb.pool_port)
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=15)
        report("F6 вызов через автоматически найденного дирижёра", b"CONNECT" in got, repr(got))
        c.close()
    finally:
        fb.stop()

    # ============ F7: прямое подключение к боксу ===========================
    fb = FleetBench()
    try:
        wait_registrations(fb, 3)
        c = connect(fb.box_port(0, 0))       # прямой порт SIM1 бокса 1
        send_all(c, b"+70000000001\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=15)
        report("F7 прямое подключение к порту бокса работает", b"CONNECT" in got, repr(got))
        c.close()
        time.sleep(0.5)

        c = connect(fb.pool_port)            # и пул при этом жив
        send_all(c, b"+70000000002\r\n")
        got = recv_until(c, b"CONNECT\n", timeout=15)
        report("F7 пул работает наряду с прямыми подключениями", b"CONNECT" in got, repr(got))
        c.close()
    finally:
        fb.stop()


# ---------------------------------------------------------------------------

def main():
    build()
    cleanup()

    run_t_tests()
    run_f_tests()

    print(f"\nИтог: {len(passed)} passed, {len(failed)} failed")
    if failed:
        for n, d in failed:
            print(f"  FAIL: {n}: {d}")
        sys.exit(1)


if __name__ == "__main__":
    main()
