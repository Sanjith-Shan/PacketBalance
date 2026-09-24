// SPDX-License-Identifier: MIT
//
// The control plane's model of the load balancer and the only code that
// writes the control-plane maps (vip_map, rings, reals, neigh, config).
//
// Three threads mutate it: the API (vip/real commands, reload), the health
// checker (UP/DOWN transitions) and the neighbour resolver (MACs). Every public
// method takes one mutex, so a transition and an API change to the same VIP
// cannot interleave between "compute the ring" and "swap it in": the ring that
// lands in the kernel always reflects the latest model.
//
// A real's slot share in its VIP's ring is its weight if it is eligible and
// zero otherwise. Eligible = health UP, not draining, and its next-hop MAC is
// known (a real we cannot address would black-hole its share).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "loader.h"
#include "map_reader.h"
#include "packetbalance/config.h"
#include "packetbalance/maglev.h"
#include "packetbalance/vipspec.h"
#include "real_table.h"

namespace pb {

// Error with a message fit for an API response ("unknown vip ...").
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct HealthTarget {
    VipSpec vip;
    uint32_t addr;
    uint64_t incarnation;  // guards against a real deleted and re-added mid-probe
};

struct ProbeResult {
    bool ok = false;
    uint32_t rtt_us = 0;
    std::string detail;  // e.g. "Connection refused", "timeout"
    // The probe could not be made for a reason local to the LB (out of file
    // descriptors, ephemeral ports or buffers). Not evidence about the real,
    // so it is not counted either way.
    bool local_error = false;
};

struct RealView {
    uint32_t addr = 0;
    uint32_t real_id = 0;
    uint32_t weight = 0;
    bool up = true;
    bool draining = false;
    bool checked = false;  // false for UDP VIPs or with health checks off
    bool in_ring = false;
    std::optional<Mac> mac;
    uint32_t consecutive_ok = 0;
    uint32_t consecutive_fail = 0;
    int64_t last_change_ms = 0;  // Unix epoch ms of the last UP/DOWN change (or of creation)
    uint32_t last_rtt_us = 0;
};

struct VipView {
    VipSpec spec;
    uint32_t vip_id = 0;
    uint32_t flags = 0;
    uint64_t generation = 0;
    std::vector<RealView> reals;
};

class LbState {
public:
    // Looks an IP up in the kernel's neighbour table without probing.
    using MacLookup = std::function<std::optional<Mac>(uint32_t ip)>;

    LbState(const MapFds& fds, const MapReader& reader, std::string interface, MacLookup lookup);

    // Makes the maps match `cfg`: config map, hash mode, VIPs, reals and
    // weights. The first call adopts what a previous daemon left in the pinned
    // maps (vip_ids, real_ids, MACs) so tracked flows keep their meaning.
    // Later calls (reload) apply only the difference; health and drain state
    // of reals that stay are kept, and the connection table is never touched.
    void apply(const Config& cfg);

    // API mutations. Throw ApiError.
    uint32_t add_vip(const VipSpec& vip, bool no_conntrack);
    void del_vip(const VipSpec& vip);
    uint32_t add_real(const VipSpec& vip, uint32_t addr, uint32_t weight);
    void del_real(const VipSpec& vip, uint32_t addr);
    void set_weight(const VipSpec& vip, uint32_t addr, uint32_t weight);
    void set_draining(const VipSpec& vip, uint32_t addr, bool draining);

    // Health checker interface.
    std::vector<HealthTarget> health_targets() const;
    void report_health(const HealthTarget& target, const ProbeResult& result, uint32_t rise,
                       uint32_t fall);

    // Neighbour resolver interface: the IPs whose MAC we need (the next hop,
    // or every real), and the answer.
    std::vector<uint32_t> neighbor_targets() const;
    std::vector<uint32_t> unresolved_neighbors() const;
    void neighbor_resolved(uint32_t ip, const Mac& mac);

    // Read-only views.
    std::vector<VipView> snapshot() const;
    std::optional<VipView> find(const VipSpec& vip) const;
    std::map<uint32_t, uint32_t> real_addrs() const;  // real_id -> addr
    std::optional<uint32_t> real_id_of(uint32_t addr) const;
    HashMode hash_mode() const;

private:
    struct Member {
        uint32_t weight = 1;
        bool draining = false;
        bool up = true;  // new reals start UP; see HealthChecker
        uint32_t consecutive_ok = 0;
        uint32_t consecutive_fail = 0;
        int64_t last_change_ms = 0;
        uint32_t last_rtt_us = 0;
        bool in_ring = false;
        uint64_t incarnation = 0;
    };
    struct Vip {
        VipSpec spec;
        uint32_t id = 0;
        uint32_t flags = 0;
        uint64_t generation = 0;
        std::map<uint32_t, Member> members;  // by addr
    };

    void adopt_locked();
    void write_config_map(const Config& cfg);
    Vip& create_vip_locked(const VipSpec& spec, uint32_t flags,
                           const std::map<uint32_t, uint32_t>& reals);  // addr -> weight
    void delete_vip_locked(const VipSpec& spec);
    void write_vip_entry(const Vip& v);
    void add_member_locked(Vip& v, uint32_t addr, uint32_t weight);
    void remove_member_locked(Vip& v, uint32_t addr);
    bool eligible(uint32_t addr, const Member& m) const;
    void rebuild_locked(Vip& v, const char* reason);
    uint32_t allocate_vip_id() const;
    Vip& vip_or_throw(const VipSpec& spec);
    const Vip& vip_or_throw(const VipSpec& spec) const;
    Member& member_or_throw(Vip& v, uint32_t addr);
    RealView view_of(const Vip& v, uint32_t addr, const Member& m) const;

    mutable std::mutex mu_;
    MapFds fds_;
    const MapReader& reader_;
    std::string interface_;
    MacLookup mac_lookup_;
    RealTable reals_;
    std::map<VipSpec, Vip> vips_;
    std::map<VipSpec, uint32_t> adopted_vip_ids_;  // from pinned vip_map, first apply only
    HashMode hash_ = HashMode::Maglev;
    std::optional<uint32_t> next_hop_;
    bool health_enabled_ = true;
    bool initialized_ = false;
    uint64_t next_incarnation_ = 1;
};

}  // namespace pb
