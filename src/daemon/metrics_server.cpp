// SPDX-License-Identifier: MIT
#include "metrics_server.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>

#include "log.h"
#include "packetbalance/vipspec.h"

namespace pb {

MetricsServer::MetricsServer(std::string listen, Render render)
    : listen_(std::move(listen)), render_(std::move(render)) {}

MetricsServer::~MetricsServer() { stop(); }

void MetricsServer::start() {
    const auto colon = listen_.rfind(':');
    if (colon == std::string::npos) throw std::runtime_error("metrics.listen must be ADDR:PORT: " + listen_);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    try {
        sa.sin_addr.s_addr = parse_ipv4(listen_.substr(0, colon));
        const int port = std::stoi(listen_.substr(colon + 1));
        if (port < 1 || port > 65535) throw std::invalid_argument("port");
        sa.sin_port = port_to_be(static_cast<uint16_t>(port));
    } catch (const std::exception&) {
        throw std::runtime_error("bad metrics.listen: " + listen_);
    }

    fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    const int one = 1;
    ::setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (::bind(fd_.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) != 0 ||
        ::listen(fd_.get(), 16) != 0)
        throw std::runtime_error("metrics: bind " + listen_ + ": " + std::strerror(errno));
    thread_ = std::thread([this] { loop(); });
    log::info("metrics: http://{}/metrics", listen_);
}

void MetricsServer::stop() {
    if (stopping_.exchange(true)) return;
    if (thread_.joinable()) thread_.join();
    fd_.reset();
}

void MetricsServer::loop() {
    while (!stopping_) {
        pollfd p{fd_.get(), POLLIN, 0};
        if (::poll(&p, 1, 200) <= 0) continue;
        UniqueFd client(::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC));
        if (client) serve(client.get());
    }
}

void MetricsServer::serve(int fd) {
    // A slow or silent client must not wedge the only metrics thread.
    const timeval tv{.tv_sec = 2, .tv_usec = 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    std::string req;
    char buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.find("\n\n") == std::string::npos &&
           req.size() < 8192) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n <= 0) break;
        req.append(buf, static_cast<size_t>(n));
    }

    // Request line: METHOD SP PATH SP VERSION. Only the first two matter.
    const std::string line = req.substr(0, req.find_first_of("\r\n"));
    const bool is_metrics = line.rfind("GET /metrics ", 0) == 0 || line == "GET /metrics" ||
                            line.rfind("GET /metrics?", 0) == 0;
    std::string status = "200 OK", body;
    if (is_metrics) {
        try {
            body = render_();
        } catch (const std::exception& e) {
            status = "500 Internal Server Error";
            body = std::string(e.what()) + "\n";
        }
    } else {
        status = "404 Not Found";
        body = "not found; try /metrics\n";
    }
    const std::string resp = std::format(
        "HTTP/1.0 {}\r\nContent-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
        status, body.size(), body);
    size_t off = 0;
    while (off < resp.size()) {
        ssize_t n = ::send(fd, resp.data() + off, resp.size() - off, MSG_NOSIGNAL);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
}

}  // namespace pb
