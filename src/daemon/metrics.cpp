// SPDX-License-Identifier: MIT
#include "metrics.h"

#include <format>
#include <map>

#include "counters.h"

namespace pb {

namespace {

class Exposition {
public:
    void family(const char* name, const char* type, const char* help) {
        out_ += std::format("# HELP {} {}\n# TYPE {} {}\n", name, help, name, type);
    }
    void sample(const char* name, const std::string& labels, uint64_t value) {
        out_ += labels.empty() ? std::format("{} {}\n", name, value)
                               : std::format("{}{{{}}} {}\n", name, labels, value);
    }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
};

std::string vip_label(const VipView& v) { return std::format("vip=\"{}\"", v.spec.str()); }

struct PerVipCounter {
    const char* metric;
    pb_counter counter;
    const char* help;
};

constexpr PerVipCounter kPerVip[] = {
    {"pb_packets_total", PB_CNT_PACKETS, "Packets that matched the VIP."},
    {"pb_bytes_total", PB_CNT_BYTES, "Bytes of packets that matched the VIP, before encapsulation."},
    {"pb_conntrack_hits_total", PB_CNT_CT_HIT, "Packets whose real came from the connection table."},
    {"pb_conntrack_misses_total", PB_CNT_CT_MISS, "Connection table lookups that missed."},
    {"pb_hash_total", PB_CNT_HASH, "Packets whose real was chosen by the ring."},
    {"pb_syn_total", PB_CNT_SYN, "TCP SYNs seen."},
    {"pb_tx_total", PB_CNT_TX, "Packets encapsulated and transmitted."},
};

struct DropReason {
    const char* reason;
    pb_counter counter;
};

constexpr DropReason kDrops[] = {
    {"frag", PB_CNT_DROP_FRAG},       {"opts", PB_CNT_DROP_OPTS},   {"no_real", PB_CNT_DROP_NO_REAL},
    {"adj_head", PB_CNT_DROP_ADJ_HEAD}, {"mtu", PB_CNT_DROP_MTU},   {"short", PB_CNT_DROP_SHORT},
    {"other", PB_CNT_DROP_OTHER},
};

}  // namespace

std::string render_metrics(const LbState& state, const MapReader& reader, XdpMode attached_mode) {
    const std::vector<VipView> vips = state.snapshot();
    std::vector<Counters> per_vip;
    for (const VipView& v : vips) per_vip.push_back(reader.counters(v.vip_id));
    const Counters global = reader.counters(PB_STATS_GLOBAL);

    Exposition e;
    for (const PerVipCounter& m : kPerVip) {
        e.family(m.metric, "counter", m.help);
        for (size_t i = 0; i < vips.size(); ++i) e.sample(m.metric, vip_label(vips[i]), per_vip[i][m.counter]);
    }

    e.family("pb_pass_total", "counter", "Packets passed to the kernel stack (not for any VIP).");
    e.sample("pb_pass_total", "", global[PB_CNT_PASS]);

    // Drops before the VIP is known (fragments, IP options, truncated headers)
    // are counted in the global slot and exported as vip="global".
    e.family("pb_drops_total", "counter", "Packets dropped by the data plane, by reason.");
    for (const DropReason& d : kDrops) {
        e.sample("pb_drops_total", std::format("vip=\"global\",reason=\"{}\"", d.reason), global[d.counter]);
        for (size_t i = 0; i < vips.size(); ++i)
            e.sample("pb_drops_total", std::format("{},reason=\"{}\"", vip_label(vips[i]), d.reason),
                     per_vip[i][d.counter]);
    }

    e.family("pb_real_packets_total", "counter", "Packets sent to the real, all VIPs.");
    std::map<uint32_t, uint32_t> seen;  // real_id -> addr, each real once
    for (const VipView& v : vips)
        for (const RealView& r : v.reals) seen[r.real_id] = r.addr;
    for (const auto& [id, addr] : seen)
        e.sample("pb_real_packets_total", std::format("real=\"{}\"", ipv4_to_string(addr)),
                 reader.real_counters(id).packets);

    e.family("pb_real_up", "gauge", "1 if the real passes health checks (always 1 when unchecked).");
    for (const VipView& v : vips)
        for (const RealView& r : v.reals)
            e.sample("pb_real_up", std::format("{},real=\"{}\"", vip_label(v), ipv4_to_string(r.addr)), r.up);

    e.family("pb_real_weight", "gauge", "Configured weight of the real (draining reals keep theirs).");
    for (const VipView& v : vips)
        for (const RealView& r : v.reals)
            e.sample("pb_real_weight", std::format("{},real=\"{}\"", vip_label(v), ipv4_to_string(r.addr)),
                     r.weight);

    e.family("pb_ring_generation", "counter", "Number of ring swaps for the VIP since the daemon started.");
    for (const VipView& v : vips) e.sample("pb_ring_generation", vip_label(v), v.generation);

    e.family("pb_conntrack_entries", "gauge", "Keys in the connection table (O(n) walk per scrape).");
    e.sample("pb_conntrack_entries", "", reader.conntrack_entries());

    e.family("pb_xdp_mode", "gauge", "XDP attach mode in effect.");
    e.sample("pb_xdp_mode", std::format("mode=\"{}\"", xdp_mode_name(attached_mode)), 1);
    return e.take();
}

}  // namespace pb
