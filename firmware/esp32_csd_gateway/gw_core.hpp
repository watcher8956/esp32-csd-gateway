// ============================================================================
//  gw_core.hpp — платформо-независимое ядро CSD-шлюза (TCP <-> GSM CSD).
//
//  Файл не зависит ни от Arduino, ни от Linux: вместо конкретных UART и
//  сокетов он работает через четыре узких интерфейса — IModem, IClient,
//  IClock, ILogger. Один и тот же код используется в прошивке ESP32
//  (main.ino) и на испытательном стенде Linux (linux_gw.cpp + fake_modem.py),
//  поэтому вся логика ниже прогоняется автоматическими тестами без железа.
//
//  Сценарий сессии:
//    TCP-клиент присылает первой строкой номер телефона -> шлюз проверяет
//    номер, отвечает модему (AT, ATE0) и набирает ATD<номер> без ';' — это
//    вызов в режиме передачи данных (CSD). После CONNECT шлюз прозрачно
//    ретранслирует байты TCP <-> модем до обрыва любой из сторон. Разрыв
//    инициируется Hayes-последовательностью: 1 с тишины -> +++ -> 1 с
//    тишины -> ATH.
//
//  Дизайн:
//    * service() вызывается внешним циклом каждые ~10 мс и ни разу не
//      блокируется: все ожидания отсчитываются по часам (IClock). Это
//      позволяет вести несколько сессий параллельно в разных потоках;
//    * ответ модема накапливается в rxBuf_, и подстрока ищется в накопленном
//      каждый цикл до истечения таймаута — результат не зависит от того,
//      пришёл ответ одной посылкой или кусками, и от скорости модема;
//    * таймауты собраны в Config. configForModem() возвращает готовый Config
//      под профиль MODEM_*: разные поколения CSD-модулей отвечают на +++ и
//      ATH по-разному, профиль задаётся параметром прошивки и меняется без
//      перекомпиляции (см. gw_params.hpp).
// ============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdarg>

namespace gw {

// ----------------------------- настройки ----------------------------------
struct Config {
    uint32_t phoneTimeoutMs  = 15000;  // ожидание первой строки (номера) от клиента
    uint32_t atTimeoutMs     = 2000;   // ожидание OK на AT / ATE0
    uint32_t dialTimeoutMs   = 45000;  // ожидание CONNECT после ATD
    uint32_t idleTimeoutMs   = 180000; // простой в режиме DATA (180 с) до разрыва
    uint32_t guardMs         = 1000;   // тишина до/после escape-последовательности +++
    uint32_t escapeTimeoutMs = 3000;   // ожидание OK после +++
    uint32_t athTimeoutMs    = 3000;   // ожидание OK после ATH
    bool     expectEscapeOk  = true;   // false: модем вправе не ответить на +++
    bool     expectAthOk     = true;   // false: модем вправе не ответить на ATH
};

// Профили поведения модема. Задаются параметром modem1_type / modem2_type.
enum ModemProfile : uint8_t {
    MODEM_STANDARD        = 0,  // полное подтверждение AT / +++ / ATH
    MODEM_NO_OK_ON_ATH    = 1,  // OK после ATH не обязателен
    MODEM_SLOW            = 2,  // увеличенные таймауты команд и разрыва
    MODEM_NO_OK_ON_ESCAPE = 3,  // OK после +++ не обязателен
};

inline Config configForModem(uint8_t profile) {
    Config c;
    switch (profile) {
        case MODEM_NO_OK_ON_ATH:
            c.athTimeoutMs = 700;
            c.expectAthOk = false;
            break;
        case MODEM_SLOW:
            c.atTimeoutMs = 5000;
            c.dialTimeoutMs = 60000;
            c.guardMs = 1500;
            c.escapeTimeoutMs = 6000;
            c.athTimeoutMs = 6000;
            break;
        case MODEM_NO_OK_ON_ESCAPE:
            c.escapeTimeoutMs = 700;
            c.expectEscapeOk = false;
            break;
        default:
            break;
    }
    return c;
}

// Номер телефона: только цифры, «+» допустим только первым символом.
// Ограничение отсекает случайный мусор и попытки injection в ATD-команду
// (например, «;» переводит вызов в голосовой режим).
inline bool isValidPhone(const char* s, int len) {
    if (len < 3 || len > 20) return false;
    for (int i = 0; i < len; ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') continue;
        if (c == '+' && i == 0)   continue;
        return false;
    }
    return true;
}

// ------------------------ платформенные интерфейсы ------------------------
class IModem {              // последовательный порт модема
public:
    virtual ~IModem() {}
    virtual int read(uint8_t* buf, size_t len) = 0;              // 0=нет данных, >0=байты, <0=ошибка
    virtual int write(const uint8_t* buf, size_t len) = 0;       // <0=ошибка, иначе записано всё
};

class IClient {             // TCP-клиент (одна сессия)
public:
    virtual ~IClient() {}
    virtual int  read(uint8_t* buf, size_t len) = 0;             // 0=нет данных, >0=байты, <0=обрыв
    virtual int  write(const uint8_t* buf, size_t len) = 0;
    virtual bool connected() = 0;
    virtual void stop() = 0;
};

class IClock {              // миллисекунды с переполнением (millis() и т.п.)
public:
    virtual ~IClock() {}
    virtual uint32_t nowMs() = 0;
};

class ILogger {             // куда писать журнал (Serial на ESP32, stderr на стенде)
public:
    virtual ~ILogger() {}
    virtual void log(const char* text) = 0;
};

// ------------------------------- состояния --------------------------------
enum State {
    ST_WAIT_PHONE,  // ждём первую строку — номер телефона
    ST_AT,          // послали AT, ждём OK
    ST_ATE0,        // послали ATE0 (выключить эхо), ждём OK
    ST_DIAL,        // послали ATD<номер>, ждём CONNECT / ошибку
    ST_DATA,        // прозрачная ретрансляция TCP <-> модем
    ST_GUARD_PRE,   // тишина перед +++
    ST_ESC,         // послали +++, ждём OK
    ST_GUARD_POST,  // тишина после +++
    ST_ATH,         // послали ATH, ждём OK
    ST_DONE         // сессия закончена, можно закрывать клиента
};

// ------------------------------- сессия -----------------------------------
class Session {
public:
    Session(IModem& modem, IClient& client, IClock& clock, ILogger& log,
            const char* tag, const Config& cfg = Config())
        : m_(modem), c_(client), clk_(clock), lg_(log), cfg_(cfg), tag_(tag)
    {
        phoneLen_ = 0;
        rxLen_    = 0;
        lastActivity_ = 0;
        enter(ST_WAIT_PHONE);
    }

    // Вызывать каждые ~10 мс из потока порта. Ничего не блокирует.
    void service();

    bool done() const { return st_ == ST_DONE; }

private:
    void enter(State s) { st_ = s; tEnter_ = clk_.nowMs(); }

    void logf(const char* fmt, ...) {
        char b[192];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        char out[208];
        snprintf(out, sizeof(out), "[%s] %s", tag_, b);
        lg_.log(out);
    }

    // --- модем ---
    int rawWrite(const char* s, size_t n) { return m_.write((const uint8_t*)s, n); }

    void sendCmd(const char* cmd) {           // команда + накопление ответа заново
        rxLen_ = 0;
        char line[48];
        int n = snprintf(line, sizeof(line), "%s\r\n", cmd);
        rawWrite(line, (size_t)n);
    }

    // Ответ модема перекачивается в rxBuf_ целиком и searched по подстроке.
    void pumpModemIntoRxBuf() {
        while (rxLen_ < sizeof(rxBuf_) - 1) {
            uint8_t tmp[64];
            size_t want = sizeof(rxBuf_) - 1 - rxLen_;
            if (want > sizeof(tmp)) want = sizeof(tmp);
            int n = m_.read(tmp, want);
            if (n <= 0) break;
            memcpy(rxBuf_ + rxLen_, tmp, (size_t)n);
            rxLen_ += (uint16_t)n;
            rxBuf_[rxLen_] = '\0';
        }
        if (rxLen_ == sizeof(rxBuf_) - 1) {   // переполнение: выбрасываем старую половину
            uint16_t half = rxLen_ / 2;
            memmove(rxBuf_, rxBuf_ + half, (size_t)(rxLen_ - half));
            rxLen_ = (uint16_t)(rxLen_ - half);
            rxBuf_[rxLen_] = '\0';
        }
    }

    bool rxBufHas(const char* s) const { return strstr(rxBuf_, s) != nullptr; }

    // --- клиент ---
    void failToClient() {
        static const char e[] = "ERROR\n";
        c_.write((const uint8_t*)e, sizeof(e) - 1);
    }

    void startHangup(bool wasDataMode) {
        // В командном режиме (вызов не состоялся) достаточно ATH;
        // из режима данных сначала +++, с-guard'ами по guardMs (требование Hayes).
        if (wasDataMode) {
            enter(ST_GUARD_PRE);
        } else {
            sendCmd("ATH");
            enter(ST_ATH);
        }
    }

    // ------------------------------ данные -------------------------------
    IModem&   m_;
    IClient&  c_;
    IClock&   clk_;
    ILogger&  lg_;
    const Config& cfg_;
    const char* tag_;

    State    st_;
    uint32_t tEnter_;         // момент входа в состояние
    uint32_t lastActivity_;   // для таймаута простоя
    char     phone_[24];
    int      phoneLen_;
    char     rxBuf_[256];     // накопитель ответов модема (AT-фаза и дозвон)
    uint16_t rxLen_;          // сколько байт уже накоплено в rxBuf_
    uint8_t  xbuf_[512];      // буфер прозрачной передачи
};

inline void Session::service() {
    uint32_t now = clk_.nowMs();

    switch (st_) {

    case ST_WAIT_PHONE: {
        uint8_t b;
        for (;;) {
            int n = c_.read(&b, 1);
            if (n <= 0) break;
            if (b == '\r' || b == '\n') {
                if (phoneLen_ > 0) {
                    phone_[phoneLen_] = '\0';
                    if (!isValidPhone(phone_, phoneLen_)) {
                        logf("некорректный номер: %s", phone_);
                        failToClient();
                        enter(ST_DONE);
                        return;
                    }
                    logf("номер: %s", phone_);
                    sendCmd("AT");
                    enter(ST_AT);
                    return;
                }
            } else if (phoneLen_ < (int)sizeof(phone_) - 1) {
                phone_[phoneLen_++] = (char)b;
            }
        }
        if (!c_.connected()) {
            logf("клиент отключился, не прислав номер");
            enter(ST_DONE);
            return;
        }
        if (now - tEnter_ > cfg_.phoneTimeoutMs) {
            logf("таймаут ввода номера (%u мс)", (unsigned)cfg_.phoneTimeoutMs);
            enter(ST_DONE);
            return;
        }
        break;
    }

    case ST_AT:
    case ST_ATE0: {
        pumpModemIntoRxBuf();
        if (rxBufHas("OK")) {
            if (st_ == ST_AT) {
                sendCmd("ATE0");
                enter(ST_ATE0);
            } else {
                char cmd[48];
                snprintf(cmd, sizeof(cmd), "ATD%s", phone_);
                logf("набор номера %s", phone_);
                sendCmd(cmd);
                enter(ST_DIAL);
            }
            return;
        }
        if (now - tEnter_ > cfg_.atTimeoutMs) {
            logf("модем не отвечает на %s", (st_ == ST_AT ? "AT" : "ATE0"));
            failToClient();               // вызова ещё не было — ATH не нужен
            enter(ST_DONE);
        }
        break;
    }

    case ST_DIAL: {
        if (!c_.connected()) {            // клиент мог уйти во время дозвона
            logf("клиент отключился во время набора");
            startHangup(false);
            return;
        }
        pumpModemIntoRxBuf();
        if (rxBufHas("CONNECT")) {
            static const char ok[] = "CONNECT\n";
            c_.write((const uint8_t*)ok, sizeof(ok) - 1);
            logf("CSD установлено");
            rxLen_ = 0;                   // хвост ответа модема в данные не отдаём
            lastActivity_ = now;
            enter(ST_DATA);
            return;
        }
        static const char* errs[] = {
            "NO CARRIER", "BUSY", "NO ANSWER", "NO DIALTONE", "ERROR"
        };
        for (const char* e : errs) {
            if (rxBufHas(e)) {
                logf("модем отклонил вызов: %s", e);
                failToClient();
                startHangup(false);
                return;
            }
        }
        if (now - tEnter_ > cfg_.dialTimeoutMs) {
            logf("таймаут дозвона (%u мс)", (unsigned)cfg_.dialTimeoutMs);
            failToClient();
            startHangup(false);
            return;
        }
        {   // клиент мог прислать данные во время набора — игнорируем
            uint8_t sink[64];
            while (c_.read(sink, sizeof(sink)) > 0) {}
        }
        break;
    }

    case ST_DATA: {
        int n = c_.read(xbuf_, sizeof(xbuf_));       // TCP -> модем
        if (n < 0) { logf("обрыв TCP");               startHangup(true); return; }
        if (n > 0) {
            if (m_.write(xbuf_, (size_t)n) < 0) { logf("ошибка записи модему"); startHangup(true); return; }
            lastActivity_ = now;
        }
        n = m_.read(xbuf_, sizeof(xbuf_));            // модем -> TCP
        if (n < 0) { logf("потеря несущей");          startHangup(true); return; }
        if (n > 0) {
            if (c_.write(xbuf_, (size_t)n) < 0) { logf("ошибка записи клиенту"); startHangup(true); return; }
            lastActivity_ = now;
        }
        if (!c_.connected()) {
            logf("TCP-клиент отключился");
            startHangup(true);
            return;
        }
        if (now - lastActivity_ > cfg_.idleTimeoutMs) {
            logf("таймаут простоя (%u мс), разрываем CSD", (unsigned)cfg_.idleTimeoutMs);
            startHangup(true);
            return;
        }
        break;
    }

    case ST_GUARD_PRE:      // тишина перед +++
        if (now - tEnter_ >= cfg_.guardMs) {
            rxLen_ = 0;
            rawWrite("+++", 3);
            enter(ST_ESC);
        }
        break;

    case ST_ESC:            // ждём OK на +++; для некоторых модемов его не бывает
        pumpModemIntoRxBuf();
        if (rxBufHas("OK")) {
            enter(ST_GUARD_POST);
            return;
        }
        if (now - tEnter_ > cfg_.escapeTimeoutMs) {
            if (cfg_.expectEscapeOk) {
                logf("нет OK после +++");
            } else {
                logf("+++ без OK — допустимо для профиля модема");
            }
            enter(ST_GUARD_POST);
        }
        break;

    case ST_GUARD_POST:     // тишина после +++
        if (now - tEnter_ >= cfg_.guardMs) {
            sendCmd("ATH");
            enter(ST_ATH);
        }
        break;

    case ST_ATH:
        pumpModemIntoRxBuf();
        if (rxBufHas("OK")) {
            logf("вызов завершён (ATH OK)");
            enter(ST_DONE);
            return;
        }
        if (now - tEnter_ > cfg_.athTimeoutMs) {
            if (cfg_.expectAthOk) {
                logf("ATH без ответа — закрываем сессию");
            } else {
                logf("ATH без подтверждения — допустимо для профиля модема");
            }
            enter(ST_DONE);
        }
        break;

    case ST_DONE:
        break;
    }
}

} // namespace gw
