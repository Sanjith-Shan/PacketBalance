// SPDX-License-Identifier: MIT
//
// packetbalance: the control-plane daemon. See `packetbalance --help`.
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
