// ============================================================================
//  linux_gw.cpp — Linux-версия шлюза на том же ядре, что и прошивка ESP32.
//  Существует ради испытательного стенда: вместо модемов — псевдо-терминалы
//  с fake_modem.py, вместо Wi-Fi — обычные TCP-сокеты. Прогнав сценарии на
//  стенде, мы проверяем ровно ту логику, которая полетит на плату.
//
//  Режимы:
//    linux_gw <port1> <modem1|-> <port2> <modem2|->
//        одиночный шлюз: два порта, два модема (старый формат запуска);
//    linux_gw gw <port1> <modem1|-> <port2> <modem2|-> <имя> <дирижёр> <порт>
//        бокс парка: те же два порта + агент координации. <дирижёр>:
//        IP-адрес дирижёра, 'auto' — искать по UDP-объявлению,
//        'no' — агент выключен;
//    linux_gw dir <agentPort> <poolPort> <имя> [p1 m1 p2 m2]
//        дирижёр: принимает агентов на agentPort (там же UDP-объявления),
//        клиентов пула на poolPort; при указанных p1/m1/p2/m2 обслуживает
//        и собственные две SIM (как настоящая коробка-дирижёр).
//
//  Общие флаги: --idle-ms N (таймаут простоя), --agent-timeout-ms N
//  (сколько ждать весточек от агента), --discover-addr IP (куда слать
//  UDP-объявление; по умолчанию 255.255.255.255), --self-ip IP.
//
//  Сборка:  g++ -O2 -std=c++17 -pthread linux_gw.cpp -o linux_gw
// ============================================================================

#include "gw_core.hpp"
#include "gw_director.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <csignal>

using namespace gw;

// --------------------------- реализация интерфейсов ------------------------

struct PtyModem : IModem {
    int fd = -1;
    // Путь вида "fd:N" — унаследовать уже открытый pty-master (так тест
    // передаёт сюда конец псевдо-терминала); иначе открыть путь обычным open.
    explicit PtyModem(const char* path) {
        if (strncmp(path, "fd:", 3) == 0) {
            fd = dup(atoi(path + 3));
        } else {
            fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        }
        // dup() мог принести блокирующий fd — read() на пустом pty повесил бы
        // поток навсегда, поэтому O_NONBLOCK ставим явно
        if (fd >= 0) {
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        }
        // raw-режим, как у UART с SIM800L: без эха, без перевода \r<->\n
        if (fd >= 0) {
            struct termios t;
            if (tcgetattr(fd, &t) == 0) {
                cfmakeraw(&t);
                cfsetispeed(&t, B9600);
                cfsetospeed(&t, B9600);
                tcsetattr(fd, TCSANOW, &t);
            }
        }
    }
    ~PtyModem() override { if (fd >= 0) close(fd); }
    int read(uint8_t* buf, size_t len) override {
        int n = ::read(fd, buf, len);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        if (n <= 0) return -1;           // 0 или EIO: модем пропал
        return n;
    }
    int write(const uint8_t* buf, size_t len) override {
        ssize_t off = 0;
        while (off < (ssize_t)len) {
            ssize_t n = ::write(fd, buf + off, len - off);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) { usleep(1000); continue; }
                return -1;
            }
            off += n;
        }
        return (int)len;
    }
};

struct SockClient : IClient {
    int fd = -1;
    bool broken = false;
    SockClient() = default;
    explicit SockClient(int f) : fd(f) {}
    ~SockClient() override { if (fd >= 0) close(fd); }
    int read(uint8_t* buf, size_t len) override {
        if (broken) return -1;
        int n = ::recv(fd, buf, len, MSG_DONTWAIT);
        if (n == 0) { broken = true; return -1; }     // EOF — клиент закрылся
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        if (n < 0) { broken = true; return -1; }
        return n;
    }
    int write(const uint8_t* buf, size_t len) override {
        if (broken) return -1;
        ssize_t off = 0;
        while (off < (ssize_t)len) {
            ssize_t n = ::send(fd, buf + off, len - off, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) { usleep(1000); continue; }
                broken = true;
                return -1;
            }
            off += n;
        }
        return (int)len;
    }
    bool connected() override { return !broken && fd >= 0; }
    void stop() override { if (fd >= 0) { ::shutdown(fd, SHUT_RDWR); close(fd); fd = -1; } }
};

struct SockListener : gwd::IListener {
    int fd = -1;
    explicit SockListener(int f) : fd(f) {}
    gwd::IClient* acceptClient() override {
        sockaddr_in cli {};
        socklen_t cl = sizeof(cli);
        int cfd = ::accept(fd, (sockaddr*)&cli, &cl);
        if (cfd < 0) return nullptr;                 // EAGAIN: подключений нет
        return new SockClient(cfd);
    }
};

struct SockConnector : gwd::IConnector {
    gwd::IClient* connectTo(const char* ip, uint16_t port, uint32_t timeoutMs) override {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        int rc = ::connect(fd, (sockaddr*)&a, sizeof(a));
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd p {};
            p.fd = fd;
            p.events = POLLOUT;
            int pr = poll(&p, 1, (int)timeoutMs);
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (pr <= 0 || soerr != 0) { close(fd); return nullptr; }
        } else if (rc < 0) {
            close(fd);
            return nullptr;
        }
        return new SockClient(fd);                   // fd остаётся неблокирующим
    }
};

struct UdpSock : gwd::IUdp {
    int fd = -1;
    explicit UdpSock(uint16_t bindPort) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return;
        int broadcast = 1, reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(bindPort);
        a.sin_addr.s_addr = INADDR_ANY;
        if (::bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { close(fd); fd = -1; }
    }
    ~UdpSock() override { if (fd >= 0) close(fd); }
    bool sendTo(const char* ip, uint16_t port, const char* data, size_t len) override {
        if (fd < 0) return false;
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip, &a.sin_addr);
        return ::sendto(fd, data, len, MSG_NOSIGNAL, (sockaddr*)&a, sizeof(a)) == (ssize_t)len;
    }
    int receive(char* buf, size_t cap, char* fromIp, size_t ipCap, uint16_t& fromPort) override {
        if (fd < 0) return 0;
        sockaddr_in src {};
        socklen_t sl = sizeof(src);
        ssize_t n = ::recvfrom(fd, buf, cap - 1, MSG_DONTWAIT, (sockaddr*)&src, &sl);
        if (n <= 0) return 0;
        buf[n] = '\0';
        char ipbuf[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
        snprintf(fromIp, ipCap, "%s", ipbuf);
        fromPort = ntohs(src.sin_port);
        return (int)n;
    }
};

struct RealClock : IClock {
    uint32_t nowMs() override {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
};

struct StdoutLog : ILogger {
    void log(const char* text) override {
        fprintf(stderr, "%s\n", text);   // stderr: не смешивается с протоколом
        fflush(stderr);
    }
};

// ------------------------ занятость линий этой коробки ----------------------

static volatile bool g_simBusy[2] = { false, false };

struct LocalLines : gwd::ILineState {
    bool simBusy(uint8_t idx) override { return g_simBusy[idx]; }
};

static LocalLines g_localLines;

// --------------------------- поток одного порта ----------------------------

struct PortArgs {
    int         listen_fd = -1;
    const char* pty       = "-";
    const char* tag       = "?";
    int         simIndex  = -1;    // >= 0: обновлять g_simBusy (режимы gw/dir)
    uint32_t    idleMs    = 0;     // переопределение таймаута простоя
};

static void port_thread(PortArgs a) {
    RealClock clock;
    StdoutLog log;
    Config cfg;
    if (a.idleMs) cfg.idleTimeoutMs = a.idleMs;

    const bool blockingAccept = (a.simIndex < 0);  // одиночный режим: блокирующий accept

    for (;;) {
        sockaddr_in cli {};
        socklen_t cl = sizeof(cli);
        int cfd = accept(a.listen_fd, (sockaddr*)&cli, &cl);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            if (blockingAccept) break;
            usleep(20000);               // неблокирующий accept: подключений нет
            continue;
        }

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof(ip));
        fprintf(stderr, "[%s] клиент %s подключён\n", a.tag, ip);

        // Пустой pty (модема нет) — порт вежливо отвечает ERROR и живёт дальше
        std::unique_ptr<PtyModem> modem;
        if (strcmp(a.pty, "-") != 0) modem.reset(new PtyModem(a.pty));
        if (!modem || modem->fd < 0) {
            fprintf(stderr, "[%s] НЕТ МОДЕМА (%s) — отвечаем ERROR\n", a.tag, a.pty);
            static const char e[] = "ERROR\n";
            (void)!::send(cfd, e, sizeof(e) - 1, MSG_NOSIGNAL);
            ::shutdown(cfd, SHUT_RDWR);
            close(cfd);
            continue;
        }

        SockClient client(cfd);

        if (a.simIndex >= 0) g_simBusy[a.simIndex] = true;
        Session session(*modem, client, clock, log, a.tag, cfg);
        while (!session.done()) {
            session.service();
            usleep(10000);              // те же ~10 мс, что и vTaskDelay(10) на ESP32
        }
        client.stop();
        if (a.simIndex >= 0) g_simBusy[a.simIndex] = false;
        fprintf(stderr, "[%s] сессия закрыта, ждём следующего клиента\n", a.tag);
    }
}

// ------------------------------ утилиты ------------------------------------

static int make_listener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); exit(1); }
    listen(fd, 16);
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);       // acceptClient() не должен блокировать
    return fd;
}

static uint32_t flag_uint(int argc, char** argv, const char* name, uint32_t def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) return (uint32_t)strtoul(argv[i + 1], nullptr, 10);
    }
    return def;
}

static const char* flag_str(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

// ----------------------------- режим gw (бокс парка) ------------------------

static int box_main(int argc, char** argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: %s gw <port1> <modem1|-> <port2> <modem2|-> <name> <dirIp|auto|no> <dirPort>\n", argv[0]);
        return 1;
    }
    const uint16_t port1 = (uint16_t)atoi(argv[2]);
    const char*    pty1  = argv[3];
    const uint16_t port2 = (uint16_t)atoi(argv[4]);
    const char*    pty2  = argv[5];
    const char*    name  = argv[6];
    const char*    dir   = argv[7];
    const uint16_t dirPort = (uint16_t)atoi(argv[8]);

    const uint32_t idleMs       = flag_uint(argc, argv, "--idle-ms", 0);
    const uint32_t agentTimeout = flag_uint(argc, argv, "--agent-timeout-ms", 0);
    const char*    discAddr     = flag_str(argc, argv, "--discover-addr", nullptr);
    const char*    selfIp       = flag_str(argc, argv, "--self-ip", "127.0.0.1");

    gwd::FleetConfig fcfg;
    snprintf(fcfg.selfName, sizeof(fcfg.selfName), "%s", name);
    snprintf(fcfg.selfIp, sizeof(fcfg.selfIp), "%s", selfIp);
    fcfg.selfPort1 = port1;
    fcfg.selfPort2 = port2;
    fcfg.directorPort = dirPort;
    if (idleMs)       fcfg.idleTimeoutMs = idleMs;
    if (agentTimeout) fcfg.agentTimeoutMs = agentTimeout;
    if (discAddr)     snprintf(fcfg.discoverAddr, sizeof(fcfg.discoverAddr), "%s", discAddr);
    if (strcmp(dir, "no") == 0) {
        // агент выключен: коробка работает сама по себе
    } else if (strcmp(dir, "auto") == 0) {
        fcfg.directorIp[0] = '\0';               // автопоиск по UDP-объявлению
    } else {
        snprintf(fcfg.directorIp, sizeof(fcfg.directorIp), "%s", dir);
        fcfg.autoDiscover = false;
    }

    PortArgs a1 { make_listener(port1), pty1, "SIM1", 0, idleMs };
    PortArgs a2 { make_listener(port2), pty2, "SIM2", 1, idleMs };
    std::thread t1(port_thread, a1);
    std::thread t2(port_thread, a2);

    if (strcmp(dir, "no") != 0) {
        std::thread agent([fcfg]() {
            RealClock clock;
            StdoutLog log;
            SockConnector connector;
            UdpSock udp(0);                      // эфемерный порт: приём ответов HERE
            gwd::Agent agentImpl(clock, log, connector, &udp, fcfg, g_localLines);
            for (;;) {
                agentImpl.service();
                usleep(100000);                  // те же ~100 мс, что и на ESP32
            }
        });
        agent.detach();
    }

    t1.join();
    t2.join();
    return 0;
}

// --------------------------- режим dir (дирижёр) ----------------------------

static int director_main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s dir <agentPort> <poolPort> <имя> [p1 m1 p2 m2] [--idle-ms N] [--agent-timeout-ms N] [--discover-addr IP]\n", argv[0]);
        return 1;
    }
    const uint16_t agentPort = (uint16_t)atoi(argv[2]);
    const uint16_t poolPort  = (uint16_t)atoi(argv[3]);
    const char*    name      = argv[4];

    bool haveSelf = (argc >= 9 && strcmp(argv[5], "--") != 0);
    const uint16_t selfPort1 = haveSelf ? (uint16_t)atoi(argv[5]) : 0;
    const char*    selfM1    = haveSelf ? argv[6] : "-";
    const uint16_t selfPort2 = haveSelf ? (uint16_t)atoi(argv[7]) : 0;
    const char*    selfM2    = haveSelf ? argv[8] : "-";

    const uint32_t idleMs       = flag_uint(argc, argv, "--idle-ms", 0);
    const uint32_t agentTimeout = flag_uint(argc, argv, "--agent-timeout-ms", 0);
    const char*    discAddr     = flag_str(argc, argv, "--discover-addr", nullptr);

    gwd::FleetConfig fcfg;
    snprintf(fcfg.selfName, sizeof(fcfg.selfName), "%s", name);
    snprintf(fcfg.selfIp, sizeof(fcfg.selfIp), "127.0.0.1");
    fcfg.selfPort1 = selfPort1 ? selfPort1 : 5000;
    fcfg.selfPort2 = selfPort2 ? selfPort2 : 5001;
    fcfg.directorPort = agentPort;
    fcfg.poolPort = poolPort;
    if (idleMs)       fcfg.idleTimeoutMs = idleMs;
    if (agentTimeout) fcfg.agentTimeoutMs = agentTimeout;
    if (discAddr)     snprintf(fcfg.discoverAddr, sizeof(fcfg.discoverAddr), "%s", discAddr);

    SockListener agentListener(make_listener(agentPort));
    SockListener poolListener(make_listener(poolPort));

    if (haveSelf) {
        PortArgs a1 { make_listener(selfPort1), selfM1, "SIM1", 0, idleMs };
        PortArgs a2 { make_listener(selfPort2), selfM2, "SIM2", 1, idleMs };
        std::thread(port_thread, a1).detach();
        std::thread(port_thread, a2).detach();
    }

    std::thread director([fcfg, &agentListener, &poolListener, haveSelf, selfPort1, selfPort2]() {
        RealClock clock;
        StdoutLog log;
        SockConnector connector;
        UdpSock udp(fcfg.directorPort);
        gwd::Director directorImpl(clock, log, fcfg, agentListener, poolListener,
                                   connector, &udp, &g_localLines);
        if (haveSelf) {
            directorImpl.addLocalLine(fcfg.selfName, "127.0.0.1", selfPort1, selfPort2);
        }
        fprintf(stderr, "[DIR] дирижёр запущен: агентский порт %u, порт пула %u\n",
                fcfg.directorPort, fcfg.poolPort);
        for (;;) {
            directorImpl.service();
            usleep(10000);
        }
    });
    director.join();
    return 0;
}

// ------------------------- одиночный режим (старый) -------------------------

static int legacy_main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <port1> <modem1|fd:N> <port2> <modem2|fd:N|->\n", argv[0]);
        fprintf(stderr, "  либо: %s gw ... / %s dir ... (режимы парка)\n", argv[0], argv[0]);
        return 1;
    }
    int ports[2] = { atoi(argv[1]), atoi(argv[3]) };
    const char* ptys[2] = { argv[2], argv[4] };

    for (int i = 0; i < 2; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in a {};
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)ports[i]);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(fd, (sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
        listen(fd, 8);
        fprintf(stderr, "[P%d] TCP-сервер на порту %d (модем: %s)\n", i + 1, ports[i], ptys[i]);
        PortArgs args;
        args.listen_fd = fd;                     // блокирующий accept (simIndex < 0)
        args.pty = ptys[i];
        args.tag = i == 0 ? "P1" : "P2";
        std::thread(port_thread, args).detach();
    }
    for (;;) pause();
    return 0;
}

// --------------------------------- main -------------------------------------

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc >= 2 && strcmp(argv[1], "gw") == 0)  return box_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "dir") == 0) return director_main(argc, argv);
    return legacy_main(argc, argv);
}
