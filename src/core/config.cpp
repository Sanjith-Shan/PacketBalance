// SPDX-License-Identifier: MIT
//
// YAML config -> pb::Config. The schema is exactly deploy/packetbalance.yaml.
#include "packetbalance/config.h"

#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#ifdef PB_HAVE_YAML
#include <yaml-cpp/yaml.h>
#endif

namespace pb {

XdpMode parse_xdp_mode(const std::string& s) {
    if (s == "native") return XdpMode::Native;
    if (s == "generic") return XdpMode::Generic;
    if (s == "auto") return XdpMode::Auto;
    throw std::invalid_argument("bad xdp_mode (native|generic|auto): " + s);
}

const char* xdp_mode_name(XdpMode m) {
    switch (m) {
        case XdpMode::Native: return "native";
        case XdpMode::Generic: return "generic";
        case XdpMode::Auto: return "auto";
    }
    return "?";
}

std::string Cidr::str() const { return ipv4_to_string(prefix_be) + "/" + std::to_string(len); }

Cidr parse_cidr(const std::string& s) {
    auto slash = s.find('/');
    if (slash == std::string::npos) throw std::invalid_argument("bad CIDR, want ADDR/LEN: " + s);
    const std::string len_str = s.substr(slash + 1);
    if (len_str.empty() || len_str.size() > 2 ||
        len_str.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("bad CIDR prefix length: " + s);
    const int len = std::stoi(len_str);
    if (len > 32) throw std::invalid_argument("bad CIDR prefix length: " + s);

    const uint32_t addr = parse_ipv4(s.substr(0, slash));
    const uint32_t host_mask = len == 0 ? 0u : ~uint32_t{0} << (32 - len);
    // Convert the host-order mask to network order the same portable way
    // vipspec.h does: through its dotted-quad bytes.
    const unsigned char mb[4] = {(unsigned char)(host_mask >> 24), (unsigned char)(host_mask >> 16),
                                 (unsigned char)(host_mask >> 8), (unsigned char)host_mask};
    uint32_t mask_be;
    std::memcpy(&mask_be, mb, 4);

    Cidr c;
    c.prefix_be = addr & mask_be;
    c.mask_be = mask_be;
    c.len = static_cast<uint8_t>(len);
    return c;
}

void validate_config(const Config& cfg) {
    if (cfg.interface.empty()) throw std::runtime_error("config: `interface` is required");
    // A /0 would put every IPv4 address in the outer source; it also stands
    // for "not set", since a default-constructed Cidr is 0.0.0.0/0.
    if (cfg.encap_src.len == 0)
        throw std::runtime_error("config: `encap_src_prefix` is required and must not be /0");
    if (cfg.conntrack_size == 0) throw std::runtime_error("config: conntrack.size must be > 0");
    if (cfg.health.interval_ms == 0 || cfg.health.timeout_ms == 0 || cfg.health.fall == 0 ||
        cfg.health.rise == 0)
        throw std::runtime_error("config: health_check values must be > 0");
    if (cfg.health.timeout_ms > cfg.health.interval_ms)
        throw std::runtime_error("config: health_check.timeout_ms must be <= interval_ms");
    if (cfg.vips.size() > PB_MAX_VIPS)
        throw std::runtime_error("config: too many vips (max " + std::to_string(PB_MAX_VIPS) + ")");

    std::set<VipSpec> seen_vips;
    std::set<uint32_t> all_reals;
    for (const VipConfig& v : cfg.vips) {
        if (!seen_vips.insert(v.spec).second)
            throw std::runtime_error("config: duplicate vip " + v.spec.str());
        std::set<uint32_t> seen_reals;
        for (const RealConfig& r : v.reals) {
            if (r.addr_be == 0) throw std::runtime_error("config: real 0.0.0.0 on " + v.spec.str());
            if (!seen_reals.insert(r.addr_be).second)
                throw std::runtime_error("config: duplicate real " + ipv4_to_string(r.addr_be) +
                                         " on " + v.spec.str());
            if (r.weight > kMaxRealWeight)
                throw std::runtime_error("config: weight of " + ipv4_to_string(r.addr_be) +
                                         " exceeds " + std::to_string(kMaxRealWeight));
            all_reals.insert(r.addr_be);
        }
    }
    // real_id 0 is reserved (see the daemon's RealTable), so PB_MAX_REALS - 1 usable.
    if (all_reals.size() > PB_MAX_REALS - 1)
        throw std::runtime_error("config: too many distinct reals (max " +
                                 std::to_string(PB_MAX_REALS - 1) + ")");
}

#ifdef PB_HAVE_YAML

namespace {

// Rejects keys the schema does not know, so a typo fails loudly.
void check_keys(const YAML::Node& node, const std::string& where,
                std::initializer_list<const char*> allowed) {
    if (!node.IsMap()) throw std::runtime_error("config: `" + where + "` must be a mapping");
    for (const auto& kv : node) {
        const std::string key = kv.first.as<std::string>();
        bool ok = false;
        for (const char* a : allowed) ok = ok || key == a;
        if (!ok) throw std::runtime_error("config: unknown key `" + where + key + "`");
    }
}

template <typename T>
T get(const YAML::Node& node, const char* key, const std::string& where, T fallback) {
    const YAML::Node v = node[key];
    if (!v) return fallback;
    try {
        return v.as<T>();
    } catch (const YAML::Exception&) {
        throw std::runtime_error("config: bad value for `" + where + key + "`");
    }
}

template <typename T>
T require(const YAML::Node& node, const char* key, const std::string& where) {
    if (!node[key]) throw std::runtime_error("config: `" + where + key + "` is required");
    return get<T>(node, key, where, T{});
}

// Wraps invalid_argument from the shared parsers with the key it came from.
template <typename F>
auto with_key(const std::string& key, F&& f) {
    try {
        return f();
    } catch (const std::invalid_argument& e) {
        throw std::runtime_error("config: `" + key + "`: " + e.what());
    }
}

uint8_t parse_proto(const std::string& s, const std::string& where) {
    if (s == "tcp") return 6;
    if (s == "udp") return 17;
    throw std::runtime_error("config: `" + where + "proto` must be tcp or udp, got " + s);
}

VipConfig parse_vip(const YAML::Node& n, size_t index) {
    const std::string where = "vips[" + std::to_string(index) + "].";
    check_keys(n, where, {"address", "port", "proto", "no_conntrack", "reals"});

    VipConfig v;
    v.spec.addr_be = with_key(where + "address",
                              [&] { return parse_ipv4(require<std::string>(n, "address", where)); });
    const int port = require<int>(n, "port", where);
    if (port < 1 || port > 65535) throw std::runtime_error("config: `" + where + "port` out of range");
    v.spec.port_be = port_to_be(static_cast<uint16_t>(port));
    v.spec.proto = parse_proto(require<std::string>(n, "proto", where), where);
    v.no_conntrack = get<bool>(n, "no_conntrack", where, false);

    const YAML::Node reals = n["reals"];
    if (reals && !reals.IsNull()) {
        if (!reals.IsSequence()) throw std::runtime_error("config: `" + where + "reals` must be a list");
        for (size_t i = 0; i < reals.size(); ++i) {
            const std::string rwhere = where + "reals[" + std::to_string(i) + "].";
            check_keys(reals[i], rwhere, {"address", "weight"});
            RealConfig r;
            r.addr_be = with_key(rwhere + "address", [&] {
                return parse_ipv4(require<std::string>(reals[i], "address", rwhere));
            });
            const int w = get<int>(reals[i], "weight", rwhere, 1);
            if (w < 0) throw std::runtime_error("config: `" + rwhere + "weight` must be >= 0");
            r.weight = static_cast<uint32_t>(w);
            v.reals.push_back(r);
        }
    }
    return v;
}

}  // namespace

Config parse_config(const std::string& yaml_text) {
    YAML::Node root;
    try {
        root = YAML::Load(yaml_text);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("config: YAML syntax: ") + e.what());
    }
    check_keys(root, "", {"interface", "xdp_mode", "hash", "encap_src_prefix", "next_hop",
                          "icmp_pmtu", "conntrack", "socket", "pin_path", "metrics", "health_check",
                          "vips"});

    Config c;
    c.interface = require<std::string>(root, "interface", "");
    c.xdp_mode = with_key("xdp_mode", [&] {
        return parse_xdp_mode(get<std::string>(root, "xdp_mode", "", "auto"));
    });
    c.hash = with_key("hash", [&] { return parse_hash_mode(get<std::string>(root, "hash", "", "maglev")); });
    c.encap_src = with_key("encap_src_prefix", [&] {
        return parse_cidr(require<std::string>(root, "encap_src_prefix", ""));
    });
    if (root["next_hop"] && !root["next_hop"].IsNull())
        c.next_hop_be = with_key("next_hop", [&] { return parse_ipv4(root["next_hop"].as<std::string>()); });

    c.icmp_pmtu = get<bool>(root, "icmp_pmtu", "", false);

    if (const YAML::Node ct = root["conntrack"]) {
        check_keys(ct, "conntrack.", {"enabled", "size"});
        c.conntrack_enabled = get<bool>(ct, "enabled", "conntrack.", true);
        const long long size = get<long long>(ct, "size", "conntrack.", PB_CT_DEFAULT_SIZE);
        if (size <= 0 || size > (1ll << 27))
            throw std::runtime_error("config: `conntrack.size` must be in 1..134217728");
        c.conntrack_size = static_cast<uint32_t>(size);
    }

    c.socket = get<std::string>(root, "socket", "", c.socket);
    c.pin_path = get<std::string>(root, "pin_path", "", c.pin_path);

    if (const YAML::Node m = root["metrics"]) {
        check_keys(m, "metrics.", {"listen"});
        c.metrics_listen = get<std::string>(m, "listen", "metrics.", c.metrics_listen);
    }

    if (const YAML::Node h = root["health_check"]) {
        check_keys(h, "health_check.", {"enabled", "interval_ms", "timeout_ms", "fall", "rise"});
        c.health.enabled = get<bool>(h, "enabled", "health_check.", true);
        c.health.interval_ms = get<uint32_t>(h, "interval_ms", "health_check.", c.health.interval_ms);
        c.health.timeout_ms = get<uint32_t>(h, "timeout_ms", "health_check.", c.health.timeout_ms);
        c.health.fall = get<uint32_t>(h, "fall", "health_check.", c.health.fall);
        c.health.rise = get<uint32_t>(h, "rise", "health_check.", c.health.rise);
    }

    if (const YAML::Node vips = root["vips"]; vips && !vips.IsNull()) {
        if (!vips.IsSequence()) throw std::runtime_error("config: `vips` must be a list");
        for (size_t i = 0; i < vips.size(); ++i) c.vips.push_back(parse_vip(vips[i], i));
    }

    validate_config(c);
    return c;
}

#else  // !PB_HAVE_YAML

Config parse_config(const std::string&) {
    throw std::runtime_error("config: this build has no yaml-cpp (install libyaml-cpp-dev)");
}

#endif

Config load_config_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("config: cannot open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parse_config(ss.str());
}

}  // namespace pb
