// ============================================================================
//  gw_director.hpp — платформо-независимая надстройка над gw_core.hpp:
//  координация парка шлюзов («дирижёр» и агенты).
//
//  Зачем: каждая коробка шлюза обслуживает две SIM-карты, а коробок в
//  развёртывании может быть до 16. Без координации клиент должен сам знать,
//  на какой адрес и порт идти. С координацией клиент всегда подключается к
//  одному порту дирижёра, а тот сам находит свободную линию во всём парке:
//
//    ┌────────┐   TCP    ┌──────────┐   TCP    ┌──────┐
//    │ клиент ├──────────► дирижёр  ├──────────► бокс │──► свободная SIM
//    └────────┘          │  (пул)   │          └──────┘
//                        │          ▲
//                        │  TCP     │ агенты всех боксов: HELLO/STATE/PING
//                        │  UDP     │ поиск дирижёра (объявление GWD1 DISCOVER)
//
//  Роли (включаются параметрами прошивки, любая коробка может быть любой):
//    * агент есть у каждой коробки: держит постоянное соединение с дирижёром,
//      сообщает занятость своих SIM (STATE) и живость (PING), при обрыве
//      сам переподключается и перерегистрируется;
//    * дирижёр ведёт таблицу линий парка (до 16 боксов × 2 SIM), выбирает
//      свободную по кругу (round-robin), сам подключается к выбранному боксу
//      и ретранслирует сессию. Клиенту он отвечает ровно тем же протоколом,
//      что и одиночный шлюз: первая строка — номер, ответ CONNECT/ERROR/BUSY.
//
//  Освобождение линий: агент сообщает STATE ... FREE, как только его сессия
//  завершилась (в том числе по таймеру простоя 180 с — он един и действует
//  на всём пути: и в боксе, и в ретрансляции дирижёра). Если от агента нет
//  весточек дольше agentTimeoutMs, его линии считаются офлайн.
//
//  Автообнаружение: агент может не знать адрес дирижёра — тогда он
//  периодически посылает UDP-объявление на discoverAddr (по умолчанию
//  255.255.255.255 — limited broadcast на всю локальную сеть), и дирижёр
//  отвечает ему напрямую. Новая коробка подключается к парку без ручной
//  настройки адресов.
//
//  Как и gw_core.hpp, файл не привязан к платформе: сеть, UDP и часы
//  подключаются через интерфейсы IListener/IConnector/IUdp/IClock, поэтому
//  весь координатор проверяется на Linux-стенде автоматическими тестами.
// ============================================================================

#pragma once

#include "gw_core.hpp"

namespace gwd {

// переиспользуем базовые интерфейсы ядра
using gw::IClient;
using gw::IClock;
using gw::ILogger;

// ----------------------------- конфигурация --------------------------------

static const char    PROTO_TAG[]      = "GWD1";
static const uint8_t MAX_BOXES       = 16;  // до 16 коробок в парке
static const uint8_t MAX_LINES       = 32;  // 16 боксов × 2 SIM
static const uint8_t MAX_AGENTS      = 16;  // одновременных агентских соединений
static const uint8_t MAX_POOL_CLIENTS = MAX_LINES;  // пул вмещает все линии парка

struct FleetConfig {
    char     selfName[16]     = "gw";   // имя коробки в парке
    char     selfIp[16]       = "";     // свой адрес (для HELLO; обновляет прошивка)
    char     directorIp[16]   = "";     // адрес дирижёра; пусто = искать по UDP
    char     discoverAddr[16] = "255.255.255.255";
    uint16_t selfPort1        = 5000;   // порты линий этой коробки (для HELLO)
    uint16_t selfPort2        = 5001;
    uint16_t directorPort     = 5010;   // агент: порт дирижёра; дирижёр: порт агентов + UDP
    uint16_t poolPort         = 5020;   // дирижёр: порт для клиентов пула
    bool     autoDiscover     = true;   // UDP-объявление, если адрес не задан
    uint32_t phoneTimeoutMs   = 15000;
    uint32_t dialTimeoutMs    = 45000;
    uint32_t idleTimeoutMs    = 180000; // простой вызова до разрыва (180 с, как у шлюза)
    uint32_t agentTimeoutMs   = 45000;  // нет весточек дольше — линии считаются офлайн
    uint32_t pingPeriodMs     = 10000;
    uint32_t helloPeriodMs    = 30000;
    uint32_t reconnectMs      = 3000;
    uint32_t discoverPeriodMs = 2000;
    uint32_t connectTimeoutMs = 4000;   // на исходящее соединения к боксу
};

// ----------------------------- интерфейсы ----------------------------------

class IListener {           // входящий TCP-сервер (соединения агентов/клиентов)
public:
    virtual ~IListener() {}
    virtual IClient* acceptClient() = 0;   // nullptr, если подключений нет; объект удаляет вызывающий
};

class IConnector {          // исходящие соединения (дирижёр -> бокс)
public:
    virtual ~IConnector() {}
    virtual IClient* connectTo(const char* ip, uint16_t port, uint32_t timeoutMs) = 0;
};

class IUdp {                // UDP для автообнаружения (объявление/ответ)
public:
    virtual ~IUdp() {}
    virtual bool sendTo(const char* ip, uint16_t port, const char* data, size_t len) = 0;
    virtual int  receive(char* buf, size_t cap, char* fromIp, size_t ipCap, uint16_t& fromPort) = 0;
};

class ILineState {          // занятость двух SIM этой коробки (флаги задач портов)
public:
    virtual ~ILineState() {}
    virtual bool simBusy(uint8_t idx) = 0;   // idx: 0 = SIM1, 1 = SIM2
};

// ---------------------------- вспомогательное ------------------------------

// Строгая проверка строки IPv4 (без внешних библиотек: код общий для платформ).
inline bool validIpv4(const char* s) {
    if (!s || !s[0]) return false;
    int octets = 0;
    const char* p = s;
    while (*p) {
        if (*p < '0' || *p > '9') return false;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            ++digits;
            ++p;
            if (digits > 3) return false;
        }
        if (v > 255) return false;
        ++octets;
        if (*p == '.') { ++p; if (!*p) return false; }
        else if (*p)     { return false; }
    }
    return octets == 4;
}

// Накопитель строк, разделённых '\n'. Неблокирующий, с защитой от переполнения:
// строка, не поместившаяся в буфер, отбрасывается целиком.
class LineReader {
public:
    void clear() { len_ = 0; }

    void feed(const uint8_t* data, size_t n) {
        while (n--) {
            if (len_ >= sizeof(buf_)) len_ = 0;   // длинный мусор — сбрасываем накопитель
            buf_[len_++] = *data++;
        }
    }

    // >0 — длина готовой строки (скопирована в out), 0 — строк пока нет,
    // -1 — накопитель переполнился без '\n', содержимое сброшено.
    int next(char* out, size_t outCap) {
        for (;;) {
            size_t nl = len_;
            for (size_t i = 0; i < len_; ++i) {
                if (buf_[i] == '\n') { nl = i; break; }
            }
            if (nl == len_) {
                if (len_ == sizeof(buf_)) { len_ = 0; return -1; }
                return 0;
            }
            size_t lineLen = nl;
            if (lineLen > 0 && buf_[lineLen - 1] == '\r') lineLen--;
            memmove(buf_, buf_ + nl + 1, len_ - nl - 1);
            len_ -= nl + 1;
            if (lineLen == 0) continue;            // пустые строки игнорируем
            if (lineLen + 1 > outCap) continue;    // не влезла — игнорируем
            memcpy(out, buf_, lineLen);
            out[lineLen] = '\0';
            return (int)lineLen;
        }
    }

private:
    uint8_t buf_[128];
    size_t  len_ = 0;
};

// ------------------------------- дирижёр -----------------------------------

class Director {
public:
    Director(IClock& clk, ILogger& lg, const FleetConfig& cfg,
             IListener& agentListener, IListener& poolListener,
             IConnector& connector, IUdp* udp, ILineState* localLines)
        : clk_(clk), lg_(lg), cfg_(cfg),
          agentListener_(agentListener), poolListener_(poolListener),
          connector_(connector), udp_(udp), localLines_(localLines)
    {
        for (uint8_t i = 0; i < MAX_LINES; ++i) lines_[i] = Line{};
        for (uint8_t i = 0; i < MAX_AGENTS; ++i) agents_[i] = AgentConn{};
        for (uint8_t i = 0; i < MAX_POOL_CLIENTS; ++i) pool_[i] = PoolClient{};
        rrCursor_ = 0;
    }

    // Регистрирует линии самой коробки-дирижёра (агент ей не нужен: состояние
    // приходит напрямую от задач портов). Вызывается один раз перед service().
    void addLocalLine(const char* name, const char* ip, uint16_t port1, uint16_t port2) {
        int base = freePair();
        if (base < 0) { logf("нет места для собственных линий"); return; }
        initLine(lines_[base],     name, ip, port1, 1, true);
        initLine(lines_[base + 1], name, ip, port2, 2, true);
        logf("собственные линии: SIM1 %s:%u, SIM2 %s:%u", ip, port1, ip, port2);
    }

    // Вызывать каждые ~10 мс из потока дирижёра. Соединения к боксам
    // (connectTo) блокирующие на время connectTimeoutMs — на это время цикл
    // дирижёра приостанавливается, остальные роли работают в своих потоках.
    void service() {
        const uint32_t now = clk_.nowMs();
        foldLocalState();
        sweepFreshness(now);
        acceptAgents();
        acceptPoolClients(now);
        serviceAgents(now);
        servicePoolClients(now);
        serviceDiscovery();
    }

    // Сколько линий парка свободно прямо сейчас (для диагностики).
    uint8_t freeLineCount() const {
        uint8_t n = 0;
        for (uint8_t i = 0; i < MAX_LINES; ++i) {
            const Line& L = lines_[i];
            if (L.used && L.online && !L.agentBusy && !L.relayBusy) ++n;
        }
        return n;
    }

private:
    // Одна линия парка = одна SIM одного бокса. Бокс занимает пару соседних
    // слотов (SIM1, SIM2).
    struct Line {
        bool     used;
        bool     local;       // линия самой коробки-дирижёра
        bool     online;      // агент на связи (для локальных всегда)
        bool     agentBusy;   // агент сообщил о занятой SIM (или локальный флаг)
        bool     relayBusy;   // занята текущим ретранслируемым вызовом
        uint8_t  sim;         // 1 или 2 — номер SIM у бокса
        int8_t   agentConn;   // индекс агентского соединения или -1
        char     name[16];
        char     ip[16];
        uint16_t port;
        uint32_t lastSeenMs;
    };

    struct AgentConn {
        IClient*   cli;
        LineReader lr;
    };

    enum PoolPhase : uint8_t { PH_PHONE, PH_CONNECT, PH_DIAL, PH_DATA, PH_DONE };

    // Клиент пула: тот же протокол, что у одиночного шлюза, но свободную
    // линию подбирает дирижёр, при отказе пробует следующую.
    struct PoolClient {
        IClient*  cli;
        PoolPhase phase;
        LineReader lr;                    // фаза ввода номера
        char      phone[40];
        char      rxBuf[192];             // накопитель ответа бокса при дозвоне
        uint16_t  rxLen;
        uint32_t  triedMask[(MAX_LINES + 31) / 32];  // уже неудачно опробованные
        int       curLine;                // линия, закреплённая за этим вызовом
        IClient*  box;
        uint32_t  tPhase;
        uint32_t  tActivity;
    };

    // ------------------------------ журнал -------------------------------
    void logf(const char* fmt, ...) {
        char b[192];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        char out[208];
        snprintf(out, sizeof(out), "[DIR] %s", b);
        lg_.log(out);
    }

    // ---------------------------- таблица линий --------------------------
    int freePair() const {
        for (uint8_t i = 0; i + 1 < MAX_LINES; i += 2) {
            if (!lines_[i].used && !lines_[i + 1].used) return (int)i;
        }
        return -1;
    }

    void initLine(Line& L, const char* name, const char* ip, uint16_t port,
                  uint8_t sim, bool local) {
        memset(&L, 0, sizeof(L));
        L.used = true;
        L.local = local;
        L.online = true;
        L.sim = sim;
        L.agentConn = -1;
        snprintf(L.name, sizeof(L.name), "%s", name);
        snprintf(L.ip, sizeof(L.ip), "%s", ip);
        L.port = port;
        L.lastSeenMs = clk_.nowMs();
    }

    void foldLocalState() {
        if (!localLines_) return;
        for (uint8_t i = 0; i < MAX_LINES; ++i) {
            Line& L = lines_[i];
            if (L.used && L.local) {
                L.agentBusy = localLines_->simBusy(L.sim - 1);
                L.lastSeenMs = clk_.nowMs();
            }
        }
    }

    void sweepFreshness(uint32_t now) {
        for (uint8_t i = 0; i < MAX_LINES; ++i) {
            Line& L = lines_[i];
            if (L.used && !L.local && L.online &&
                now - L.lastSeenMs > cfg_.agentTimeoutMs) {
                logf("нет весточек от '%s' — линии отключены", L.name);
                L.online = false;
                L.agentBusy = false;
            }
        }
    }

    // ------------------------------ агенты -------------------------------
    void acceptAgents() {
        for (;;) {
            IClient* c = agentListener_.acceptClient();
            if (!c) return;
            int slot = -1;
            for (uint8_t i = 0; i < MAX_AGENTS; ++i) {
                if (!agents_[i].cli) { slot = i; break; }
            }
            if (slot < 0) {
                logf("лимит агентских соединений исчерпан — отказ");
                c->stop();
                delete c;
                continue;
            }
            agents_[slot].cli = c;
            agents_[slot].lr.clear();
        }
    }

    void detachAgent(uint8_t idx, const char* why) {
        AgentConn& a = agents_[idx];
        const char* name = "?";
        for (uint8_t i = 0; i < MAX_LINES; ++i) {
            Line& L = lines_[i];
            if (L.used && !L.local && L.agentConn == (int8_t)idx) {
                name = L.name;
                L.online = false;
                L.agentBusy = false;
                L.agentConn = -1;
            }
        }
        logf("агент '%s' %s", name, why);
        if (a.cli) { a.cli->stop(); delete a.cli; a.cli = nullptr; }
        a.lr.clear();
    }

    void serviceAgents(uint32_t now) {
        for (uint8_t idx = 0; idx < MAX_AGENTS; ++idx) {
            AgentConn& a = agents_[idx];
            if (!a.cli) continue;
            uint8_t tmp[256];
            int n = a.cli->read(tmp, sizeof(tmp));
            if (n < 0) { detachAgent(idx, "отключился"); continue; }
            if (n > 0) {
                a.lr.feed(tmp, (size_t)n);
                char line[128];
                int r;
                while ((r = a.lr.next(line, sizeof(line))) != 0) {
                    if (r > 0) handleAgentLine(idx, line, now);
                }
            }
        }
    }

    void handleAgentLine(uint8_t idx, const char* line, uint32_t now) {
        if (strncmp(line, "GWD1 ", 5) != 0) return;
        const char* p = line + 5;

        char name[16] = {0}, ip[16] = {0}, w1[8] = {0}, w2[8] = {0};
        unsigned p1 = 0, p2 = 0;

        if (sscanf(p, "HELLO %15s %15s %u %u", name, ip, &p1, &p2) == 4) {
            registerAgent(idx, name, ip, (uint16_t)p1, (uint16_t)p2, now);
            return;
        }
        if (sscanf(p, "STATE %3s %7s", w1, w2) == 2) {
            uint8_t sim = 0;
            if      (strcmp(w1, "SIM1") == 0) sim = 1;
            else if (strcmp(w1, "SIM2") == 0) sim = 2;
            bool busy = (strcmp(w2, "BUSY") == 0);
            for (uint8_t i = 0; i < MAX_LINES; ++i) {
                Line& L = lines_[i];
                if (L.used && !L.local && L.agentConn == (int8_t)idx && L.sim == sim) {
                    L.agentBusy = busy;
                    L.lastSeenMs = now;
                    logf("%s: SIM%u %s", L.name, (unsigned)sim, busy ? "BUSY" : "FREE");
                }
            }
            return;
        }
        // PING и любые незнакомые строки — просто подтверждение живости
        for (uint8_t i = 0; i < MAX_LINES; ++i) {
            Line& L = lines_[i];
            if (L.used && !L.local && L.agentConn == (int8_t)idx) L.lastSeenMs = now;
        }
    }

    void registerAgent(uint8_t idx, const char* name, const char* ip,
                       uint16_t p1, uint16_t p2, uint32_t now) {
        if (!validIpv4(ip) || p1 == 0 || p2 == 0) {
            logf("агент прислал некорректный HELLO (ip=%s)", ip);
            return;
        }
        // бокс с этим именем уже есть — обновляем (переподключение агента).
        // Пары слотов всегда начинаются с чётного индекса, поэтому ищем
        // только по чётным: иначе найдя SIM2, затёрли бы пару соседнего бокса.
        int base = -1;
        for (uint8_t i = 0; i < MAX_LINES; i += 2) {
            if (lines_[i].used && strcmp(lines_[i].name, name) == 0) { base = (int)i; break; }
        }
        if (base < 0) {
            base = freePair();
            if (base < 0) {
                logf("таблица заполнена (%u боксов) — '%s' не размещён", MAX_BOXES, name);
                return;
            }
        }
        initLine(lines_[base],     name, ip, p1, 1, false);
        initLine(lines_[base + 1], name, ip, p2, 2, false);
        lines_[base].agentConn = (int8_t)idx;
        lines_[base + 1].agentConn = (int8_t)idx;
        logf("агент '%s' на связи: SIM1 %s:%u, SIM2 %s:%u", name, ip, p1, ip, p2);
        (void)now;
    }

    // --------------------------- клиенты пула ----------------------------
    void acceptPoolClients(uint32_t now) {
        for (;;) {
            IClient* c = poolListener_.acceptClient();
            if (!c) return;
            int slot = -1;
            for (uint8_t i = 0; i < MAX_POOL_CLIENTS; ++i) {
                if (!pool_[i].cli) { slot = i; break; }
            }
            if (slot < 0) {
                static const char busy[] = "BUSY\n";
                c->write((const uint8_t*)busy, sizeof(busy) - 1);
                c->stop();
                delete c;
                logf("лимит клиентов пула исчерпан — отказ");
                continue;
            }
            PoolClient& p = pool_[slot];
            p = PoolClient{};                      // чистый слот: буферы, маски, счётчики
            p.cli = c;
            p.phase = PH_PHONE;
            p.curLine = -1;
            p.tPhase = now;
        }
    }

    void finishClient(uint8_t idx) {
        PoolClient& p = pool_[idx];
        if (p.curLine >= 0) lines_[p.curLine].relayBusy = false;   // линия освобождена
        if (p.box)  { p.box->stop();  delete p.box;  p.box = nullptr; }
        if (p.cli)  { p.cli->stop();  delete p.cli;  p.cli = nullptr; }
        p.phase = PH_DONE;
    }

    void failClient(uint8_t idx, const char* msg) {
        pool_[idx].cli->write((const uint8_t*)msg, strlen(msg));
        finishClient(idx);
    }

    // Круговой выбор свободной линии среди живых; уже неудачные для этого
    // клиента пропускаются через triedMask.
    int pickFreeLine(PoolClient& p) {
        for (uint8_t k = 0; k < MAX_LINES; ++k) {
            uint8_t i = (uint8_t)((rrCursor_ + k) % MAX_LINES);
            const Line& L = lines_[i];
            if (!L.used || !L.online || L.agentBusy || L.relayBusy) continue;
            if (p.triedMask[i >> 5] & (1u << (i & 31))) continue;
            rrCursor_ = (uint8_t)((i + 1) % MAX_LINES);
            p.curLine = (int)i;
            lines_[i].relayBusy = true;        // линия закреплена за вызовом
            return (int)i;
        }
        p.curLine = -1;
        return -1;
    }

    static void markTried(PoolClient& p, int line) {
        if (line >= 0) p.triedMask[(uint8_t)line >> 5] |= 1u << ((uint8_t)line & 31);
    }

    static void accumulateDial(PoolClient& p, const uint8_t* data, int n) {
        for (int i = 0; i < n; ++i) {
            if (p.rxLen >= sizeof(p.rxBuf) - 1) {   // хвост ответа сохраняем
                uint16_t half = (uint16_t)(p.rxLen / 2);
                memmove(p.rxBuf, p.rxBuf + half, p.rxLen - half);
                p.rxLen = (uint16_t)(p.rxLen - half);
            }
            p.rxBuf[p.rxLen++] = (char)data[i];
            p.rxBuf[p.rxLen] = '\0';
        }
    }

    void dialFailed(uint8_t idx, const char* why) {
        PoolClient& p = pool_[idx];
        Line& L = lines_[p.curLine];
        logf("линия %s/SIM%u отклонила вызов (%s) — пробуем следующую", L.name, (unsigned)L.sim, why);
        L.relayBusy = false;
        markTried(p, p.curLine);
        p.curLine = -1;
        if (p.box) { p.box->stop(); delete p.box; p.box = nullptr; }
        if (pickFreeLine(p) < 0) { failClient(idx, "ERROR\n"); return; }
        p.phase = PH_CONNECT;                  // следующий тик — попытка на новой линии
    }

    void servicePoolClients(uint32_t now) {
        for (uint8_t idx = 0; idx < MAX_POOL_CLIENTS; ++idx) {
            PoolClient& p = pool_[idx];
            if (!p.cli) continue;

            switch (p.phase) {

            case PH_PHONE: {
                uint8_t tmp[128];
                int n = p.cli->read(tmp, sizeof(tmp));
                if (n < 0) { logf("клиент пула отключился, не прислав номер"); finishClient(idx); break; }
                if (n > 0) {
                    p.lr.feed(tmp, (size_t)n);
                    char line[40];
                    int r;
                    while ((r = p.lr.next(line, sizeof(line))) != 0) {
                        if (r <= 0) continue;
                        if (!gw::isValidPhone(line, r)) {
                            logf("некорректный номер: %s", line);
                            failClient(idx, "ERROR\n");
                            break;
                        }
                        snprintf(p.phone, sizeof(p.phone), "%s", line);
                        logf("номер %s — ищем свободную линию (%u свободно)", p.phone, freeLineCount());
                        if (pickFreeLine(p) < 0) {
                            logf("свободных линий нет — отвечаем BUSY");
                            failClient(idx, "BUSY\n");
                            break;
                        }
                        p.phase = PH_CONNECT;
                        p.tPhase = now;
                        break;
                    }
                    if (p.phase != PH_PHONE || !p.cli) break;
                }
                if (p.cli && !p.cli->connected()) { logf("клиент пула отключился"); finishClient(idx); break; }
                if (p.cli && now - p.tPhase > cfg_.phoneTimeoutMs) {
                    logf("клиент пула не прислал номер за %u мс", (unsigned)cfg_.phoneTimeoutMs);
                    finishClient(idx);
                }
                break;
            }

            case PH_CONNECT: {
                Line& L = lines_[p.curLine];
                logf("попытка: линия %s/SIM%u (%s:%u)", L.name, (unsigned)L.sim, L.ip, L.port);
                p.box = connector_.connectTo(L.ip, L.port, cfg_.connectTimeoutMs);
                if (!p.box) {
                    logf("бокс '%s' недоступен — линия пропущена", L.name);
                    L.relayBusy = false;
                    markTried(p, p.curLine);
                    p.curLine = -1;
                    if (pickFreeLine(p) < 0) { failClient(idx, "ERROR\n"); break; }
                    break;                          // новый тик — попытка на новой линии
                }
                char dial[32];
                int dn = snprintf(dial, sizeof(dial), "%s\n", p.phone);
                p.box->write((const uint8_t*)dial, (size_t)dn);
                p.rxLen = 0;
                p.rxBuf[0] = '\0';
                p.tPhase = now;
                p.phase = PH_DIAL;
                break;
            }

            case PH_DIAL: {
                uint8_t tmp[128];
                int n = p.box->read(tmp, sizeof(tmp));
                if (n < 0) { dialFailed(idx, "обрыв соединения с боксом"); break; }
                if (n > 0) accumulateDial(p, tmp, n);
                uint8_t sink[64];                   // данные до CONNECT не ждём
                while (p.cli->read(sink, sizeof(sink)) > 0) {}
                if (!p.cli->connected()) { logf("клиент пула ушёл во время дозвона"); finishClient(idx); break; }
                if (strstr(p.rxBuf, "CONNECT")) {
                    static const char ok[] = "CONNECT\n";
                    p.cli->write((const uint8_t*)ok, sizeof(ok) - 1);
                    logf("вызов установлен через %s/SIM%u",
                         lines_[p.curLine].name, (unsigned)lines_[p.curLine].sim);
                    p.tActivity = now;
                    p.phase = PH_DATA;
                    break;
                }
                static const char* errs[] = {
                    "NO CARRIER", "BUSY", "NO ANSWER", "NO DIALTONE", "ERROR"
                };
                const char* failed = nullptr;
                for (const char* e : errs) {
                    if (strstr(p.rxBuf, e)) { failed = e; break; }
                }
                if (failed)                    { dialFailed(idx, failed); break; }
                if (now - p.tPhase > cfg_.dialTimeoutMs) { dialFailed(idx, "таймаут дозвона"); break; }
                break;
            }

            case PH_DATA: {
                uint8_t buf[512];
                int n = p.cli->read(buf, sizeof(buf));        // клиент -> бокс
                if (n < 0) { logf("клиент пула отключился — кладём трубку"); finishClient(idx); break; }
                if (n > 0) {
                    if (p.box->write(buf, (size_t)n) < 0) { finishClient(idx); break; }
                    p.tActivity = now;
                }
                n = p.box->read(buf, sizeof(buf));            // бокс -> клиент
                if (n < 0) {
                    logf("бокс '%s' закрыл соединение — вызов завершён",
                         lines_[p.curLine].name);
                    finishClient(idx);
                    break;
                }
                if (n > 0) {
                    if (p.cli->write(buf, (size_t)n) < 0) { finishClient(idx); break; }
                    p.tActivity = now;
                }
                if (!p.cli->connected()) { finishClient(idx); break; }
                if (now - p.tActivity > cfg_.idleTimeoutMs) {
                    logf("простой %u мс — разрываем вызов (%s/SIM%u)",
                         (unsigned)cfg_.idleTimeoutMs,
                         lines_[p.curLine].name, (unsigned)lines_[p.curLine].sim);
                    finishClient(idx);
                }
                break;
            }

            case PH_DONE:
                break;
            }
        }
    }

    // --------------------------- обнаружение -----------------------------
    void serviceDiscovery() {
        if (!udp_) return;
        char buf[64], fromIp[16];
        uint16_t fromPort = 0;
        for (;;) {
            int n = udp_->receive(buf, sizeof(buf), fromIp, sizeof(fromIp), fromPort);
            if (n <= 0) break;
            if (strstr(buf, "GWD1 DISCOVER")) {
                udp_->sendTo(fromIp, fromPort, "GWD1 HERE\n", 10);
                logf("UDP: запрос обнаружения от %s:%u — ответили HERE", fromIp, fromPort);
            }
        }
    }

    // ------------------------------ данные -------------------------------
    IClock&     clk_;
    ILogger&    lg_;
    const FleetConfig& cfg_;
    IListener&  agentListener_;
    IListener&  poolListener_;
    IConnector& connector_;
    IUdp*       udp_;
    ILineState* localLines_;

    Line      lines_[MAX_LINES];
    AgentConn agents_[MAX_AGENTS];
    PoolClient pool_[MAX_POOL_CLIENTS];
    uint8_t   rrCursor_;
};

// -------------------------------- агент ------------------------------------

// Агент есть у каждой коробки (кроме работающей дирижёром — её линии
// регистрируются напрямую). Он сообщает дирижёру имя, адрес, порты своих
// SIM и их занятость, а при потере связи сам восстанавливает её.
class Agent {
public:
    Agent(IClock& clk, ILogger& lg, IConnector& conn, IUdp* udp,
          const FleetConfig& cfg, ILineState& lines)
        : clk_(clk), lg_(lg), conn_(conn), udp_(udp), cfg_(cfg), lines_(lines)
    {
        discoveredIp_[0] = '\0';
        viaDiscovery_ = false;
        discFailures_ = 0;
        cli_ = nullptr;
        ph_ = A_FIND;
        tPhase_ = 0;
        tLastHello_ = 0;
        tLastPing_ = 0;
        tLastDiscover_ = 0;
        sentBusy_[0] = sentBusy_[1] = false;
    }

    // Вызывать раз в ~100 мс из потока агента. connectTo внутри блокирует
    // поток на несколько секунд при недоступном дирижёре — это нормально:
    // агент живёт в отдельном потоке и не мешает шлюзу.
    void service() {
        const uint32_t now = clk_.nowMs();

        switch (ph_) {

        case A_FIND:
            if (cfg_.directorIp[0]) { switchTo(A_CONNECT, now); break; }
            if (!udp_) break;                       // адреса нет и искать нечем
            if (now - tLastDiscover_ >= cfg_.discoverPeriodMs) {
                tLastDiscover_ = now;
                char msg[24];
                int n = snprintf(msg, sizeof(msg), "%s DISCOVER\n", PROTO_TAG);
                udp_->sendTo(cfg_.discoverAddr, cfg_.directorPort, msg, (size_t)n);
                lg_.log("[AGENT] ищем дирижёра: объявление GWD1 DISCOVER");
            }
            {
                char buf[64], fromIp[16];
                uint16_t fromPort = 0;
                int n = udp_->receive(buf, sizeof(buf), fromIp, sizeof(fromIp), fromPort);
                if (n > 0 && strstr(buf, "GWD1 HERE") && validIpv4(fromIp)) {
                    snprintf(discoveredIp_, sizeof(discoveredIp_), "%s", fromIp);
                    viaDiscovery_ = true;
                    discFailures_ = 0;
                    char b[64];
                    snprintf(b, sizeof(b), "[AGENT] дирижёр найден по UDP: %s", discoveredIp_);
                    lg_.log(b);
                    switchTo(A_CONNECT, now);
                }
            }
            break;

        case A_CONNECT: {
            const char* ip = cfg_.directorIp[0] ? cfg_.directorIp : discoveredIp_;
            if (!ip[0]) { switchTo(A_FIND, now); break; }
            cli_ = conn_.connectTo(ip, cfg_.directorPort, 4000);
            if (!cli_) {
                char b[128];
                snprintf(b, sizeof(b), "[AGENT] дирижёр %s:%u недоступен — повтор через %u мс",
                         ip, cfg_.directorPort, (unsigned)cfg_.reconnectMs);
                lg_.log(b);
                // адрес, найденный по UDP, проверяем не бесконечно: после
                // нескольких неудач ищем дирижёра заново
                if (viaDiscovery_ && ++discFailures_ >= 3) {
                    discoveredIp_[0] = '\0';
                    viaDiscovery_ = false;
                }
                switchTo(A_HOLD, now);
                break;
            }
            discFailures_ = 0;
            char b[192];
            snprintf(b, sizeof(b), "[AGENT] связь с дирижёром %s:%u установлена — регистрация '%s' (SIM1 :%u, SIM2 :%u)",
                     ip, cfg_.directorPort, cfg_.selfName, cfg_.selfPort1, cfg_.selfPort2);
            lg_.log(b);
            sendHello();
            tLastHello_ = now;
            tLastPing_ = now;
            sentBusy_[0] = sentBusy_[1] = false;   // после переподключения — полная сводка
            switchTo(A_ONLINE, now);
            break;
        }

        case A_ONLINE: {
            uint8_t tmp[128];
            int n = cli_->read(tmp, sizeof(tmp));
            if (n < 0) {
                lg_.log("[AGENT] связь с дирижёром потеряна");
                dropConnection(now);
                break;
            }
            // (ответов дирижёра агенту не предусмотрено — байты выбрасываем)
            if (now - tLastHello_ >= cfg_.helloPeriodMs) {
                sendHello();
                tLastHello_ = now;
            }
            if (now - tLastPing_ >= cfg_.pingPeriodMs) {
                sendLine("%s PING\n", PROTO_TAG);
                tLastPing_ = now;
            }
            for (uint8_t s = 0; s < 2; ++s) {
                bool busy = lines_.simBusy(s);
                if (busy != sentBusy_[s]) {
                    sentBusy_[s] = busy;
                    sendLine("%s STATE SIM%u %s\n", PROTO_TAG,
                             (unsigned)(s + 1), busy ? "BUSY" : "FREE");
                    char b[48];
                    snprintf(b, sizeof(b), "[AGENT] SIM%u: %s",
                             (unsigned)(s + 1), busy ? "BUSY" : "FREE");
                    lg_.log(b);
                }
            }
            break;
        }

        case A_HOLD:
            if (now - tPhase_ >= cfg_.reconnectMs) {
                switchTo(cfg_.directorIp[0] || discoveredIp_[0] ? A_CONNECT : A_FIND, now);
            }
            break;
        }
    }

private:
    enum Phase : uint8_t { A_FIND, A_CONNECT, A_ONLINE, A_HOLD };

    void switchTo(Phase p, uint32_t now) { ph_ = p; tPhase_ = now; }

    void sendLine(const char* fmt, ...) {
        if (!cli_) return;
        char b[128];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        cli_->write((const uint8_t*)b, strlen(b));
    }

    void sendHello() {
        sendLine("%s HELLO %s %s %u %u\n", PROTO_TAG, cfg_.selfName, cfg_.selfIp,
                 (unsigned)cfg_.selfPort1, (unsigned)cfg_.selfPort2);
    }

    void dropConnection(uint32_t now) {
        if (cli_) { cli_->stop(); delete cli_; cli_ = nullptr; }
        switchTo(A_HOLD, now);
    }

    IClock&        clk_;
    ILogger&       lg_;
    IConnector&    conn_;
    IUdp*          udp_;
    const FleetConfig& cfg_;
    ILineState&    lines_;

    Phase      ph_;
    uint32_t   tPhase_;
    uint32_t   tLastHello_;
    uint32_t   tLastPing_;
    uint32_t   tLastDiscover_;
    bool       sentBusy_[2];
    IClient*   cli_;
    char       discoveredIp_[16];
    bool       viaDiscovery_;
    uint8_t    discFailures_;
};

} // namespace gwd
