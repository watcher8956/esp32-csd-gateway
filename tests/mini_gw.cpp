// mini_gw.cpp — односессионный отладочный шлюз на gw_core.hpp (без потоков).
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <ctime>

#include "gw_core.hpp"

using namespace gw;

struct M : IModem {
    int fd;
    int read(uint8_t* b, size_t l) override {
        int n = ::read(fd, b, l);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        return n;
    }
    int write(const uint8_t* b, size_t l) override {
        return ::write(fd, b, l) < 0 ? -1 : (int)l;
    }
};

struct C : IClient {
    int fd;
    int read(uint8_t* b, size_t l) override {
        int n = ::recv(fd, b, l, MSG_DONTWAIT);
        if (n == 0) return -1;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        return n;
    }
    int write(const uint8_t* b, size_t l) override {
        return ::send(fd, b, l, MSG_NOSIGNAL) < 0 ? -1 : (int)l;
    }
    bool connected() override { return true; }
    void stop() override {}
};

struct CLK : IClock {
    uint32_t nowMs() override {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
};

struct L : ILogger {
    void log(const char* t) override {
        fprintf(stderr, "%s\n", t);
        fflush(stderr);
    }
};

int main(int argc, char** argv) {
    int mfd = dup(atoi(argv[1]));
    // dup наследует флаги, но O_NONBLOCK обязательно ставим явно:
    // иначе ::read() на пустом pty-master блокирует навсегда
    int fl = fcntl(mfd, F_GETFL, 0);
    fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
    struct termios t;
    tcgetattr(mfd, &t);
    cfmakeraw(&t);
    tcsetattr(mfd, TCSANOW, &t);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int o = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &o, sizeof o);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(15500);
    a.sin_addr.s_addr = INADDR_ANY;
    bind(ls, (sockaddr*)&a, sizeof a);
    listen(ls, 1);
    fprintf(stderr, "mini_gw listening 15500\n");
    fflush(stderr);

    int cf = accept(ls, nullptr, nullptr);
    M modem;
    modem.fd = mfd;

    // проверяем чтение прямо тут: читаем всё, что есть, печатаем
    {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(mfd, &rfds);
        struct timeval tv = {0, 300000};
        int r = select(mfd + 1, &rfds, NULL, NULL, &tv);
        char tmp[128] = {0};
        int n = 0;
        if (r > 0 && FD_ISSET(mfd, &rfds)) {
            n = ::read(mfd, tmp, sizeof(tmp) - 1);
        }
        fprintf(stderr, "pre-session read: sel=%d n=%d [%s] errno=%d\n", r, n, tmp, errno);
        fflush(stderr);
    }
    C cl;
    cl.fd = cf;
    CLK clk;
    L lg;
    Session se(modem, cl, clk, lg, "T1");
    while (!se.done()) {
        se.service();
        usleep(10000);
    }
    fprintf(stderr, "mini_gw session done\n");
    return 0;
}
