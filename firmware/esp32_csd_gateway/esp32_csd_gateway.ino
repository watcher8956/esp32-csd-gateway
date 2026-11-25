// ============================================================================
//  ESP32 CSD Gateway — TCP <-> GSM CSD-шлюз на двух модемах + координация
//  парка коробок (gw_director.hpp).
//
//  Один прошитый бокс — это два независимых CSD-шлюза:
//    порт 5000 -> модем 1 (UART1, SIM1), порт 5001 -> модем 2 (UART2, SIM2).
//  Клиент присылает первой строкой номер телефона, шлюз набирает его в
//  режиме данных (ATD без ';') и прозрачно ретранслирует байты TCP <-> модем
//  до обрыва любой стороны (подробности — gw_core.hpp).
//
//  Разводка (DevKit V1):
//    UART0 = USB (GPIO1/3)                  — журнал и консоль, 115200
//    UART1 = модем 1: RX=GPIO25, TX=GPIO26  — TCP-порт 5000
//    UART2 = модем 2: RX=GPIO16, TX=GPIO17  — TCP-порт 5001
//
//  Парк коробок (опционально, gw_director.hpp): коробка-«дирижёр» принимает
//  клиентов на одном порту пула и сама находит свободную SIM среди всех
//  коробок; остальные коробки-«агенты» сообщают дирижёру занятость своих SIM.
//  Роли включаются параметрами, у каждой коробки — до двух линий, в парке
//  до 16 коробок. Вызов без данных разрывается через idle_timeout (180 с по
//  умолчанию) на любом участке пути.
//
//  Все настройки — через Serial-консоль и NVS (gw_params.hpp), без
//  перекомпиляции: 'help' показывает команды, 'show' — текущие значения.
//  Ключевые параметры: wifi_ssid/wifi_pass, wifi_type, port_sim1/2,
//  modem1_type/modem2_type (профили поведения разных поколений модулей),
//  idle_timeout, director_en/director_ip/director_auto.
// ============================================================================

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HardwareSerial.h>
#include <esp_wifi.h>

#include "gw_core.hpp"
#include "gw_director.hpp"
#include "gw_params.hpp"

// ------------------------------ железо --------------------------------------

static const uint32_t MODEM_BAUD = 9600;

// UART1 = модем 1 (SIM1) -> порт 5000; UART2 = модем 2 (SIM2) -> порт 5001
static const int MODEM1_RX_PIN = 25;
static const int MODEM1_TX_PIN = 26;
static const int MODEM2_RX_PIN = 16;
static const int MODEM2_TX_PIN = 17;

static HardwareSerial modemSerial1(1);
static HardwareSerial modemSerial2(2);

// --------------------------- платформенные адаптеры -------------------------

struct SerialModem : gw::IModem {
    HardwareSerial* ser;
    explicit SerialModem(HardwareSerial* s) : ser(s) {}
    int read(uint8_t* buf, size_t len) override {
        size_t n = ser->read(buf, len);        // неблокирующий; 0 = нет данных
        return (int)n;
    }
    int write(const uint8_t* buf, size_t len) override {
        return (int)ser->write(buf, len) == (int)len ? (int)len : -1;
    }
};

struct WifiClientAdapter : gw::IClient {
    WiFiClient cli;
    bool broken = false;
    int read(uint8_t* buf, size_t len) override {
        if (broken) return -1;
        int n = cli.read(buf, len);            // 0 = нет данных, -1 = обрыв
        if (n < 0) { broken = true; return -1; }
        return n;
    }
    int write(const uint8_t* buf, size_t len) override {
        if (broken) return -1;
        size_t off = 0;
        while (off < len) {
            size_t n = cli.write(buf + off, len - off);
            if (n == 0) { broken = true; return -1; }
            off += n;
        }
        return (int)len;
    }
    bool connected() override { return !broken && cli.connected(); }
    void stop() override { cli.stop(); broken = true; }
};

struct MillisClock : gw::IClock {
    uint32_t nowMs() override { return millis(); }
};

struct SerialLog : gw::ILogger {
    void log(const char* text) override { Serial.println(text); }
};

struct WifiListener : gwd::IListener {
    WiFiServer* server;
    explicit WifiListener(WiFiServer* s) : server(s) {}
    gwd::IClient* acceptClient() override {
        WiFiClient cli = server->accept();
        if (!cli) return nullptr;
        WifiClientAdapter* adapter = new WifiClientAdapter();
        adapter->cli = cli;
        return adapter;
    }
};

struct WifiConnector : gwd::IConnector {
    gwd::IClient* connectTo(const char* ip, uint16_t port, uint32_t timeoutMs) override {
        (void)timeoutMs;                       // лимит устанавливает драйвер WiFiClient
        IPAddress addr;
        if (!addr.fromString(ip)) return nullptr;
        WiFiClient cli;
        if (!cli.connect(addr, port)) return nullptr;
        WifiClientAdapter* adapter = new WifiClientAdapter();
        adapter->cli = cli;
        return adapter;
    }
};

struct WifiUdp : gwd::IUdp {
    WiFiUDP udp;
    bool started = false;
    void begin(uint16_t port) { started = udp.begin(port) == 1; }
    bool sendTo(const char* ip, uint16_t port, const char* data, size_t len) override {
        if (!started) return false;
        IPAddress addr;
        if (!addr.fromString(ip)) return false;
        udp.beginPacket(addr, port);
        udp.write((const uint8_t*)data, len);
        return udp.endPacket() == 1;
    }
    int receive(char* buf, size_t cap, char* fromIp, size_t ipCap, uint16_t& fromPort) override {
        if (!started) return 0;
        int packet = udp.parsePacket();
        if (packet <= 0) return 0;
        int n = udp.read((uint8_t*)buf, cap - 1);
        if (n < 0) n = 0;
        buf[n] = '\0';
        snprintf(fromIp, ipCap, "%s", udp.remoteIP().toString().c_str());
        fromPort = udp.remotePort();
        return n;
    }
};

// ------------------------- занятость линий этой коробки ---------------------

// Заполняется задачами портов, читается задачами агента/дирижёра. Запись
// однословная, поэтому volatile достаточно (без блокировок).
static volatile bool g_simBusy[2] = { false, false };

struct LocalLineState : gwd::ILineState {
    bool simBusy(uint8_t idx) override { return g_simBusy[idx]; }
};

static LocalLineState g_localLines;

// ------------------------------ задача порта --------------------------------

struct PortCtx {
    uint16_t        port;
    HardwareSerial* modemSerial;
    const char*     tag;
    uint8_t         simIndex;    // 0 = SIM1, 1 = SIM2
    uint8_t         modemType;   // профиль gw::ModemProfile
};

static void gatewayTask(void* arg) {
    PortCtx* ctx = (PortCtx*)arg;
    WiFiServer server(ctx->port);
    server.begin();
    Serial.printf("[%s] TCP-сервер на порту %u\n", ctx->tag, ctx->port);

    SerialModem modem(ctx->modemSerial);
    MillisClock clock;
    SerialLog logger;

    gw::Config cfg = gw::configForModem(ctx->modemType);
    cfg.idleTimeoutMs = gp::params.idleTimeoutMs;

    for (;;) {
        WiFiClient cli = server.accept();
        if (cli) {
            g_simBusy[ctx->simIndex] = true;   // линия занята с момента приёма клиента

            WifiClientAdapter adapter;
            adapter.cli = cli;

            gw::Session session(modem, adapter, clock, logger, ctx->tag, cfg);
            while (!session.done()) {
                session.service();
                vTaskDelay(pdMS_TO_TICKS(10)); // мягкое ожидание: уступаем CPU соседям
            }
            adapter.stop();
            g_simBusy[ctx->simIndex] = false;
            Serial.printf("[%s] линия свободна, ждём следующего клиента\n", ctx->tag);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ------------------------- задачи координации парка -------------------------

static void defaultBoxName(char* out, size_t cap) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(out, cap, "gw-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

static gwd::FleetConfig buildFleetConfig() {
    gwd::FleetConfig f;
    if (gp::params.boxName[0]) {
        snprintf(f.selfName, sizeof(f.selfName), "%s", gp::params.boxName);
    } else {
        defaultBoxName(f.selfName, sizeof(f.selfName));
    }
    snprintf(f.selfIp, sizeof(f.selfIp), "%s", WiFi.localIP().toString().c_str());
    f.selfPort1     = gp::params.portSim1;
    f.selfPort2     = gp::params.portSim2;
    f.directorPort  = gp::params.directorPort;
    f.poolPort      = gp::params.poolPort;
    snprintf(f.directorIp, sizeof(f.directorIp), "%s", gp::params.directorIp);
    f.autoDiscover  = gp::params.directorAuto != 0;
    f.idleTimeoutMs = gp::params.idleTimeoutMs;
    return f;
}

// Дирижёр: агентские соединения + UDP-объявления на directorPort,
// клиенты пула на poolPort. Собственные две линии регистрируются напрямую;
// адрес 127.0.0.1 — TCP-петля lwIP до собственных портов шлюза.
static void directorTask(void* arg) {
    (void)arg;
    const gwd::FleetConfig fcfg = buildFleetConfig();

    static WiFiServer agentServer(fcfg.directorPort);
    static WiFiServer poolServer(fcfg.poolPort);
    agentServer.begin();
    poolServer.begin();

    static WifiListener agentListener(&agentServer);
    static WifiListener poolListener(&poolServer);
    static WifiConnector connector;
    static WifiUdp udp;
    udp.begin(fcfg.directorPort);

    MillisClock clock;
    SerialLog logger;

    static gwd::Director director(clock, logger, fcfg, agentListener, poolListener,
                                  connector, &udp, &g_localLines);
    director.addLocalLine(fcfg.selfName, "127.0.0.1", fcfg.selfPort1, fcfg.selfPort2);

    Serial.printf("[DIR] дирижёр запущен: агентский порт %u, порт пула %u\n",
                  fcfg.directorPort, fcfg.poolPort);

    for (;;) {
        director.service();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Агент: поддерживает связь с дирижёром и сообщает занятость своих SIM.
static void agentTask(void* arg) {
    (void)arg;
    static gwd::FleetConfig fcfg = buildFleetConfig();
    static WifiConnector connector;
    static WifiUdp udp;
    udp.begin(fcfg.directorPort);

    MillisClock clock;
    SerialLog logger;

    static gwd::Agent agent(clock, logger, connector, &udp, fcfg, g_localLines);

    Serial.printf("[AGENT] агент парка запущен (имя '%s', дирижёр %s:%u%s)\n",
                  fcfg.selfName, fcfg.directorIp[0] ? fcfg.directorIp : "автопоиск",
                  fcfg.directorPort, fcfg.directorIp[0] ? "" : " по UDP");

    for (;;) {
        // адрес обновляем постоянно: DHCP может сменить его после переподключения
        snprintf(fcfg.selfIp, sizeof(fcfg.selfIp), "%s", WiFi.localIP().toString().c_str());
        agent.service();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ------------------------------- Wi-Fi --------------------------------------

static void onWifiDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
    (void)event;
    Serial.printf("[WiFi] отключение (причина %d) — ожидаем автопереподключение\n",
                  info.wifi_sta_disconnected.reason);
}

// Подключение по параметрам из NVS. Неудача не останавливает коробку:
// настройки правятся через Serial-консоль, затем команда 'wifi'.
static bool connectWifi() {
    if (!gp::params.wifiSsid[0]) {
        Serial.println("[WiFi] wifi_ssid не задан. Настройте через Serial: "
                       "set wifi_ssid <имя>, set wifi_pass <пароль>, затем 'wifi'.");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.onEvent(onWifiDisconnected, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.setAutoReconnect(true);

    if (gp::params.mac[0]) {
        uint8_t mac[6];
        if (gp::parseMacAddress(gp::params.mac, mac)) {
            esp_wifi_set_mac(WIFI_IF_STA, mac);
        } else {
            Serial.println("[WiFi] некорректный mac — используем аппаратный");
        }
    }

    switch (gp::params.wifiType) {
        case gp::WIFI_COMPAT:
            WiFi.setSleep(false);
            esp_wifi_set_protocol(WIFI_IF_STA,
                                  WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
            Serial.println("[WiFi] профиль compat: b/g/n без энергосбережения");
            break;
        case gp::WIFI_STANDARD:
            Serial.println("[WiFi] профиль standard: настройки драйвера по умолчанию");
            break;
        default:
            WiFi.setSleep(false);
            Serial.println("[WiFi] профиль auto: sleep off");
            break;
    }

    const bool staticIp = gp::params.ip[0] != '\0' && strcmp(gp::params.ip, "0.0.0.0") != 0;
    if (staticIp) {
        if (gp::validIPv4(gp::params.ip) && gp::validIPv4(gp::params.gateway) &&
            gp::validIPv4(gp::params.subnet)) {
            IPAddress local, gateway, subnet;
            local.fromString(gp::params.ip);
            gateway.fromString(gp::params.gateway);
            subnet.fromString(gp::params.subnet);
            bool configured = false;
            if (gp::params.dns[0] != '\0' && gp::validIPv4(gp::params.dns)) {
                IPAddress dns;
                dns.fromString(gp::params.dns);
                configured = WiFi.config(local, gateway, subnet, dns);
            } else {
                configured = WiFi.config(local, gateway, subnet);
            }
            if (!configured) Serial.println("[WiFi] WiFi.config вернул false — продолжаем");
        } else {
            Serial.println("[WiFi] для статического IP нужны корректные ip/gateway/subnet — используем DHCP");
        }
    }

    Serial.printf("[WiFi] подключение к '%s'", gp::params.wifiSsid);
    WiFi.begin(gp::params.wifiSsid, gp::params.wifiPass);

    const uint32_t startedAt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 30000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[WiFi] не удалось подключиться за 30 с. Коробка продолжает работу: "
                       "исправьте параметры через Serial и введите 'wifi'.");
        return false;
    }
    Serial.printf("\n[WiFi] подключено. IP: %s, MAC: %s\n",
                  WiFi.localIP().toString().c_str(), WiFi.macAddress().c_str());
    return true;
}

// -------------------------------- setup -------------------------------------

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== ESP32 CSD Gateway: 2 модема / 2 SIM + парк коробок ===");
    Serial.println("Консоль: введите 'help' в Serial Monitor (115200).");

    gp::loadParams();

    modemSerial1.begin(MODEM_BAUD, SERIAL_8N1, MODEM1_RX_PIN, MODEM1_TX_PIN);
    modemSerial2.begin(MODEM_BAUD, SERIAL_8N1, MODEM2_RX_PIN, MODEM2_TX_PIN);
    delay(100);                                // разовый стартовый сброс портов

    connectWifi();

    static PortCtx ctx1 = { gp::params.portSim1, &modemSerial1, "SIM1",
                            0, gp::params.modem1Type };
    static PortCtx ctx2 = { gp::params.portSim2, &modemSerial2, "SIM2",
                            1, gp::params.modem2Type };
    xTaskCreate(gatewayTask, "gw_sim1", 4096, &ctx1, 3, nullptr);
    xTaskCreate(gatewayTask, "gw_sim2", 4096, &ctx2, 3, nullptr);

    if (gp::params.directorEn) {
        if (gp::params.directorIp[0] != '\0') {
            Serial.println("[MAIN] замечание: director_ip задан, но не используется — "
                           "эта коробка сама является дирижёром");
        }
        xTaskCreate(directorTask, "director", 8192, nullptr, 3, nullptr);
        Serial.printf("[MAIN] роль: дирижёр парка '%s'. Линии: SIM1 :%u, SIM2 :%u, "
                      "порт пула %u, агентский порт %u\n",
                      gp::params.boxName[0] ? gp::params.boxName : "<авто из MAC>",
                      gp::params.portSim1, gp::params.portSim2,
                      gp::params.poolPort, gp::params.directorPort);
    } else if (gp::params.directorIp[0] != '\0' || gp::params.directorAuto) {
        xTaskCreate(agentTask, "agent", 4096, nullptr, 3, nullptr);
        Serial.printf("[MAIN] роль: агент парка '%s'. Линии: SIM1 :%u, SIM2 :%u\n",
                      gp::params.boxName[0] ? gp::params.boxName : "<авто из MAC>",
                      gp::params.portSim1, gp::params.portSim2);
    } else {
        Serial.println("[MAIN] роль: автономный шлюз (координация парка выключена)");
    }

    Serial.println("[MAIN] Задачи запущены.");
}

// -------------------------------- loop ---------------------------------------
// Консоль и отложенное сохранение параметров; вся остальная работа — в тасках.

void loop() {
    gp::console.process();
    if (gp::console.wifiRequested()) {
        gp::console.clearWifiRequest();
        connectWifi();
    }
    gp::syncParams();
    vTaskDelay(pdMS_TO_TICKS(20));
}
