// SPDX-License-Identifier: MIT
//
// Wires the pieces together and owns their lifetimes:
//
//   Loader            skeleton, pinned maps, XDP attach
//   MapReader         per-CPU counters and the connection table (read side)
//   LbState           model + every control-plane map write
//   NeighborResolver  ARP -> neigh
//   HealthChecker     TCP checks -> UP/DOWN -> ring swaps
//   ApiServer         unix socket -> CommandHandler
//   MetricsServer     /metrics
//
// Members are declared in dependency order, so destruction (reverse order)
// stops the threads before the state and maps they use go away.
#pragma once

#include <csignal>
#include <memory>
#include <mutex>

#include <nlohmann/json.hpp>

#include "api_server.h"
#include "commands.h"
#include "health_checker.h"
#include "lb_state.h"
#include "loader.h"
#include "map_reader.h"
#include "metrics_server.h"
#include "neighbor_resolver.h"
#include "options.h"

namespace pb {

class Daemon {
public:
    explicit Daemon(Options opts);
    ~Daemon();

    // Runs until *stop becomes non-zero (set by the signal handler).
    void run(const volatile std::sig_atomic_t* stop);

private:
    Config load_config() const;
    void reload();
    nlohmann::json config_json() const;

    Options opts_;
    mutable std::mutex config_mu_;
    Config config_;
    std::unique_ptr<Loader> loader_;
    std::unique_ptr<MapReader> reader_;
    std::unique_ptr<LbState> state_;
    std::unique_ptr<NeighborResolver> neighbors_;
    std::unique_ptr<HealthChecker> health_;
    std::unique_ptr<CommandHandler> commands_;
    std::unique_ptr<ApiServer> api_;
    std::unique_ptr<MetricsServer> metrics_;
};

}  // namespace pb
