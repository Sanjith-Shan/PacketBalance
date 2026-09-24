// SPDX-License-Identifier: MIT
//
// The daemon's configuration: deploy/packetbalance.yaml parsed into plain
// structs. Parsing lives in libpb_core (src/core/config.cpp) so it is unit
// tested on any platform; the daemon applies command-line overrides on top.
//
// Every address in here is already validated and converted to network byte
// order, so nothing downstream re-parses strings.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "packetbalance/abi.h"
#include "packetbalance/maglev.h"
#include "packetbalance/vipspec.h"

namespace pb {

// How the XDP program is attached. Auto tries native (driver) mode and falls
// back to generic (skb) mode if the driver refuses.
enum class XdpMode { Native, Generic, Auto };

XdpMode parse_xdp_mode(const std::string& s);  // "native" | "generic" | "auto", throws
const char* xdp_mode_name(XdpMode m);

// "10.99.0.0/24" -> prefix and mask, both network order. The prefix is
// masked, so "10.99.0.7/24" yields 10.99.0.0. Throws std::invalid_argument.
struct Cidr {
    uint32_t prefix_be = 0;
    uint32_t mask_be = 0;
    uint8_t len = 0;
    std::string str() const;
};
Cidr parse_cidr(const std::string& s);

struct RealConfig {
    uint32_t addr_be = 0;
    uint32_t weight = 1;
};

struct VipConfig {
    VipSpec spec;
    bool no_conntrack = false;  // optional per-VIP `no_conntrack: true`
    std::vector<RealConfig> reals;
};

struct HealthCheckConfig {
    bool enabled = true;        // optional `enabled: false`, or --no-health-check
    uint32_t interval_ms = 1000;
    uint32_t timeout_ms = 500;
    uint32_t fall = 3;          // consecutive failures before UP -> DOWN
    uint32_t rise = 2;          // consecutive successes before DOWN -> UP
};

struct Config {
    std::string interface;
    XdpMode xdp_mode = XdpMode::Auto;
    HashMode hash = HashMode::Maglev;
    Cidr encap_src{};                   // required
    std::optional<uint32_t> next_hop_be;
    bool conntrack_enabled = true;
    uint32_t conntrack_size = PB_CT_DEFAULT_SIZE;
    std::string socket = "/run/packetbalance.sock";
    std::string pin_path = PB_PIN_DIR;
    std::string metrics_listen = "127.0.0.1:9101";
    HealthCheckConfig health;
    std::vector<VipConfig> vips;
};

// Parses YAML text. Throws std::runtime_error with a message naming the
// offending key on any schema or value error. Unknown top-level keys are an
// error too: a typo such as `conntrak:` must not silently use the default.
Config parse_config(const std::string& yaml_text);

// Reads and parses a file. Throws std::runtime_error.
Config load_config_file(const std::string& path);

// Semantic checks shared by the parser and by the daemon after command-line
// overrides: interface set, VIP count, no duplicate VIPs or reals, weights in
// range. Throws std::runtime_error.
void validate_config(const Config& cfg);

// Largest weight accepted for a real. Maglev gives a real `weight` turns per
// fill round, so an absurd weight only slows the fill; 1000 is plenty.
inline constexpr uint32_t kMaxRealWeight = 1000;

}  // namespace pb
