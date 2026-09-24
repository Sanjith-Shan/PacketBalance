// SPDX-License-Identifier: MIT
#include "api_server.h"

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "log.h"

namespace pb {

namespace {

constexpr size_t kMaxRequestBytes = 1 << 20;

sockaddr_un unix_address(const std::string& path) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (path.size() >= sizeof sa.sun_path) throw std::runtime_error("socket path too long: " + path);
    std::memcpy(sa.sun_path, path.c_str(), path.size() + 1);
    return sa;
}

// True if something is accepting on `path` right now (another daemon).
bool socket_is_live(const std::string& path) {
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const sockaddr_un sa = unix_address(path);
    return fd && ::connect(fd.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0;
}

bool write_all(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

ApiServer::ApiServer(std::string path, Handler handler)
    : path_(std::move(path)), handler_(std::move(handler)) {}

ApiServer::~ApiServer() { stop(); }

void ApiServer::start() {
    if (socket_is_live(path_))
        throw std::runtime_error("another daemon is serving " + path_ + "; refusing to take it over");
    ::unlink(path_.c_str());  // stale file from a previous run

    listen_fd_.reset(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!listen_fd_) throw std::runtime_error(std::string("api socket: ") + std::strerror(errno));
    const sockaddr_un sa = unix_address(path_);
    if (::bind(listen_fd_.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0)
        throw std::runtime_error("bind " + path_ + ": " + std::strerror(errno));
    ::chmod(path_.c_str(), 0660);  // root and the socket's group may drive the LB
    if (::listen(listen_fd_.get(), 16) != 0)
        throw std::runtime_error("listen " + path_ + ": " + std::strerror(errno));
    acceptor_ = std::thread([this] { accept_loop(); });
    log::info("api: listening on {}", path_);
}

void ApiServer::stop() {
    if (stopping_.exchange(true)) return;
    if (acceptor_.joinable()) acceptor_.join();
    {
        // Unblock every client thread's recv(), then join them.
        std::lock_guard lock(clients_mu_);
        for (Client& c : clients_) ::shutdown(c.fd, SHUT_RDWR);
    }
    for (Client& c : clients_)
        if (c.thread.joinable()) c.thread.join();
    clients_.clear();
    if (listen_fd_) ::unlink(path_.c_str());
    listen_fd_.reset();
}

void ApiServer::accept_loop() {
    while (!stopping_) {
        pollfd p{listen_fd_.get(), POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;  // wake up to notice stop()
        int fd = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;

        std::lock_guard lock(clients_mu_);
        // Reap finished connections so the list does not grow forever.
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->done) {
                it->thread.join();
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        Client& c = clients_.emplace_back(Client{fd, {}, false});
        c.thread = std::thread([this, &c] {
            serve(c.fd);
            std::lock_guard l(clients_mu_);
            ::close(c.fd);
            c.fd = -1;
            c.done = true;
        });
    }
}

void ApiServer::serve(int fd) {
    std::string buf;
    char chunk[4096];
    while (!stopping_) {
        ssize_t n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!write_all(fd, handler_(line) + "\n")) return;
        }
        if (buf.size() > kMaxRequestBytes) {
            write_all(fd, R"({"ok":false,"error":"request too large"})" "\n");
            return;
        }
    }
}

}  // namespace pb
