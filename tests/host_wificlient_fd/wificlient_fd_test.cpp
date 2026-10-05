// Host test for the accepted-socket ownership in libraries/WiFi/src (WiFiClient.cpp,
// WiFiServer.cpp). Build and run with run.sh in this directory.
//
// Checks:
//   - WiFiClient(fd) closes fd, exactly once, when any allocation in it throws
//     std::bad_alloc (scalar operator new is made to fail on demand below); before,
//     a failure to allocate the socket handle left fd open for good, since the server
//     had already let go of it and no handle existed yet to close it;
//   - a client built normally keeps fd open and closes it once when the last copy goes;
//   - WiFiServer::available() closes an accepted socket it could not configure
//     (setsockopt failing), where it used to leave it open.
#include "WiFiClient.h"
#include "WiFiServer.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <new>
#include <thread>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            ++failures;                                                     \
            std::printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond);    \
            std::printf(__VA_ARGS__);                                       \
            std::printf("\n");                                              \
        }                                                                   \
    } while (0)

unsigned long millis() {
    using namespace std::chrono;
    return (unsigned long)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ---- Seams --------------------------------------------------------------------------

static std::map<int, int>& closes() {
    static std::map<int, int>* m = new std::map<int, int>();
    return *m;
}
int test_close(int fd) {
    ++closes()[fd];
    return ::close(fd);
}

static int fail_setsockopt_name = -1;  // a setsockopt of this option name fails once
int test_setsockopt(int fd, int level, int name, const void* value, socklen_t len) {
    if (name == fail_setsockopt_name) {
        fail_setsockopt_name = -1;
        errno = ENOMEM;
        return -1;
    }
    return ::setsockopt(fd, level, name, value, len);
}

// Scalar operator new: the n-th call from arming throws std::bad_alloc.
static int news        = 0;
static int fail_new_at = 0;  // 0: never
void* operator new(size_t n) {
    if (fail_new_at != 0 && ++news == fail_new_at) {
        fail_new_at = 0;
        throw std::bad_alloc();
    }
    void* p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { free(p); }
void operator delete(void* p, size_t) noexcept { free(p); }

static bool fd_open(int fd) { return fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

// ---- WiFiClient(fd) -------------------------------------------------------------------

// Allocations in WiFiClient(fd): the socket handle (1), its shared_ptr control block (2),
// the receive buffer (3), its control block (4).
static void client_ctor_alloc_failure_closes_fd(int nth) {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    int fd = sv[0];
    closes().erase(fd);
    bool threw = false;
    news        = 0;
    fail_new_at = nth;
    try {
        WiFiClient c(fd);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_new_at = 0;
    CHECK(threw, "allocation %d: the failure reaches the caller", nth);
    CHECK(!fd_open(fd), "allocation %d: the accepted fd is closed (leaked before)", nth);
    CHECK(closes()[fd] == 1, "allocation %d: closed exactly once (%d)", nth, closes()[fd]);
    ::close(sv[1]);
}

static void client_normal_keeps_then_closes_fd() {
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    int fd = sv[0];
    closes().erase(fd);
    {
        WiFiClient c(fd);
        WiFiClient copy = c;
        CHECK(fd_open(fd), "a built client keeps its fd open");
        CHECK(closes()[fd] == 0, "and has not closed it");
        CHECK(::write(sv[1], "x", 1) == 1, "peer write");
        CHECK(c.available() == 1 && c.read() == 'x', "the client reads through it");
    }
    CHECK(!fd_open(fd), "the last copy closes the fd");
    CHECK(closes()[fd] == 1, "exactly once (%d)", closes()[fd]);
    ::close(sv[1]);
}

// ---- WiFiServer::available() ----------------------------------------------------------

static void server_closes_socket_it_cannot_configure() {
    const uint16_t port = (uint16_t)(42000 + getpid() % 2000);
    WiFiServer     server(IPAddress(127, 0, 0, 1), port);  // loopback only
    server.begin();
    CHECK((bool)server, "listening on 127.0.0.1:%u", port);
    if (!server) return;

    int peer = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(peer, (sockaddr*)&a, sizeof(a)) == 0, "connect (errno %d)", errno);

    fail_setsockopt_name = TCP_NODELAY;
    WiFiClient got;
    for (int i = 0; i < 200 && fail_setsockopt_name != -1; ++i) {  // until accept() returns it
        got = server.available();
        if (fail_setsockopt_name != -1) delay(5);
    }
    CHECK(fail_setsockopt_name == -1, "the connection was accepted");
    CHECK(!got.connected(), "no client for a socket that could not be configured");

    // The peer sees the server side close: recv returns 0 (EOF) instead of timing out.
    timeval tv{2, 0};
    ::setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char    b;
    ssize_t r = recv(peer, &b, 1, 0);
    CHECK(r == 0, "the accepted socket was closed (recv %zd, errno %d; open before)", r, r < 0 ? errno : 0);
    ::close(peer);
}

static void server_normal_accept_still_works() {
    const uint16_t port = (uint16_t)(44000 + getpid() % 2000);
    WiFiServer     server(IPAddress(127, 0, 0, 1), port);
    server.begin();
    CHECK((bool)server, "listening on 127.0.0.1:%u", port);
    if (!server) return;
    int peer = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_port        = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(peer, (sockaddr*)&a, sizeof(a)) == 0, "connect (errno %d)", errno);
    WiFiClient got;
    for (int i = 0; i < 200 && !got.connected(); ++i) {
        got = server.available();
        if (!got.connected()) delay(5);
    }
    CHECK(got.connected(), "a normal connection becomes a client");
    CHECK(::write(peer, "y", 1) == 1, "peer write");
    delay(20);
    CHECK(got.available() == 1 && got.read() == 'y', "and reads");
    ::close(peer);
}

int main() {
    signal(SIGPIPE, SIG_IGN);
    for (int nth = 1; nth <= 4; ++nth) client_ctor_alloc_failure_closes_fd(nth);
    client_normal_keeps_then_closes_fd();
    server_closes_socket_it_cannot_configure();
    server_normal_accept_still_works();
    if (failures) {
        std::printf("%d FAILED\n", failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
