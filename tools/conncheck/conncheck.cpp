// conncheck: holds N long-lived TCP connections to a VIP and counts every
// one that breaks. The instrument for Experiments 3 (backend churn) and 4
// (load balancer failover).
//
//   conncheck --vip 198.51.100.1:7000 --connections 10000 --heartbeat-ms 100
//             --duration-s 40 --json out.json [--connect-rate 5000] [--ramp-ms 100]
//             [--timeout-ms 2000] [--connect-timeout-ms 5000] [--quiet]
//
// Each connection sends "hb <seq>\n" every heartbeat interval, stop-and-wait:
// a new heartbeat is only sent once the previous one was answered. The
// server (conncheck-server) answers each line with "id=<N>\n". The first id a
// connection sees is its backend. A connection is BROKEN, and closed, when:
//   rst            recv/send fails with ECONNRESET/EPIPE, or EPOLLERR carries it
//   eof            the peer closed (recv returns 0)
//   timeout        a heartbeat stays unanswered for --timeout-ms, or the kernel
//                  gives up (ETIMEDOUT / EHOSTUNREACH)
//   wrong_backend  a heartbeat is answered by a different id than the first
// Connections that never complete the handshake are counted separately as
// connect_failed, they never held state and so cannot "break".
//
// Connects are paced (--connect-rate per second, issued in batches every
// --ramp-ms) so 10,000 SYNs do not arrive at the load balancer in one burst.
// One thread, epoll. RLIMIT_NOFILE is raised to fit.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

int64_t now_ms() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

enum class State { Idle, Connecting, Open, Broken, ConnectFailed };
enum Cause { kRst = 0, kEof, kTimeout, kWrongBackend, kNumCauses };
const char* kCauseName[kNumCauses] = {"rst", "eof", "timeout", "wrong_backend"};

struct Conn {
    int fd = -1;
    State state = State::Idle;
    int backend = -1;          // first id seen, -1 until the first reply
    int64_t connect_start = 0;
    int64_t hb_sent_at = 0;    // 0 when no heartbeat is outstanding
    int64_t next_hb = 0;
    uint64_t seq = 0;
    std::string in;
};

struct Options {
    std::string vip_host;
    int vip_port = 0;
    int connections = 1000;
    int heartbeat_ms = 100;
    int duration_s = 40;
    int connect_rate = 5000;   // connects per second
    int ramp_ms = 100;         // batch interval for connects
    int timeout_ms = 2000;
    int connect_timeout_ms = 5000;
    std::string json_path;
    bool quiet = false;
};

[[noreturn]] void usage() {
    std::fprintf(stderr,
                 "usage: conncheck --vip ADDR:PORT [--connections N] [--heartbeat-ms MS]\n"
                 "                 [--duration-s S] [--json FILE] [--connect-rate PER_S]\n"
                 "                 [--ramp-ms MS] [--timeout-ms MS] [--connect-timeout-ms MS] [--quiet]\n");
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage();
            return argv[++i];
        };
        if (a == "--vip") {
            std::string v = next();
            auto c = v.rfind(':');
            if (c == std::string::npos) usage();
            o.vip_host = v.substr(0, c);
            o.vip_port = std::atoi(v.c_str() + c + 1);
        } else if (a == "--connections") o.connections = std::atoi(next().c_str());
        else if (a == "--heartbeat-ms") o.heartbeat_ms = std::atoi(next().c_str());
        else if (a == "--duration-s") o.duration_s = std::atoi(next().c_str());
        else if (a == "--connect-rate") o.connect_rate = std::atoi(next().c_str());
        else if (a == "--ramp-ms") o.ramp_ms = std::atoi(next().c_str());
        else if (a == "--timeout-ms") o.timeout_ms = std::atoi(next().c_str());
        else if (a == "--connect-timeout-ms") o.connect_timeout_ms = std::atoi(next().c_str());
        else if (a == "--json") o.json_path = next();
        else if (a == "--quiet") o.quiet = true;
        else usage();
    }
    if (o.vip_host.empty() || o.vip_port <= 0 || o.vip_port > 65535 || o.connections <= 0 ||
        o.heartbeat_ms <= 0 || o.duration_s <= 0 || o.connect_rate <= 0 || o.ramp_ms <= 0 ||
        o.timeout_ms <= 0 || o.connect_timeout_ms <= 0)
        usage();
    return o;
}

void raise_nofile(int need) {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) return;
    rlim_t want = static_cast<rlim_t>(need) + 64;
    if (rl.rlim_cur >= want) return;
    rl.rlim_cur = want;
    if (rl.rlim_max < want) rl.rlim_max = want;  // succeeds as root
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        getrlimit(RLIMIT_NOFILE, &rl);
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
        std::fprintf(stderr, "conncheck: warning: RLIMIT_NOFILE capped at %llu\n",
                     static_cast<unsigned long long>(rl.rlim_cur));
    }
}

class Runner {
public:
    explicit Runner(Options o) : o_(std::move(o)), conns_(static_cast<size_t>(o_.connections)) {
        std::memset(&dst_, 0, sizeof dst_);
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(static_cast<uint16_t>(o_.vip_port));
        if (inet_pton(AF_INET, o_.vip_host.c_str(), &dst_.sin_addr) != 1) usage();
        ep_ = epoll_create1(EPOLL_CLOEXEC);
        timeline_.assign(static_cast<size_t>(o_.duration_s) + 2, std::array<uint64_t, kNumCauses>{});
    }

    int run() {
        t0_ = now_ms();
        const int64_t end = t0_ + static_cast<int64_t>(o_.duration_s) * 1000;
        int64_t next_batch = t0_;
        int64_t batches = 0;
        int64_t next_scan = t0_;
        int64_t next_progress = t0_ + 5000;
        size_t next_idx = 0;
        std::vector<epoll_event> evs(4096);

        while (!g_stop) {
            int64_t now = now_ms();
            if (now >= end) break;

            // Ramp: issue the next batch of connects. Batch k brings the total
            // up to connect_rate * (k + 1) * ramp_ms / 1000, so the average rate
            // is --connect-rate even when one batch would round to zero connects.
            if (next_idx < conns_.size() && now >= next_batch) {
                ++batches;
                const auto due = static_cast<size_t>(std::max<int64_t>(
                    1, static_cast<int64_t>(o_.connect_rate) * batches * o_.ramp_ms / 1000));
                while (next_idx < conns_.size() && next_idx < due) start_connect(next_idx++, now);
                next_batch += o_.ramp_ms;
            }
            // Snapshot per-backend counts once every connect resolved.
            if (!start_snapshot_taken_ && next_idx == conns_.size() && pending_connects_ == 0 && all_have_backend()) {
                take_snapshot(start_counts_);
                start_snapshot_taken_ = true;
                ramp_done_ms_ = now - t0_;
                if (!o_.quiet)
                    std::fprintf(stderr, "conncheck: ramp complete at t=%.1fs established=%llu connect_failed=%llu\n",
                                 ramp_done_ms_ / 1000.0, static_cast<unsigned long long>(established_),
                                 static_cast<unsigned long long>(connect_failed_));
            }

            int timeout = 5;
            int n = epoll_wait(ep_, evs.data(), static_cast<int>(evs.size()), timeout);
            if (n < 0 && errno != EINTR) { std::perror("epoll_wait"); break; }
            now = now_ms();
            for (int k = 0; k < n; ++k) handle_event(static_cast<size_t>(evs[static_cast<size_t>(k)].data.u64), evs[static_cast<size_t>(k)].events, now);

            if (now >= next_scan) {
                scan(now);
                next_scan = now + 5;
            }
            if (!o_.quiet && now >= next_progress) {
                std::fprintf(stderr, "conncheck: t=%2llds open=%llu broken=%llu connect_failed=%llu\n",
                             static_cast<long long>((now - t0_) / 1000), static_cast<unsigned long long>(open_count()),
                             static_cast<unsigned long long>(broken_total()),
                             static_cast<unsigned long long>(connect_failed_));
                next_progress += 5000;
            }
        }
        const int64_t stopped = now_ms();
        if (!start_snapshot_taken_) take_snapshot(start_counts_);
        take_snapshot(end_counts_);
        for (auto& c : conns_) {
            if (c.fd >= 0) { close(c.fd); c.fd = -1; }
        }
        elapsed_ms_ = stopped - t0_;
        report();
        return 0;
    }

private:
    void start_connect(size_t i, int64_t now) {
        Conn& c = conns_[i];
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            const int err = errno;  // fprintf may clobber errno
            std::fprintf(stderr, "conncheck: socket: %s\n", std::strerror(err));
            fail_connect(c, err);
            return;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        c.fd = fd;
        c.connect_start = now;
        int r = connect(fd, reinterpret_cast<sockaddr*>(&dst_), sizeof dst_);
        if (r < 0 && errno != EINPROGRESS) {
            fail_connect(c, errno);
            return;
        }
        c.state = State::Connecting;
        ++pending_connects_;
        epoll_event ev{};
        ev.events = EPOLLOUT | EPOLLIN | EPOLLRDHUP;
        ev.data.u64 = i;
        epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
        if (r == 0) on_connected(i, now);
    }

    void fail_connect(Conn& c, int err) {
        if (c.state == State::Connecting) --pending_connects_;
        ++connect_failed_;
        ++connect_fail_reasons_[err];
        c.state = State::ConnectFailed;
        if (c.fd >= 0) { close(c.fd); c.fd = -1; }
    }

    void on_connected(size_t i, int64_t now) {
        Conn& c = conns_[i];
        if (c.state != State::Connecting) return;
        --pending_connects_;
        c.state = State::Open;
        ++established_;
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.u64 = i;
        epoll_ctl(ep_, EPOLL_CTL_MOD, c.fd, &ev);
        c.next_hb = now;  // first heartbeat right away, to learn the backend
        send_heartbeat(i, now);
    }

    void break_conn(size_t i, Cause cause, int64_t now) {
        Conn& c = conns_[i];
        if (c.state != State::Open) return;
        c.state = State::Broken;
        size_t sec = static_cast<size_t>(std::max<int64_t>(0, (now - t0_) / 1000));
        if (sec >= timeline_.size()) sec = timeline_.size() - 1;
        timeline_[sec][cause]++;
        broken_[cause]++;
        broken_by_backend_[c.backend]++;
        if (c.fd >= 0) {
            epoll_ctl(ep_, EPOLL_CTL_DEL, c.fd, nullptr);
            close(c.fd);
            c.fd = -1;
        }
    }

    static Cause cause_for_errno(int err) {
        switch (err) {
            case ECONNRESET:
            case EPIPE:
            case ECONNREFUSED: return kRst;
            default: return kTimeout;  // ETIMEDOUT, EHOSTUNREACH, ENETUNREACH ...
        }
    }

    void send_heartbeat(size_t i, int64_t now) {
        Conn& c = conns_[i];
        char line[48];
        int len = std::snprintf(line, sizeof line, "hb %llu\n", static_cast<unsigned long long>(++c.seq));
        ssize_t w = send(c.fd, line, static_cast<size_t>(len), MSG_NOSIGNAL);
        if (w == len) {
            c.hb_sent_at = now;
            return;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Socket buffer full: something is badly stuck. The timeout below catches it.
            c.hb_sent_at = now;
            return;
        }
        if (w >= 0) {  // short write of a 10-byte line: treat like a stall
            c.hb_sent_at = now;
            return;
        }
        break_conn(i, cause_for_errno(errno), now);
    }

    void handle_event(size_t i, uint32_t events, int64_t now) {
        Conn& c = conns_[i];
        if (c.state == State::Connecting) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) { fail_connect(c, err); return; }
            if (events & (EPOLLOUT | EPOLLIN)) on_connected(i, now);
            else if (events & (EPOLLERR | EPOLLHUP)) { fail_connect(c, ECONNREFUSED); return; }
            if (c.state != State::Open) return;
        }
        if (c.state != State::Open) return;
        char buf[2048];
        for (;;) {
            ssize_t r = recv(c.fd, buf, sizeof buf, 0);
            if (r > 0) {
                c.in.append(buf, static_cast<size_t>(r));
                continue;
            }
            if (r == 0) {
                consume_lines(i, now);  // replies that arrived before the FIN still count
                if (c.state == State::Open) break_conn(i, kEof, now);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            int err = errno;
            consume_lines(i, now);
            if (c.state == State::Open) break_conn(i, cause_for_errno(err), now);
            return;
        }
        consume_lines(i, now);
        if (c.state == State::Open && (events & EPOLLERR)) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            break_conn(i, cause_for_errno(err ? err : ECONNRESET), now);
        }
    }

    // Parse complete "id=N" lines. A partial line stays in the buffer.
    void consume_lines(size_t i, int64_t now) {
        Conn& c = conns_[i];
        size_t pos;
        while (c.state == State::Open && (pos = c.in.find('\n')) != std::string::npos) {
            std::string line = c.in.substr(0, pos);
            c.in.erase(0, pos + 1);
            if (line.rfind("id=", 0) != 0) continue;  // not ours, ignore
            int id = std::atoi(line.c_str() + 3);
            c.hb_sent_at = 0;
            c.next_hb = now + o_.heartbeat_ms;
            if (c.backend < 0) {
                c.backend = id;
            } else if (id != c.backend) {
                break_conn(i, kWrongBackend, now);
                return;
            }
        }
        if (c.in.size() > 4096) break_conn(i, kWrongBackend, now);  // garbage, not a conncheck server
    }

    void scan(int64_t now) {
        for (size_t i = 0; i < conns_.size(); ++i) {
            Conn& c = conns_[i];
            if (c.state == State::Connecting) {
                if (now - c.connect_start > o_.connect_timeout_ms) fail_connect(c, ETIMEDOUT);
                continue;
            }
            if (c.state != State::Open) continue;
            if (c.hb_sent_at != 0) {
                if (now - c.hb_sent_at > o_.timeout_ms) break_conn(i, kTimeout, now);
            } else if (now >= c.next_hb) {
                send_heartbeat(i, now);
            }
        }
    }

    bool all_have_backend() const {
        for (const auto& c : conns_)
            if (c.state == State::Open && c.backend < 0) return false;
        return true;
    }

    void take_snapshot(std::map<int, uint64_t>& out) const {
        out.clear();
        for (const auto& c : conns_)
            if (c.state == State::Open) out[c.backend]++;
    }

    uint64_t open_count() const {
        uint64_t n = 0;
        for (const auto& c : conns_) n += (c.state == State::Open);
        return n;
    }

    uint64_t broken_total() const {
        uint64_t n = 0;
        for (auto b : broken_) n += b;
        return n;
    }

    static std::string backend_name(int id) { return id < 0 ? "unknown" : std::to_string(id); }

    static void write_counts(FILE* f, const std::map<int, uint64_t>& m) {
        std::fputc('{', f);
        bool first = true;
        for (auto& [k, v] : m) {
            std::fprintf(f, "%s\"%s\": %llu", first ? "" : ", ", backend_name(k).c_str(), static_cast<unsigned long long>(v));
            first = false;
        }
        std::fputc('}', f);
    }

    void report() {
        const uint64_t broken = broken_total();
        const uint64_t survived = end_counts_.empty() ? 0 : [&] {
            uint64_t s = 0;
            for (auto& [k, v] : end_counts_) s += v;
            return s;
        }();
        if (!o_.json_path.empty()) {
            FILE* f = std::fopen(o_.json_path.c_str(), "w");
            if (!f) {
                std::perror(o_.json_path.c_str());
            } else {
                std::fprintf(f, "{\n");
                std::fprintf(f, "  \"vip\": \"%s:%d\",\n", o_.vip_host.c_str(), o_.vip_port);
                std::fprintf(f, "  \"heartbeat_ms\": %d,\n  \"timeout_ms\": %d,\n", o_.heartbeat_ms, o_.timeout_ms);
                std::fprintf(f, "  \"duration_s\": %.3f,\n", elapsed_ms_ / 1000.0);
                std::fprintf(f, "  \"ramp_complete_s\": %.3f,\n", ramp_done_ms_ / 1000.0);
                std::fprintf(f, "  \"total\": %d,\n", o_.connections);
                std::fprintf(f, "  \"established\": %llu,\n", static_cast<unsigned long long>(established_));
                std::fprintf(f, "  \"connect_failed\": %llu,\n", static_cast<unsigned long long>(connect_failed_));
                std::fprintf(f, "  \"connect_fail_reasons\": {");
                bool first = true;
                for (auto& [e, n] : connect_fail_reasons_) {
                    std::fprintf(f, "%s\"%s\": %llu", first ? "" : ", ", std::strerror(e), static_cast<unsigned long long>(n));
                    first = false;
                }
                std::fprintf(f, "},\n");
                std::fprintf(f, "  \"broken\": %llu,\n", static_cast<unsigned long long>(broken));
                std::fprintf(f, "  \"broken_by_cause\": {");
                for (int k = 0; k < kNumCauses; ++k)
                    std::fprintf(f, "%s\"%s\": %llu", k ? ", " : "", kCauseName[k], static_cast<unsigned long long>(broken_[k]));
                std::fprintf(f, "},\n");
                std::fprintf(f, "  \"survived\": %llu,\n", static_cast<unsigned long long>(survived));
                std::fprintf(f, "  \"backends_at_start\": ");
                write_counts(f, start_counts_);
                std::fprintf(f, ",\n  \"backends_at_end\": ");
                write_counts(f, end_counts_);
                std::fprintf(f, ",\n  \"broken_by_backend\": ");
                write_counts(f, broken_by_backend_);
                std::fprintf(f, ",\n  \"timeline\": [");
                size_t last = 0;
                for (size_t s = 0; s < timeline_.size(); ++s)
                    for (auto v : timeline_[s]) if (v) last = s;
                for (size_t s = 0; s <= last && s < timeline_.size(); ++s) {
                    uint64_t tot = 0;
                    for (auto v : timeline_[s]) tot += v;
                    std::fprintf(f, "%s\n    {\"t\": %zu, \"broken\": %llu", s ? "," : "", s, static_cast<unsigned long long>(tot));
                    for (int k = 0; k < kNumCauses; ++k)
                        std::fprintf(f, ", \"%s\": %llu", kCauseName[k], static_cast<unsigned long long>(timeline_[s][k]));
                    std::fprintf(f, "}");
                }
                std::fprintf(f, "\n  ]\n}\n");
                std::fclose(f);
            }
        }
        std::printf("conncheck: total=%d established=%llu broken=%llu (rst=%llu eof=%llu timeout=%llu wrong_backend=%llu) "
                    "survived=%llu connect_failed=%llu duration=%.1fs\n",
                    o_.connections, static_cast<unsigned long long>(established_), static_cast<unsigned long long>(broken),
                    static_cast<unsigned long long>(broken_[kRst]), static_cast<unsigned long long>(broken_[kEof]),
                    static_cast<unsigned long long>(broken_[kTimeout]), static_cast<unsigned long long>(broken_[kWrongBackend]),
                    static_cast<unsigned long long>(survived), static_cast<unsigned long long>(connect_failed_),
                    elapsed_ms_ / 1000.0);
        std::fflush(stdout);
    }

    Options o_;
    std::vector<Conn> conns_;
    sockaddr_in dst_{};
    int ep_ = -1;
    int64_t t0_ = 0;
    int64_t elapsed_ms_ = 0;
    int64_t ramp_done_ms_ = 0;
    uint64_t pending_connects_ = 0;
    uint64_t established_ = 0;
    uint64_t connect_failed_ = 0;
    std::map<int, uint64_t> connect_fail_reasons_;
    uint64_t broken_[kNumCauses] = {};
    std::map<int, uint64_t> broken_by_backend_;
    std::map<int, uint64_t> start_counts_;
    std::map<int, uint64_t> end_counts_;
    bool start_snapshot_taken_ = false;
    std::vector<std::array<uint64_t, kNumCauses>> timeline_;
};

}  // namespace

int main(int argc, char** argv) {
    Options o = parse(argc, argv);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    raise_nofile(o.connections);
    Runner r(std::move(o));
    return r.run();
}
