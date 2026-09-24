// SPDX-License-Identifier: MIT
#include "daemon.h"

#include <chrono>
#include <thread>

#include "log.h"
#include "metrics.h"

namespace pb {

using nlohmann::json;

Config Daemon::load_config() const {
    Config cfg = load_config_file(opts_.config_path);
    opts_.apply_to(cfg);
    validate_config(cfg);
    return cfg;
}

Daemon::Daemon(Options opts) : opts_(std::move(opts)) {
    config_ = load_config();
    log::info("config {}: interface={} xdp_mode={} hash={} conntrack={} size={} vips={}",
              opts_.config_path, config_.interface, xdp_mode_name(config_.xdp_mode),
              hash_mode_name(config_.hash), config_.conntrack_enabled ? "on" : "off",
              config_.conntrack_size, config_.vips.size());

    loader_ = std::make_unique<Loader>(Loader::Options{
        .interface = config_.interface,
        .mode = config_.xdp_mode,
        .conntrack_size = config_.conntrack_size,
        .pin_path = config_.pin_path,
        .recreate_maps = opts_.recreate_maps,
    });
    if (loader_->reused_pins())
        log::info("reusing pinned maps under {}: {}", config_.pin_path,
                  loader_->reused_conntrack() ? "tracked flows are preserved"
                                              : "but the connection table is new (no tracked flows)");

    reader_ = std::make_unique<MapReader>(loader_->maps());
    state_ = std::make_unique<LbState>(
        loader_->maps(), *reader_, config_.interface, [iface = config_.interface](uint32_t ip) {
            const auto table = read_arp_table(iface);
            auto it = table.find(ip);
            return it == table.end() ? std::nullopt : std::optional<Mac>(it->second);
        });
    state_->apply(config_);

    neighbors_ = std::make_unique<NeighborResolver>(*state_, config_.interface);
    health_ = std::make_unique<HealthChecker>(*state_, config_.health);
    commands_ = std::make_unique<CommandHandler>(*state_, *reader_,
                                                 CommandHandler::Hooks{
                                                     .reload = [this] { reload(); },
                                                     .config = [this] { return config_json(); },
                                                     .reals_changed = [this] { neighbors_->wake(); },
                                                 });
    api_ = std::make_unique<ApiServer>(config_.socket,
                                       [this](const std::string& line) { return commands_->handle_line(line); });
    metrics_ = std::make_unique<MetricsServer>(config_.metrics_listen, [this] {
        return render_metrics(*state_, *reader_, loader_->attached_mode());
    });
}

Daemon::~Daemon() = default;

void Daemon::run(const volatile std::sig_atomic_t* stop) {
    neighbors_->start();
    if (config_.health.enabled)
        health_->start();
    else
        log::info("health checks disabled: every real is considered up");
    api_->start();
    metrics_->start();
    log::info("packetbalance running (xdp {} on {})", xdp_mode_name(loader_->attached_mode()),
              config_.interface);

    while (!*stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    log::info("shutting down");
    metrics_->stop();
    api_->stop();
    health_->stop();
    neighbors_->stop();
    if (opts_.detach_on_exit) {
        loader_->detach_and_unpin();
    } else {
        log::info("leaving the XDP program attached to {} and the maps pinned under {} "
                  "(forwarding continues; use --detach-on-exit to tear down)",
                  config_.interface, config_.pin_path);
    }
}

void Daemon::reload() {
    Config next;
    try {
        next = load_config();
    } catch (const std::exception& e) {
        throw ApiError(std::string("reload: ") + e.what());
    }

    std::lock_guard lock(config_mu_);
    auto restart_only = [&](const char* what, bool differs) {
        if (differs) log::warn("reload: {} changed; it takes effect on restart only", what);
    };
    restart_only("interface", next.interface != config_.interface);
    restart_only("xdp_mode", next.xdp_mode != config_.xdp_mode);
    restart_only("conntrack.size", next.conntrack_size != config_.conntrack_size);
    restart_only("socket", next.socket != config_.socket);
    restart_only("pin_path", next.pin_path != config_.pin_path);
    restart_only("metrics.listen", next.metrics_listen != config_.metrics_listen);
    restart_only("health_check.enabled", next.health.enabled != config_.health.enabled);
    // Keep those at their running values so `config` reports the truth.
    next.interface = config_.interface;
    next.xdp_mode = config_.xdp_mode;
    next.conntrack_size = config_.conntrack_size;
    next.socket = config_.socket;
    next.pin_path = config_.pin_path;
    next.metrics_listen = config_.metrics_listen;
    next.health.enabled = config_.health.enabled;

    state_->apply(next);
    health_->set_params(next.health);
    config_ = next;
    log::info("reload: applied {} ({} vips)", opts_.config_path, config_.vips.size());
}

json Daemon::config_json() const {
    std::lock_guard lock(config_mu_);
    const Config& c = config_;
    json vips = json::array();
    for (const VipConfig& v : c.vips) {
        json reals = json::array();
        for (const RealConfig& r : v.reals) reals.push_back({{"address", ipv4_to_string(r.addr_be)}, {"weight", r.weight}});
        vips.push_back({{"vip", v.spec.str()}, {"no_conntrack", v.no_conntrack}, {"reals", reals}});
    }
    return {
        {"config_path", opts_.config_path},
        {"interface", c.interface},
        {"xdp_mode", xdp_mode_name(loader_->attached_mode())},  // in effect
        {"xdp_mode_requested", xdp_mode_name(c.xdp_mode)},
        {"hash", hash_mode_name(c.hash)},
        {"conntrack", {{"enabled", c.conntrack_enabled}, {"size", c.conntrack_size}}},
        {"encap_src_prefix", c.encap_src.str()},
        {"next_hop", c.next_hop_be ? json(ipv4_to_string(*c.next_hop_be)) : json(nullptr)},
        {"socket", c.socket},
        {"pin_path", c.pin_path},
        {"metrics_listen", c.metrics_listen},
        {"health_check",
         {{"enabled", c.health.enabled},
          {"interval_ms", c.health.interval_ms},
          {"timeout_ms", c.health.timeout_ms},
          {"fall", c.health.fall},
          {"rise", c.health.rise}}},
        {"detach_on_exit", opts_.detach_on_exit},
        {"num_possible_cpus", reader_->num_cpus()},
        {"vips", vips},
    };
}

}  // namespace pb
