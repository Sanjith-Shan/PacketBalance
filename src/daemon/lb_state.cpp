// SPDX-License-Identifier: MIT
#include "lb_state.h"

#include <bpf/bpf.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <set>

#include "log.h"
#include "ring_map.h"

namespace pb {

namespace {

int64_t epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string errstr(int err) { return std::strerror(err < 0 ? -err : err); }

Mac read_interface_mac(const std::string& ifname) {
    std::ifstream in("/sys/class/net/" + ifname + "/address");
    std::string text;
    in >> text;
    auto mac = Mac::parse(text);
    if (!mac) throw std::runtime_error("cannot read MAC of " + ifname);
    return *mac;
}

}  // namespace

LbState::LbState(const MapFds& fds, const MapReader& reader, std::string interface, MacLookup lookup)
    : fds_(fds),
      reader_(reader),
      interface_(std::move(interface)),
      mac_lookup_(std::move(lookup)),
      reals_(fds.reals, fds.neigh, reader) {}

// ---------------------------------------------------------------------------
// Configuration

void LbState::adopt_locked() {
    reals_.adopt();
    pb_vip_key key{}, next{};
    bool first = true;
    while (bpf_map_get_next_key(fds_.vip_map, first ? nullptr : &key, &next) == 0) {
        first = false;
        key = next;
        pb_vip_value value{};
        if (bpf_map_lookup_elem(fds_.vip_map, &key, &value) != 0) continue;
        VipSpec spec{.addr_be = key.addr, .port_be = key.port, .proto = key.proto};
        adopted_vip_ids_[spec] = value.vip_id;
        log::info("adopted vip {} = vip_id {} from pinned maps", spec.str(), value.vip_id);
    }
}

void LbState::write_config_map(const Config& cfg) {
    pb_config c{};
    const Mac mac = read_interface_mac(interface_);
    std::memcpy(c.lb_mac, mac.bytes.data(), 6);
    c.encap_src_prefix = cfg.encap_src.prefix_be;
    c.encap_src_mask = cfg.encap_src.mask_be;
    c.flags = cfg.conntrack_enabled ? 0 : PB_CFG_F_NO_CONNTRACK;
    const uint32_t zero = 0;
    if (int err = bpf_map_update_elem(fds_.config, &zero, &c, BPF_ANY))
        throw std::runtime_error("write config map: " + errstr(err));
    log::info("config map: lb_mac={} encap_src={} conntrack={}", mac.str(), cfg.encap_src.str(),
              cfg.conntrack_enabled ? "on" : "off (hash every packet)");
}

void LbState::apply(const Config& cfg) {
    std::lock_guard lock(mu_);
    const bool first = !initialized_;
    if (first) adopt_locked();

    write_config_map(cfg);
    health_enabled_ = cfg.health.enabled;
    next_hop_ = cfg.next_hop_be;
    const bool hash_changed = hash_ != cfg.hash;
    if (hash_changed && !first)
        log::info("hash mode {} -> {}: rebuilding every ring", hash_mode_name(hash_), hash_mode_name(cfg.hash));
    hash_ = cfg.hash;

    std::set<VipSpec> wanted;
    for (const VipConfig& vc : cfg.vips) wanted.insert(vc.spec);
    for (auto it = vips_.begin(); it != vips_.end();) {
        const VipSpec spec = (it++)->first;
        if (!wanted.count(spec)) delete_vip_locked(spec);
    }

    for (const VipConfig& vc : cfg.vips) {
        const uint32_t flags = vc.no_conntrack ? PB_VIP_F_NO_CONNTRACK : 0;
        std::map<uint32_t, uint32_t> want;  // addr -> weight
        for (const RealConfig& r : vc.reals) want[r.addr_be] = r.weight;

        auto it = vips_.find(vc.spec);
        if (it == vips_.end()) {
            create_vip_locked(vc.spec, flags, want);
            continue;
        }
        Vip& v = it->second;
        if (v.flags != flags) {
            v.flags = flags;
            write_vip_entry(v);
        }
        std::vector<uint32_t> removed;
        for (const auto& [addr, m] : v.members)
            if (!want.count(addr)) removed.push_back(addr);
        for (uint32_t addr : removed) remove_member_locked(v, addr);
        bool changed = hash_changed || !removed.empty();
        for (const auto& [addr, weight] : want) {
            auto m = v.members.find(addr);
            if (m == v.members.end()) {
                add_member_locked(v, addr, weight);
                changed = true;
            } else if (m->second.weight != weight) {
                log::info("real {} on {}: weight {} -> {}", ipv4_to_string(addr), v.spec.str(),
                          m->second.weight, weight);
                m->second.weight = weight;
                changed = true;
            }
        }
        if (changed) rebuild_locked(v, "reload");
        for (uint32_t addr : removed) reals_.release(addr);  // after the new ring is live
    }

    if (first) {
        // Pinned VIPs the new config does not have: remove from the data plane.
        for (const auto& [spec, id] : adopted_vip_ids_) {
            if (vips_.count(spec)) continue;
            log::info("removing pinned vip {} (vip_id {}): not in the config", spec.str(), id);
            const pb_vip_key key = spec.key();
            bpf_map_delete_elem(fds_.vip_map, &key);
            remove_ring(fds_.rings, id);
        }
        adopted_vip_ids_.clear();
        reals_.drop_unreferenced();
        initialized_ = true;
    }
}

// ---------------------------------------------------------------------------
// VIPs

uint32_t LbState::allocate_vip_id() const {
    std::set<uint32_t> used;
    for (const auto& [spec, v] : vips_) used.insert(v.id);
    for (const auto& [spec, id] : adopted_vip_ids_) used.insert(id);
    for (uint32_t id = 0; id < PB_MAX_VIPS; ++id)
        if (!used.count(id)) return id;
    throw ApiError("too many vips (max " + std::to_string(PB_MAX_VIPS) + ")");
}

// The ring is installed BEFORE the vip_map entry, so the data plane can never
// match a VIP that has no ring, and it is built with the VIP's reals already in
// it: after a restart the adopted VIP is live in vip_map the whole time, and an
// empty ring even for a millisecond would drop its new connections.
// Deletion goes in the opposite order.
LbState::Vip& LbState::create_vip_locked(const VipSpec& spec, uint32_t flags,
                                         const std::map<uint32_t, uint32_t>& reals) {
    Vip v;
    v.spec = spec;
    v.flags = flags;
    const char* reason = "new vip";
    if (auto it = adopted_vip_ids_.find(spec); it != adopted_vip_ids_.end()) {
        v.id = it->second;  // keep the id and the counters across a restart
        adopted_vip_ids_.erase(it);
        reason = "startup (adopted)";
    } else {
        v.id = allocate_vip_id();
        reader_.reset_counters(v.id);
    }
    Vip& stored = vips_.emplace(spec, std::move(v)).first->second;
    log::info("vip {} is vip_id {}{}", spec.str(), stored.id,
              flags & PB_VIP_F_NO_CONNTRACK ? " (no conntrack)" : "");
    for (const auto& [addr, weight] : reals) add_member_locked(stored, addr, weight);
    rebuild_locked(stored, reason);
    write_vip_entry(stored);
    return stored;
}

void LbState::write_vip_entry(const Vip& v) {
    const pb_vip_key key = v.spec.key();
    const pb_vip_value value{.vip_id = v.id, .flags = v.flags};
    if (int err = bpf_map_update_elem(fds_.vip_map, &key, &value, BPF_ANY))
        throw std::runtime_error("write vip_map for " + v.spec.str() + ": " + errstr(err));
}

void LbState::delete_vip_locked(const VipSpec& spec) {
    auto it = vips_.find(spec);
    if (it == vips_.end()) return;
    Vip& v = it->second;
    const pb_vip_key key = spec.key();
    bpf_map_delete_elem(fds_.vip_map, &key);
    remove_ring(fds_.rings, v.id);
    for (const auto& [addr, m] : v.members) reals_.release(addr);
    log::info("vip {} (vip_id {}) removed", spec.str(), v.id);
    vips_.erase(it);
}

uint32_t LbState::add_vip(const VipSpec& vip, bool no_conntrack) {
    std::lock_guard lock(mu_);
    if (vips_.count(vip)) throw ApiError("vip already exists");
    return create_vip_locked(vip, no_conntrack ? PB_VIP_F_NO_CONNTRACK : 0, {}).id;
}

void LbState::del_vip(const VipSpec& vip) {
    std::lock_guard lock(mu_);
    vip_or_throw(vip);
    delete_vip_locked(vip);
}

// ---------------------------------------------------------------------------
// Reals

void LbState::add_member_locked(Vip& v, uint32_t addr, uint32_t weight) {
    reals_.acquire(addr);  // writes reals[id] before any ring can reference it
    // A real that is new to the table has no MAC yet and would get no ring
    // slots until the resolver's next pass. The kernel usually knows it
    // already (health checks, earlier traffic), so ask it now.
    if (!reals_.mac_of(addr) && mac_lookup_) {
        if (auto mac = mac_lookup_(next_hop_.value_or(addr))) reals_.set_mac(addr, *mac);
    }
    Member m;
    m.weight = weight;
    m.last_change_ms = epoch_ms();
    m.incarnation = next_incarnation_++;
    v.members.emplace(addr, m);
    log::info("real {} added to {} (real_id {}, weight {})", ipv4_to_string(addr), v.spec.str(),
              *reals_.id_of(addr), weight);
}

// Removes the real from the VIP's model only. The caller rebuilds the ring and
// THEN releases the real_id: in the other order a packet could hash to an id
// whose `reals` slot is already empty.
void LbState::remove_member_locked(Vip& v, uint32_t addr) {
    v.members.erase(addr);
    log::info("real {} removed from {}", ipv4_to_string(addr), v.spec.str());
}

uint32_t LbState::add_real(const VipSpec& vip, uint32_t addr, uint32_t weight) {
    std::lock_guard lock(mu_);
    Vip& v = vip_or_throw(vip);
    if (v.members.count(addr)) throw ApiError("real already exists");
    if (weight > kMaxRealWeight) throw ApiError("weight must be 0.." + std::to_string(kMaxRealWeight));
    add_member_locked(v, addr, weight);
    rebuild_locked(v, "real.add");
    return *reals_.id_of(addr);
}

void LbState::del_real(const VipSpec& vip, uint32_t addr) {
    std::lock_guard lock(mu_);
    Vip& v = vip_or_throw(vip);
    member_or_throw(v, addr);
    remove_member_locked(v, addr);
    rebuild_locked(v, "real.del");
    reals_.release(addr);
}

void LbState::set_weight(const VipSpec& vip, uint32_t addr, uint32_t weight) {
    std::lock_guard lock(mu_);
    if (weight > kMaxRealWeight) throw ApiError("weight must be 0.." + std::to_string(kMaxRealWeight));
    Vip& v = vip_or_throw(vip);
    Member& m = member_or_throw(v, addr);
    if (m.weight == weight) return;
    log::info("real {} on {}: weight {} -> {}", ipv4_to_string(addr), vip.str(), m.weight, weight);
    m.weight = weight;
    rebuild_locked(v, "real.weight");
}

void LbState::set_draining(const VipSpec& vip, uint32_t addr, bool draining) {
    std::lock_guard lock(mu_);
    Vip& v = vip_or_throw(vip);
    Member& m = member_or_throw(v, addr);
    if (m.draining == draining) return;
    m.draining = draining;
    log::info("real {} on {}: {}", ipv4_to_string(addr), vip.str(),
              draining ? "draining (no new flows; tracked flows continue)" : "undrained");
    rebuild_locked(v, draining ? "real.drain" : "real.undrain");
}

// ---------------------------------------------------------------------------
// Rings

bool LbState::eligible(uint32_t addr, const Member& m) const {
    return m.weight > 0 && !m.draining && m.up && reals_.mac_of(addr).has_value();
}

void LbState::rebuild_locked(Vip& v, const char* reason) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Backend> backends;
    size_t active = 0;
    for (auto& [addr, m] : v.members) {
        m.in_ring = eligible(addr, m);
        active += m.in_ring;
        backends.push_back(Backend{*reals_.id_of(addr), addr, m.in_ring ? m.weight : 0});
    }
    install_ring(fds_.rings, v.id, build_ring(backends, hash_));
    ++v.generation;
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log::info("ring: vip={} generation={} hash={} reals_in_ring={}/{} reason={} built+swapped in {:.1f} ms",
              v.spec.str(), v.generation, hash_mode_name(hash_), active, v.members.size(), reason, ms);
}

// ---------------------------------------------------------------------------
// Health

std::vector<HealthTarget> LbState::health_targets() const {
    std::lock_guard lock(mu_);
    std::vector<HealthTarget> out;
    if (!health_enabled_) return out;
    for (const auto& [spec, v] : vips_) {
        if (spec.proto != 6) continue;  // UDP: nothing to connect to, always UP
        for (const auto& [addr, m] : v.members) out.push_back({spec, addr, m.incarnation});
    }
    return out;
}

void LbState::report_health(const HealthTarget& t, const ProbeResult& r, uint32_t rise, uint32_t fall) {
    std::lock_guard lock(mu_);
    auto vit = vips_.find(t.vip);
    if (vit == vips_.end()) return;
    Vip& v = vit->second;
    auto mit = v.members.find(t.addr);
    // Deleted (or deleted and re-added) while the probe was in flight.
    if (mit == v.members.end() || mit->second.incarnation != t.incarnation) return;
    Member& m = mit->second;

    const std::string real = ipv4_to_string(t.addr);
    if (r.ok) {
        m.last_rtt_us = r.rtt_us;
        m.consecutive_fail = 0;
        ++m.consecutive_ok;
        log::debug("health: vip={} real={} ok rtt={}us", t.vip.str(), real, r.rtt_us);
        if (!m.up && m.consecutive_ok >= rise) {
            m.up = true;
            m.last_change_ms = epoch_ms();
            log::info("health: vip={} real={} DOWN->UP after {} successes", t.vip.str(), real,
                      m.consecutive_ok);
            rebuild_locked(v, "health");
        }
    } else {
        m.consecutive_ok = 0;
        ++m.consecutive_fail;
        if (m.up)
            log::info("health: vip={} real={} check failed ({}/{}): {}", t.vip.str(), real,
                      m.consecutive_fail, fall, r.detail);
        if (m.up && m.consecutive_fail >= fall) {
            m.up = false;
            m.last_change_ms = epoch_ms();
            log::info("health: vip={} real={} UP->DOWN after {} failures ({})", t.vip.str(), real,
                      m.consecutive_fail, r.detail);
            rebuild_locked(v, "health");
        }
    }
}

// ---------------------------------------------------------------------------
// Neighbours

std::vector<uint32_t> LbState::neighbor_targets() const {
    std::lock_guard lock(mu_);
    if (next_hop_) return {*next_hop_};
    return reals_.addrs();
}

std::vector<uint32_t> LbState::unresolved_neighbors() const {
    std::lock_guard lock(mu_);
    std::vector<uint32_t> out;
    for (uint32_t addr : reals_.addrs()) {
        if (reals_.mac_of(addr)) continue;
        out.push_back(next_hop_ ? *next_hop_ : addr);
        if (next_hop_) break;
    }
    return out;
}

void LbState::neighbor_resolved(uint32_t ip, const Mac& mac) {
    std::lock_guard lock(mu_);
    std::set<uint32_t> newly_usable;
    auto set_one = [&](uint32_t addr) {
        const bool was_known = reals_.mac_of(addr).has_value();
        if (!reals_.set_mac(addr, mac)) return;
        log::info("neigh: real {} -> {}{}", ipv4_to_string(addr), mac.str(),
                  next_hop_ ? " (next hop " + ipv4_to_string(*next_hop_) + ")" : "");
        if (!was_known) newly_usable.insert(addr);
    };
    if (next_hop_) {
        if (ip != *next_hop_) return;
        for (uint32_t addr : reals_.addrs()) set_one(addr);
    } else {
        set_one(ip);
    }
    if (newly_usable.empty()) return;
    for (auto& [spec, v] : vips_) {
        for (const auto& [addr, m] : v.members)
            if (newly_usable.count(addr)) {
                rebuild_locked(v, "neighbor resolved");
                break;
            }
    }
}

// ---------------------------------------------------------------------------
// Views

RealView LbState::view_of(const Vip& v, uint32_t addr, const Member& m) const {
    RealView r;
    r.addr = addr;
    r.real_id = reals_.id_of(addr).value_or(0);
    r.weight = m.weight;
    r.up = m.up;
    r.draining = m.draining;
    r.checked = health_enabled_ && v.spec.proto == 6;
    r.in_ring = m.in_ring;
    r.mac = reals_.mac_of(addr);
    r.consecutive_ok = m.consecutive_ok;
    r.consecutive_fail = m.consecutive_fail;
    r.last_change_ms = m.last_change_ms;
    r.last_rtt_us = m.last_rtt_us;
    return r;
}

std::vector<VipView> LbState::snapshot() const {
    std::lock_guard lock(mu_);
    std::vector<VipView> out;
    for (const auto& [spec, v] : vips_) {
        VipView view{spec, v.id, v.flags, v.generation, {}};
        for (const auto& [addr, m] : v.members) view.reals.push_back(view_of(v, addr, m));
        out.push_back(std::move(view));
    }
    std::sort(out.begin(), out.end(), [](const VipView& a, const VipView& b) { return a.vip_id < b.vip_id; });
    return out;
}

std::optional<VipView> LbState::find(const VipSpec& vip) const {
    for (VipView& v : snapshot())
        if (v.spec == vip) return std::move(v);
    return std::nullopt;
}

std::map<uint32_t, uint32_t> LbState::real_addrs() const {
    std::lock_guard lock(mu_);
    std::map<uint32_t, uint32_t> out;
    for (uint32_t addr : reals_.addrs()) out[*reals_.id_of(addr)] = addr;
    return out;
}

std::optional<uint32_t> LbState::real_id_of(uint32_t addr) const {
    std::lock_guard lock(mu_);
    return reals_.id_of(addr);
}

HashMode LbState::hash_mode() const {
    std::lock_guard lock(mu_);
    return hash_;
}

LbState::Vip& LbState::vip_or_throw(const VipSpec& spec) {
    auto it = vips_.find(spec);
    if (it == vips_.end()) throw ApiError("unknown vip " + spec.str());
    return it->second;
}

const LbState::Vip& LbState::vip_or_throw(const VipSpec& spec) const {
    auto it = vips_.find(spec);
    if (it == vips_.end()) throw ApiError("unknown vip " + spec.str());
    return it->second;
}

LbState::Member& LbState::member_or_throw(Vip& v, uint32_t addr) {
    auto it = v.members.find(addr);
    if (it == v.members.end())
        throw ApiError("unknown real " + ipv4_to_string(addr) + " on vip " + v.spec.str());
    return it->second;
}

}  // namespace pb
