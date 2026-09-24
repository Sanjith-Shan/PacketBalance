// SPDX-License-Identifier: MIT
#include "health_checker.h"

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

#include "log.h"
#include "unique_fd.h"

namespace pb {

using Clock = std::chrono::steady_clock;

HealthChecker::HealthChecker(LbState& state, const HealthCheckConfig& params)
    : state_(state), params_(params) {}

void HealthChecker::start() {
    log::info("health checker: interval={}ms timeout={}ms fall={} rise={}", params_.interval_ms,
              params_.timeout_ms, params_.fall, params_.rise);
    thread_.start([this] { return step(); });
}

void HealthChecker::stop() { thread_.stop(); }

void HealthChecker::set_params(const HealthCheckConfig& params) {
    std::lock_guard lock(params_mu_);
    params_ = params;
}

std::chrono::milliseconds HealthChecker::step() {
    HealthCheckConfig p;
    {
        std::lock_guard lock(params_mu_);
        p = params_;
    }
    const auto start = Clock::now();
    const std::vector<HealthTarget> targets = state_.health_targets();
    const std::vector<ProbeResult> results = probe_all(targets, std::chrono::milliseconds(p.timeout_ms));
    for (size_t i = 0; i < targets.size(); ++i) state_.report_health(targets[i], results[i], p.rise, p.fall);

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    const auto interval = std::chrono::milliseconds(p.interval_ms);
    return spent < interval ? interval - spent : std::chrono::milliseconds(0);
}

std::vector<ProbeResult> HealthChecker::probe_all(const std::vector<HealthTarget>& targets,
                                                  std::chrono::milliseconds timeout) const {
    std::vector<ProbeResult> results(targets.size());
    std::vector<UniqueFd> socks(targets.size());
    std::vector<Clock::time_point> started(targets.size());
    std::vector<bool> pending(targets.size(), false);

    auto finish = [&](size_t i, bool ok, std::string detail) {
        results[i].ok = ok;
        results[i].detail = std::move(detail);
        results[i].rtt_us = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started[i]).count());
        pending[i] = false;
        socks[i].reset();
    };

    for (size_t i = 0; i < targets.size(); ++i) {
        started[i] = Clock::now();
        socks[i].reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!socks[i]) {
            finish(i, false, std::string("socket: ") + std::strerror(errno));
            continue;
        }
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = targets[i].addr;
        sa.sin_port = targets[i].vip.port_be;
        if (::connect(socks[i].get(), reinterpret_cast<const sockaddr*>(&sa), sizeof sa) == 0)
            finish(i, true, "connected");
        else if (errno == EINPROGRESS)
            pending[i] = true;
        else
            finish(i, false, std::strerror(errno));
    }

    const auto deadline = Clock::now() + timeout;
    while (true) {
        std::vector<pollfd> pfds;
        std::vector<size_t> index;
        for (size_t i = 0; i < targets.size(); ++i)
            if (pending[i]) {
                pfds.push_back({socks[i].get(), POLLOUT, 0});
                index.push_back(i);
            }
        if (pfds.empty()) break;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (left.count() <= 0) break;
        if (::poll(pfds.data(), pfds.size(), static_cast<int>(left.count())) < 0 && errno != EINTR) break;
        for (size_t k = 0; k < pfds.size(); ++k) {
            if (!pfds[k].revents) continue;
            // Writable means the handshake finished, one way or the other;
            // SO_ERROR says which.
            int err = 0;
            socklen_t len = sizeof err;
            ::getsockopt(pfds[k].fd, SOL_SOCKET, SO_ERROR, &err, &len);
            finish(index[k], err == 0, err == 0 ? "connected" : std::strerror(err));
        }
    }
    for (size_t i = 0; i < targets.size(); ++i)
        if (pending[i]) finish(i, false, "timeout after " + std::to_string(timeout.count()) + " ms");
    return results;
}

}  // namespace pb
