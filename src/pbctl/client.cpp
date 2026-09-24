// SPDX-License-Identifier: MIT
#include "client.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace pbctl {

namespace {

class Socket {
public:
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() {
        if (fd_ >= 0) ::close(fd_);
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    int get() const { return fd_; }

private:
    int fd_;
};

}  // namespace

nlohmann::json call(const std::string& socket_path, const nlohmann::json& request) {
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof sa.sun_path) throw std::runtime_error("socket path too long");
    std::memcpy(sa.sun_path, socket_path.c_str(), socket_path.size() + 1);

    Socket s(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (s.get() < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    if (::connect(s.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0)
        throw std::runtime_error("cannot connect to " + socket_path + ": " + std::strerror(errno) +
                                 " (is packetbalance running?)");

    const std::string line = request.dump() + "\n";
    for (size_t off = 0; off < line.size();) {
        ssize_t n = ::write(s.get(), line.data() + off, line.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error(std::string("write: ") + std::strerror(errno));
        off += static_cast<size_t>(n);
    }

    std::string resp;
    char buf[65536];
    while (resp.find('\n') == std::string::npos) {
        ssize_t n = ::read(s.get(), buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw std::runtime_error(std::string("read: ") + std::strerror(errno));
        if (n == 0) break;
        resp.append(buf, static_cast<size_t>(n));
    }
    if (resp.empty()) throw std::runtime_error("daemon closed the connection without answering");
    try {
        return nlohmann::json::parse(resp.substr(0, resp.find('\n')));
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("bad response from daemon: ") + e.what());
    }
}

}  // namespace pbctl
