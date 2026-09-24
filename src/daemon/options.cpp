// SPDX-License-Identifier: MIT
#include "options.h"

#include <getopt.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pb {

namespace {

constexpr const char* kUsage = R"(usage: packetbalance [options]

PacketBalance control plane: loads the XDP load balancer onto an interface,
programs its maps from the YAML config, health-checks the reals, and serves
the pbctl API and Prometheus metrics.

options (each overrides the config file):
  --config PATH           YAML config (default /etc/packetbalance/packetbalance.yaml)
  --interface IFACE       interface to attach the XDP program to
  --xdp-mode MODE         native | generic | auto (auto: native, else generic)
  --hash MODE             maglev | modulo (modulo is the naive baseline)
  --no-conntrack          never consult the connection table (hash every packet)
  --conntrack-size N      connection-table entries per CPU, fixed at map creation
  --socket PATH           control socket (default /run/packetbalance.sock)
  --pin-path DIR          bpffs directory for pinned maps (default /sys/fs/bpf/packetbalance)
  --metrics-listen A:P    Prometheus endpoint (default 127.0.0.1:9101)
  --no-health-check       do not health-check; every real is considered up
                          (UDP VIPs are never checked: there is no handshake to test)
  --recreate-maps         if a pinned map no longer matches this build (for example
                          conntrack.size changed), unpin and recreate it instead of
                          refusing to start. Recreating conntrack drops every tracked flow.
  --detach-on-exit        on SIGTERM/SIGINT, detach the program and unpin the maps
  --log-level LEVEL       debug | info | warn | error (default info)
  -h, --help              this text

exit behaviour:
  By default SIGTERM and SIGINT stop the control plane only. The XDP program
  stays attached and the maps stay pinned, so packets keep flowing, flows keep
  their reals, and the next start reuses the maps and swaps the program in
  atomically: restarting or upgrading the daemon drops no connection. Health
  checks stop while the daemon is down. Use --detach-on-exit to tear
  everything down (the connection table goes with it).

The config is re-read on the `reload` API command (pbctl reload).
)";

enum Flag {
    kConfig = 256, kInterface, kXdpMode, kHash, kNoConntrack, kConntrackSize, kSocket, kPinPath,
    kMetricsListen, kNoHealthCheck, kRecreateMaps, kDetachOnExit, kLogLevel,
};

[[noreturn]] void usage_error(const std::string& msg) {
    std::fprintf(stderr, "packetbalance: %s\ntry --help\n", msg.c_str());
    std::exit(2);
}

}  // namespace

void Options::apply_to(Config& cfg) const {
    if (interface) cfg.interface = *interface;
    if (xdp_mode) cfg.xdp_mode = *xdp_mode;
    if (hash) cfg.hash = *hash;
    if (no_conntrack) cfg.conntrack_enabled = false;
    if (conntrack_size) cfg.conntrack_size = *conntrack_size;
    if (socket) cfg.socket = *socket;
    if (pin_path) cfg.pin_path = *pin_path;
    if (metrics_listen) cfg.metrics_listen = *metrics_listen;
    if (no_health_check) cfg.health.enabled = false;
}

Options parse_options(int argc, char** argv) {
    static const option longopts[] = {
        {"config", required_argument, nullptr, kConfig},
        {"interface", required_argument, nullptr, kInterface},
        {"xdp-mode", required_argument, nullptr, kXdpMode},
        {"hash", required_argument, nullptr, kHash},
        {"no-conntrack", no_argument, nullptr, kNoConntrack},
        {"conntrack-size", required_argument, nullptr, kConntrackSize},
        {"socket", required_argument, nullptr, kSocket},
        {"pin-path", required_argument, nullptr, kPinPath},
        {"metrics-listen", required_argument, nullptr, kMetricsListen},
        {"no-health-check", no_argument, nullptr, kNoHealthCheck},
        {"recreate-maps", no_argument, nullptr, kRecreateMaps},
        {"detach-on-exit", no_argument, nullptr, kDetachOnExit},
        {"log-level", required_argument, nullptr, kLogLevel},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    Options o;
    int c;
    while ((c = getopt_long(argc, argv, "h", longopts, nullptr)) != -1) {
        const std::string arg = optarg ? optarg : "";
        try {
            switch (c) {
                case kConfig: o.config_path = arg; break;
                case kInterface: o.interface = arg; break;
                case kXdpMode: o.xdp_mode = parse_xdp_mode(arg); break;
                case kHash: o.hash = parse_hash_mode(arg); break;
                case kNoConntrack: o.no_conntrack = true; break;
                case kConntrackSize: {
                    const unsigned long n = std::stoul(arg);
                    if (n == 0 || n > (1ul << 27)) throw std::invalid_argument("out of range");
                    o.conntrack_size = static_cast<uint32_t>(n);
                    break;
                }
                case kSocket: o.socket = arg; break;
                case kPinPath: o.pin_path = arg; break;
                case kMetricsListen: o.metrics_listen = arg; break;
                case kNoHealthCheck: o.no_health_check = true; break;
                case kRecreateMaps: o.recreate_maps = true; break;
                case kDetachOnExit: o.detach_on_exit = true; break;
                case kLogLevel: o.log_level = log::parse_level(arg); break;
                case 'h': std::fputs(kUsage, stdout); std::exit(0);
                default: usage_error("unknown option");
            }
        } catch (const std::exception& e) {
            usage_error(std::string("bad value '") + arg + "': " + e.what());
        }
    }
    if (optind < argc) usage_error(std::string("unexpected argument: ") + argv[optind]);
    return o;
}

}  // namespace pb
