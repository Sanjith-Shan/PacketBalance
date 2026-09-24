// SPDX-License-Identifier: MIT
//
// TCP health checks with hysteresis.
//
// One thread. Every interval it starts a non-blocking connect() to
// real:vip_port for every (TCP VIP, real) pair at once and waits for all of
// them with a single poll() bounded by the timeout, so a round costs at most
// timeout_ms no matter how many reals there are, and one slow real does not
// delay the others' verdicts. Results go to LbState::report_health, which
// counts consecutive outcomes: `fall` failures take a real out of the ring,
// `rise` successes put it back.
//
// Policy choices:
//   - A new real starts UP with consecutive_ok = 0. It takes traffic the moment
//     it is added (an operator adding a real expects it to serve) and is removed
//     after `fall` failed checks if it is not actually healthy. Starting DOWN
//     would instead delay every add, and every daemon restart, by `rise` rounds.
//   - UDP VIPs are not checked: there is no handshake to test. Their reals are
//     always UP and reported with "checked": false.
//   - Draining reals are still checked, so undrain acts on current health.
#pragma once

#include <chrono>
#include <mutex>
#include <vector>

#include "lb_state.h"
#include "packetbalance/config.h"
#include "periodic_thread.h"

namespace pb {

class HealthChecker {
public:
    HealthChecker(LbState& state, const HealthCheckConfig& params);
    void start();
    void stop();
    void set_params(const HealthCheckConfig& params);  // on reload

private:
    std::chrono::milliseconds step();
    std::vector<ProbeResult> probe_all(const std::vector<HealthTarget>& targets,
                                       std::chrono::milliseconds timeout) const;

    LbState& state_;
    mutable std::mutex params_mu_;
    HealthCheckConfig params_;
    PeriodicThread thread_;
};

}  // namespace pb
