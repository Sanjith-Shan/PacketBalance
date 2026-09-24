// SPDX-License-Identifier: MIT
#include "commands.h"

#include <time.h>

#include <map>

#include "counters.h"
#include "log.h"
#include "ring_map.h"

namespace pb {

using nlohmann::json;

namespace {

// ---- argument helpers: every failure becomes an ApiError with a clear text

const json& arg(const json& req, const char* name) {
    if (!req.contains(name)) throw ApiError(std::string("missing argument '") + name + "'");
    return req.at(name);
}

std::string arg_string(const json& req, const char* name) {
    const json& v = arg(req, name);
    if (!v.is_string()) throw ApiError(std::string("argument '") + name + "' must be a string");
    return v.get<std::string>();
}

VipSpec arg_vip(const json& req, const char* name = "vip") {
    try {
        return VipSpec::parse(arg_string(req, name));
    } catch (const std::invalid_argument& e) {
        throw ApiError(e.what());
    }
}

uint32_t arg_addr(const json& req, const char* name = "addr") {
    try {
        return parse_ipv4(arg_string(req, name));
    } catch (const std::invalid_argument& e) {
        throw ApiError(e.what());
    }
}

uint32_t arg_uint(const json& req, const char* name, std::optional<uint32_t> fallback = std::nullopt) {
    if (!req.contains(name) && fallback) return *fallback;
    const json& v = arg(req, name);
    if (!v.is_number_integer() || v.get<int64_t>() < 0 || v.get<int64_t>() > UINT32_MAX)
        throw ApiError(std::string("argument '") + name + "' must be a non-negative integer");
    return v.get<uint32_t>();
}

json counters_json(const Counters& c) {
    json out = json::object();
    for (size_t i = 0; i < c.size(); ++i) out[kCounterNames[i]] = c[i];
    return out;
}

const char* proto_name(uint8_t p) { return p == 6 ? "tcp" : p == 17 ? "udp" : "?"; }

uint64_t monotonic_ns() {
    timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);  // the clock bpf_ktime_get_ns() reads
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

}  // namespace

CommandHandler::CommandHandler(LbState& state, const MapReader& reader, Hooks hooks)
    : state_(state), reader_(reader), hooks_(std::move(hooks)) {}

std::string CommandHandler::handle_line(const std::string& line) {
    json resp;
    try {
        const json req = json::parse(line);
        if (!req.is_object()) throw ApiError("request must be a JSON object");
        resp = {{"ok", true}, {"result", dispatch(req)}};
    } catch (const json::parse_error& e) {
        resp = {{"ok", false}, {"error", std::string("bad json: ") + e.what()}};
    } catch (const ApiError& e) {
        resp = {{"ok", false}, {"error", e.what()}};
    } catch (const std::exception& e) {
        // A map write failed or similar: report it, keep serving.
        log::error("api: {} failed: {}", line, e.what());
        resp = {{"ok", false}, {"error", e.what()}};
    }
    return resp.dump(-1, ' ', false, json::error_handler_t::replace);
}

json CommandHandler::dispatch(const json& req) {
    const std::string cmd = arg_string(req, "cmd");
    log::debug("api: {}", req.dump());

    if (cmd == "ping") return "pong";
    if (cmd == "vip.add") {
        const bool no_ct = req.value("no_conntrack", false);
        return {{"vip_id", state_.add_vip(arg_vip(req), no_ct)}};
    }
    if (cmd == "vip.del") {
        state_.del_vip(arg_vip(req));
        return json::object();
    }
    if (cmd == "vip.list") return vip_list();
    if (cmd == "real.add") {
        const uint32_t id = state_.add_real(arg_vip(req), arg_addr(req), arg_uint(req, "weight", 1));
        if (hooks_.reals_changed) hooks_.reals_changed();
        return {{"real_id", id}};
    }
    if (cmd == "real.del") {
        state_.del_real(arg_vip(req), arg_addr(req));
        return json::object();
    }
    if (cmd == "real.weight") {
        state_.set_weight(arg_vip(req), arg_addr(req), arg_uint(req, "weight"));
        return json::object();
    }
    if (cmd == "real.drain" || cmd == "real.undrain") {
        state_.set_draining(arg_vip(req), arg_addr(req), cmd == "real.drain");
        return json::object();
    }
    if (cmd == "stats") return stats(req);
    if (cmd == "flows") return flows(req);
    if (cmd == "ring.show") return ring_show(req);
    if (cmd == "health") return health();
    if (cmd == "reload") {
        hooks_.reload();
        if (hooks_.reals_changed) hooks_.reals_changed();
        return json::object();
    }
    if (cmd == "config") return hooks_.config();
    throw ApiError("unknown command '" + cmd + "'");
}

json CommandHandler::vip_list() const {
    json out = json::array();
    for (const VipView& v : state_.snapshot()) {
        json reals = json::array();
        for (const RealView& r : v.reals)
            reals.push_back({{"addr", ipv4_to_string(r.addr)},
                             {"real_id", r.real_id},
                             {"weight", r.weight},
                             {"up", r.up},
                             {"draining", r.draining},
                             {"in_ring", r.in_ring},
                             {"mac", r.mac ? json(r.mac->str()) : json(nullptr)}});
        out.push_back({{"vip", v.spec.str()},
                       {"vip_id", v.vip_id},
                       {"flags", v.flags},
                       {"generation", v.generation},
                       {"reals", reals}});
    }
    return out;
}

json CommandHandler::stats(const json& req) const {
    std::optional<VipSpec> only;
    if (req.contains("vip")) only = arg_vip(req);

    json vips = json::object();
    json reals = json::object();
    bool found = false;
    for (const VipView& v : state_.snapshot()) {
        if (only && !(v.spec == *only)) continue;
        found = true;
        vips[v.spec.str()] = counters_json(reader_.counters(v.vip_id));
        for (const RealView& r : v.reals) {
            const pb_real_stats rs = reader_.real_counters(r.real_id);
            reals[ipv4_to_string(r.addr)] = {{"packets", rs.packets}, {"bytes", rs.bytes}};
        }
    }
    if (only && !found) throw ApiError("unknown vip " + only->str());
    return {{"global", counters_json(reader_.counters(PB_STATS_GLOBAL))}, {"vips", vips}, {"reals", reals}};
}

json CommandHandler::flows(const json& req) const {
    std::optional<VipSpec> vip;
    std::optional<uint32_t> real_id;
    if (req.contains("vip")) vip = arg_vip(req);
    if (req.contains("real")) {
        const uint32_t addr = arg_addr(req, "real");
        real_id = state_.real_id_of(addr);
        if (!real_id) throw ApiError("unknown real " + ipv4_to_string(addr));
    }
    const uint32_t limit = arg_uint(req, "limit", 1000);

    auto keep = [&](const pb_ct_key& k, const pb_ct_value& v) {
        if (vip && (k.daddr != vip->addr_be || k.dport != vip->port_be || k.proto != vip->proto))
            return false;
        return !real_id || v.real_id == *real_id;
    };
    const auto names = state_.real_addrs();
    const uint64_t now = monotonic_ns();
    json out = json::array();
    for (const FlowRow& f : reader_.flows(keep, limit)) {
        auto it = names.find(f.value.real_id);
        const VipSpec vs{f.key.daddr, f.key.dport, f.key.proto};
        out.push_back({{"src", ipv4_to_string(f.key.saddr)},
                       {"dst", ipv4_to_string(f.key.daddr)},
                       {"sport", port_from_be(f.key.sport)},
                       {"dport", port_from_be(f.key.dport)},
                       {"proto", proto_name(f.key.proto)},
                       {"real", it != names.end() ? ipv4_to_string(it->second)
                                                  : "#" + std::to_string(f.value.real_id)},
                       {"vip", vs.str()},
                       {"age_ms", now > f.value.last_seen_ns ? (now - f.value.last_seen_ns) / 1'000'000 : 0},
                       {"cpu", f.cpu}});
    }
    return out;
}

json CommandHandler::ring_show(const json& req) const {
    const VipSpec spec = arg_vip(req);
    const auto view = state_.find(spec);
    if (!view) throw ApiError("unknown vip " + spec.str());

    // real_id -> addr taken after the ring read would be a hair more exact,
    // but ids only change under real.del, which rebuilds the ring first.
    const auto names = state_.real_addrs();
    const std::vector<uint32_t> ring = read_ring(reader_.rings_fd(), view->vip_id);
    std::map<uint32_t, uint64_t> by_id;
    for (uint32_t id : ring) ++by_id[id];

    json slots = json::object();
    slots["none"] = 0;
    for (const auto& [id, n] : by_id) {
        if (id == PB_REAL_NONE) {
            slots["none"] = n;
            continue;
        }
        auto it = names.find(id);
        slots[it != names.end() ? ipv4_to_string(it->second) : "#" + std::to_string(id)] = n;
    }
    return {{"size", ring.size()},
            {"hash", hash_mode_name(state_.hash_mode())},
            {"slots", slots},
            {"generation", view->generation}};
}

json CommandHandler::health() const {
    json out = json::array();
    for (const VipView& v : state_.snapshot())
        for (const RealView& r : v.reals)
            out.push_back({{"vip", v.spec.str()},
                           {"addr", ipv4_to_string(r.addr)},
                           {"up", r.up},
                           {"checked", r.checked},
                           {"consecutive_ok", r.consecutive_ok},
                           {"consecutive_fail", r.consecutive_fail},
                           {"last_change_ms", r.last_change_ms},
                           {"last_rtt_us", r.last_rtt_us}});
    return out;
}

}  // namespace pb
