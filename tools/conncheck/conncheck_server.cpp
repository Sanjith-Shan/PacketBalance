// conncheck-server: answers every line it receives on a TCP connection with
// "id=<N>\n", where N is the real's number. One thread, epoll, level
// triggered, so one process holds tens of thousands of idle connections.
//
//   conncheck-server --port 7000 --id 3 [--bind 0.0.0.0]
//
// The server binds the wildcard address so it answers connections addressed
// to the VIP (on lo) as well as to the real's own address, which is what DSR
// needs: the client's SYN arrives with destination = VIP.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Conn {
    std::string in;   // bytes received, not yet a full line
    std::string out;  // replies not yet written
};

void usage() {
    std::fprintf(stderr, "usage: conncheck-server --port P --id N [--bind ADDR]\n");
    std::exit(2);
}

void raise_nofile() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

}  // namespace

int main(int argc, char** argv) {
    int port = 7000;
    int id = -1;
    std::string bind_addr = "0.0.0.0";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--port") port = std::atoi(next());
        else if (a == "--id") id = std::atoi(next());
        else if (a == "--bind") bind_addr = next();
        else usage();
    }
    if (id < 0 || port <= 0 || port > 65535) usage();

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    raise_nofile();

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (lfd < 0) { std::perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, bind_addr.c_str(), &sa.sin_addr) != 1) usage();
    if (bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) { std::perror("bind"); return 1; }
    if (listen(lfd, 65535) < 0) { std::perror("listen"); return 1; }

    int ep = epoll_create1(EPOLL_CLOEXEC);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = lfd;
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &ev);

    const std::string reply = "id=" + std::to_string(id) + "\n";
    std::unordered_map<int, Conn> conns;
    std::fprintf(stderr, "conncheck-server: id=%d listening on %s:%d\n", id, bind_addr.c_str(), port);

    auto close_conn = [&](int fd) {
        epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        conns.erase(fd);
    };

    epoll_event events[1024];
    char buf[4096];
    while (!g_stop) {
        int n = epoll_wait(ep, events, 1024, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("epoll_wait");
            return 1;
        }
        for (int k = 0; k < n; ++k) {
            int fd = events[k].data.fd;
            if (fd == lfd) {
                for (;;) {
                    int c = accept4(lfd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (c < 0) break;  // EAGAIN, or EMFILE: retry on next wakeup
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    epoll_event cev{};
                    cev.events = EPOLLIN | EPOLLRDHUP;
                    cev.data.fd = c;
                    epoll_ctl(ep, EPOLL_CTL_ADD, c, &cev);
                    conns.emplace(c, Conn{});
                }
                continue;
            }
            auto it = conns.find(fd);
            if (it == conns.end()) continue;
            Conn& c = it->second;
            bool dead = false;
            if (events[k].events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                for (;;) {
                    ssize_t r = recv(fd, buf, sizeof buf, 0);
                    if (r > 0) { c.in.append(buf, static_cast<size_t>(r)); continue; }
                    if (r == 0) dead = true;
                    else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) dead = true;
                    break;
                }
                // One reply per complete line. A partial line waits for more bytes.
                size_t pos;
                while ((pos = c.in.find('\n')) != std::string::npos) {
                    c.in.erase(0, pos + 1);
                    c.out += reply;
                }
                if (c.in.size() > 65536) dead = true;  // no newline in 64 KB: not a conncheck client
            }
            if (!dead && !c.out.empty()) {
                ssize_t w = send(fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (w > 0) c.out.erase(0, static_cast<size_t>(w));
                else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) dead = true;
                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLRDHUP | (c.out.empty() ? 0u : static_cast<uint32_t>(EPOLLOUT));
                cev.data.fd = fd;
                epoll_ctl(ep, EPOLL_CTL_MOD, fd, &cev);
            } else if (!dead && (events[k].events & EPOLLOUT)) {
                epoll_event cev{};
                cev.events = EPOLLIN | EPOLLRDHUP;
                cev.data.fd = fd;
                epoll_ctl(ep, EPOLL_CTL_MOD, fd, &cev);
            }
            if (dead) close_conn(fd);
        }
    }
    for (auto& [fd, c] : conns) close(fd);
    close(lfd);
    return 0;
}
