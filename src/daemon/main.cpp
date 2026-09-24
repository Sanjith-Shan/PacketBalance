// SPDX-License-Identifier: MIT
//
// packetbalance: the control-plane daemon. See `packetbalance --help`.
#include <sys/resource.h>

#include <csignal>
#include <cstdio>
#include <exception>

#include "daemon.h"
#include "log.h"
#include "options.h"

namespace {
// The only global: set from the signal handler, polled by Daemon::run.
volatile std::sig_atomic_t g_stop = 0;

extern "C" void on_signal(int) { g_stop = 1; }

// A health-check round holds one socket per (TCP VIP, real) pair at once, up
// to 64 x 511. The usual soft limit of 1024 would starve it; the hard limit
// (524288 under systemd) does not.
void raise_nofile_limit() {
    rlimit rl{};
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur >= rl.rlim_max) return;
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        pb::log::warn("could not raise RLIMIT_NOFILE to {}", static_cast<unsigned long long>(rl.rlim_max));
}
}  // namespace

int main(int argc, char** argv) {
    const pb::Options opts = pb::parse_options(argc, argv);
    pb::log::set_level(opts.log_level);

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    std::signal(SIGPIPE, SIG_IGN);  // a vanished API client must not kill the daemon
    raise_nofile_limit();

    try {
        pb::Daemon daemon(opts);
        daemon.run(&g_stop);
    } catch (const std::exception& e) {
        pb::log::error("fatal: {}", e.what());
        return 1;
    }
    pb::log::info("exited cleanly");
    return 0;
}
