// SPDX-License-Identifier: (GPL-2.0-only OR MIT)
//
// PacketBalance XDP data plane.
//
// One XDP program, attached to the load balancer's interface. For every frame:
//
//   parse Ethernet + IPv4 (+ TCP/UDP, or ICMP for PMTU)
//     -> vip_map lookup on (daddr, dport, proto)        miss: XDP_PASS
//     -> connection table (per-CPU LRU) or Maglev ring  pick a real_id
//     -> prepend an outer IPv4 header (IP-in-IP) addressed to the real
//     -> XDP_TX back out the same interface             "load balancer on a stick"
//
// The design follows the one Meta published for Katran (Engineering blog,
// "Open-sourcing Katran, a scalable network load balancer", 2018): XDP,
// Maglev consistent hashing, an LRU flow table that is allowed to miss,
// IPIP direct server return, and an RSS-friendly outer source address. This
// file is written from that design, not from Katran's source.
//
// Byte order: every address and port in a map is in network byte order, the
// same as on the wire, so no field is swapped on the lookup path. The only
// swaps are of tot_len (to do arithmetic on it) and of constants.

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/icmp.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#include "packetbalance/abi.h"
#include "packetbalance/hash.h"

char LICENSE[] SEC("license") = "GPL";

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, PB_MAX_VIPS);
    __type(key, struct pb_vip_key);
    __type(value, struct pb_vip_value);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} vip_map SEC(".maps");

// Inner ring template: one slot per index, value real_id or PB_REAL_NONE.
struct ring_map {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, PB_RING_SIZE);
    __type(key, __u32);
    __type(value, __u32);
};

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY_OF_MAPS);
    __uint(max_entries, PB_MAX_VIPS);
    __type(key, __u32);
    __array(values, struct ring_map);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} rings SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, PB_MAX_REALS);
    __type(key, __u32);
    __type(value, struct pb_real);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} reals SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, PB_MAX_REALS);
    __type(key, __u32);
    __type(value, struct pb_mac);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} neigh SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_PERCPU_HASH);
    __uint(max_entries, PB_CT_DEFAULT_SIZE);   // daemon may resize before load
    __type(key, struct pb_ct_key);
    __type(value, struct pb_ct_value);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} conntrack SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, PB_MAX_VIPS + 1);
    __type(key, __u32);
    __type(value, struct pb_stats);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, PB_MAX_REALS);
    __type(key, __u32);
    __type(value, struct pb_real_stats);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} real_stats SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct pb_config);
    __uint(pinning, LIBBPF_PIN_BY_NAME);
} config SEC(".maps");

#define PB_OUTER_LEN   ((int)sizeof(struct iphdr))   // 20 bytes of IPIP overhead
#define PB_IP_MF       0x2000
#define PB_IP_OFFSET   0x1FFF
#define PB_OUTER_TTL   64

// The flow as the load balancer sees it: client -> VIP. For an ICMP PMTU
// message this is reconstructed from the quoted inner header.
struct pb_flow {
    __u32 saddr;
    __u32 daddr;
    __u16 sport;
    __u16 dport;
    __u8  proto;
};

// ------------------------------------------------------------------------
// Counters. Per-CPU arrays, so plain increments: no other CPU writes this
// CPU's copy and the control plane sums the copies when it reads.
// ------------------------------------------------------------------------

static __always_inline struct pb_stats *stats_slot(__u32 idx)
{
    return bpf_map_lookup_elem(&stats, &idx);
}

static __always_inline void count_global(int cnt)
{
    struct pb_stats *st = stats_slot(PB_STATS_GLOBAL);
    if (st)
        st->c[cnt]++;
}

static __always_inline int drop_global(int cnt)
{
    count_global(cnt);
    return XDP_DROP;
}

static __always_inline int pass_global(void)
{
    count_global(PB_CNT_PASS);
    return XDP_PASS;
}

static __always_inline int drop_vip(struct pb_stats *st, int cnt)
{
    if (st)
        st->c[cnt]++;
    return XDP_DROP;
}

// ------------------------------------------------------------------------
// Real selection.
// ------------------------------------------------------------------------

// Maglev ring lookup: rings[vip_id][hash % PB_RING_SIZE]. The inner map is
// swapped whole by the control plane (one pointer store in the outer
// ARRAY_OF_MAPS), so a packet sees either the old ring or the new one, never
// a half-written mix.
static __always_inline __u32 ring_lookup(__u32 vip_id, __u32 hash)
{
    void *ring = bpf_map_lookup_elem(&rings, &vip_id);
    if (!ring)
        return PB_REAL_NONE;
    __u32 slot = hash % PB_RING_SIZE;
    __u32 *rid = bpf_map_lookup_elem(ring, &slot);
    if (!rid)
        return PB_REAL_NONE;
    return *rid;
}

// Pick the real for a flow.
//
//   use_ct     connection table enabled for this VIP and daemon-wide
//   bare_syn   TCP SYN without ACK: a new connection, always hash
//   may_insert write the result into the connection table (false for ICMP,
//              which must not create state for a flow it is only about)
//
// FIN and RST do not delete the entry. Katran does the same. Deleting buys
// nothing, since the LRU evicts idle entries on its own when the table is
// full, and it costs correctness: a retransmitted FIN, or the final ACK of
// the close, arriving after the delete would miss, re-hash, and if the ring
// changed during the connection go to a different real that answers RST.
static __always_inline __u32 pick_real(const struct pb_flow *f, __u32 vip_id,
                                       __u32 hash, int use_ct, int bare_syn,
                                       int may_insert, struct pb_stats *st)
{
    struct pb_ct_key key;
    __u32 real_id;

    if (!use_ct) {
        if (st)
            st->c[PB_CNT_HASH]++;
        return ring_lookup(vip_id, hash);
    }

    __builtin_memset(&key, 0, sizeof(key));   // pad bytes are part of the key
    key.saddr = f->saddr;
    key.daddr = f->daddr;
    key.sport = f->sport;
    key.dport = f->dport;
    key.proto = f->proto;

    if (!bare_syn) {
        struct pb_ct_value *ct = bpf_map_lookup_elem(&conntrack, &key);
        // LRU_PERCPU_HASH shares the key across CPUs but keeps one value per
        // CPU. When CPU A inserts a key, the kernel zero-fills the value of
        // every other CPU. A lookup from CPU B then finds the key and returns
        // B's all-zero value: real_id 0, last_seen 0. That is not a hit, it
        // is "some other CPU saw this flow". bpf_ktime_get_ns() is never 0
        // after boot, so last_seen_ns == 0 is the marker of that case and it
        // falls through to the hash, which is deterministic and picks the
        // same real as CPU A did unless the ring changed.
        if (ct && ct->last_seen_ns != 0) {
            ct->last_seen_ns = bpf_ktime_get_ns();
            if (st)
                st->c[PB_CNT_CT_HIT]++;
            return ct->real_id;
        }
        if (st)
            st->c[PB_CNT_CT_MISS]++;
    }

    if (st)
        st->c[PB_CNT_HASH]++;
    real_id = ring_lookup(vip_id, hash);
    if (real_id == PB_REAL_NONE || !may_insert)
        return real_id;

    struct pb_ct_value val = {
        .real_id = real_id,
        .vip_id = vip_id,
        .last_seen_ns = bpf_ktime_get_ns(),
    };
    // Failure to insert is not fatal: the packet still goes to the hashed
    // real and the next packet re-hashes to the same one.
    bpf_map_update_elem(&conntrack, &key, &val, BPF_ANY);
    return real_id;
}

// ------------------------------------------------------------------------
// Encapsulation.
// ------------------------------------------------------------------------

// Ones' complement checksum of a 20-byte IPv4 header, summed as 16-bit words
// in network order. The ones' complement sum is byte-order independent, so
// summing the words as they sit in memory and storing the result as-is is
// correct on both little and big endian.
static __always_inline __u16 ipv4_csum(const struct iphdr *ip)
{
    const __u16 *w = (const __u16 *)ip;
    __u32 sum = 0;

#pragma unroll
    for (int i = 0; i < (int)(sizeof(*ip) / 2); i++)
        sum += w[i];
    // 10 words of at most 0xffff sum to < 0xa0000: two folds always suffice.
    sum = (sum & 0xffff) + (sum >> 16);
    sum = (sum & 0xffff) + (sum >> 16);
    return (__u16)~sum;
}

// Prepend an outer IPv4 header and rewrite Ethernet. Returns 0 or -1 if the
// head could not be grown. Everything needed from the original packet is
// passed in by value, because bpf_xdp_adjust_head invalidates every packet
// pointer taken before it.
static __always_inline int encap_ipip(struct xdp_md *ctx, __u32 real_addr,
                                      const struct pb_mac *dmac,
                                      const struct pb_config *cfg,
                                      __u32 hash, __u32 inner_len, __u8 tos)
{
    if (bpf_xdp_adjust_head(ctx, -PB_OUTER_LEN))
        return -1;

    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    struct ethhdr *eth = data;
    struct iphdr *outer = (void *)(eth + 1);
    if ((void *)(outer + 1) > data_end)
        return -1;

    // The old Ethernet header now sits at data + 20, over the start of what
    // becomes the outer IP header. Nothing of it is needed: both MACs are
    // rewritten and the ethertype stays IPv4. So write a fresh one at the
    // new front instead of moving the old one.
    __builtin_memcpy(eth->h_dest, dmac->mac, ETH_ALEN);
    __builtin_memcpy(eth->h_source, cfg->lb_mac, ETH_ALEN);
    eth->h_proto = bpf_htons(ETH_P_IP);

    // Outer source: the configured prefix with the host bits taken from the
    // flow hash. The real's NIC hashes the outer header for RSS (it cannot
    // see the inner 5-tuple of an IPIP packet), so a constant source would
    // put every flow from this LB on one RX queue of the real. Mask and
    // prefix are in network order; hash bits are random, so AND-ing the raw
    // hash with the network-order host mask just picks random host bits,
    // no swap needed. Same flow -> same hash -> same outer source, which
    // keeps a flow on one queue (no reordering).
    __u32 saddr = (cfg->encap_src_prefix & cfg->encap_src_mask) |
                  (hash & ~cfg->encap_src_mask);

    // version 4, ihl 5.
    *(__u8 *)outer = 0x45;
    outer->tos = tos;
    outer->tot_len = bpf_htons((__u16)(inner_len + PB_OUTER_LEN));
    outer->id = 0;
    // DF = 0. The LB is not the sender and cannot act on an ICMP
    // frag-needed addressed to the outer source (a synthetic address). Leaving
    // DF clear lets a router between LB and real fragment the outer packet
    // rather than black-hole it; the real reassembles before decapsulating.
    // Client-side PMTU discovery is handled by ICMP forwarding instead (see
    // handle_icmp).
    outer->frag_off = 0;
    outer->ttl = PB_OUTER_TTL;
    outer->protocol = IPPROTO_IPIP;
    outer->check = 0;
    outer->saddr = saddr;
    outer->daddr = real_addr;
    outer->check = ipv4_csum(outer);
    return 0;
}

// Resolve the real, encapsulate, count, XDP_TX. Common tail of the TCP/UDP
// path and the ICMP PMTU path.
static __always_inline int forward(struct xdp_md *ctx, const struct pb_flow *f,
                                   const struct pb_vip_value *vip,
                                   const struct pb_config *cfg,
                                   int bare_syn, int may_insert, int is_icmp,
                                   __u32 inner_len, __u8 tos)
{
    struct pb_stats *st = stats_slot(vip->vip_id);
    int use_ct = !(cfg->flags & PB_CFG_F_NO_CONNTRACK) &&
                 !(vip->flags & PB_VIP_F_NO_CONNTRACK);
    __u32 hash = pb_flow_hash(f->saddr, f->daddr, f->sport, f->dport, f->proto);

    if (st) {
        st->c[PB_CNT_PACKETS]++;
        st->c[PB_CNT_BYTES] += inner_len;
        if (bare_syn)
            st->c[PB_CNT_SYN]++;
    }

    // Length check before touching the connection table, so a packet that
    // is going to be dropped does not create a flow entry.
    //
    // After IPIP the packet is 20 bytes longer and must still fit the path
    // MTU toward the real. PacketBalance does not fragment (neither does
    // Katran): it relies on the client's PMTU discovery and on the MSS the
    // real advertises. Note what that means in the lab, where every veth has
    // MTU 1500: a full-size 1500-byte client packet becomes 1520 bytes and
    // the real's veth drops it even though it passes this check. The lab
    // must either raise the MTU of the LB-real path (Katran's production
    // requirement) or lower the reals' advertised MSS by 20. This check only
    // bounds what the program itself will build, PB_MAX_PACKET_LEN, the same
    // 3.5 KB cap Katran documents.
    if (inner_len + PB_OUTER_LEN > PB_MAX_PACKET_LEN)
        return drop_vip(st, PB_CNT_DROP_MTU);

    __u32 real_id = pick_real(f, vip->vip_id, hash, use_ct, bare_syn,
                              may_insert, st);
    // Bounds check the verifier needs before real_id indexes the arrays, and
    // the "no healthy real" case (ring slot PB_REAL_NONE) in one compare.
    if (real_id >= PB_MAX_REALS)
        return drop_vip(st, PB_CNT_DROP_NO_REAL);

    struct pb_real *real = bpf_map_lookup_elem(&reals, &real_id);
    if (!real || real->addr == 0)
        return drop_vip(st, PB_CNT_DROP_NO_REAL);
    struct pb_mac *dmac = bpf_map_lookup_elem(&neigh, &real_id);
    if (!dmac)
        return drop_vip(st, PB_CNT_DROP_NO_REAL);

    if (encap_ipip(ctx, real->addr, dmac, cfg, hash, inner_len, tos))
        return drop_vip(st, PB_CNT_DROP_ADJ_HEAD);

    if (st) {
        st->c[PB_CNT_TX]++;
        if (is_icmp)
            st->c[PB_CNT_ICMP_PMTU_FWD]++;
    }
    struct pb_real_stats *rs = bpf_map_lookup_elem(&real_stats, &real_id);
    if (rs) {
        rs->packets++;
        rs->bytes += inner_len;
    }
    return XDP_TX;
}

// ------------------------------------------------------------------------
// ICMP. Everything is XDP_PASS (it is for the LB host) except "destination
// unreachable, fragmentation needed" about a VIP flow, when enabled.
//
// Why it has to be forwarded: with DSR the real sends its replies straight
// to the client with source = VIP. When a router on that return path cannot
// forward a DF packet it sends ICMP frag-needed to the packet's source, the
// VIP, which is routed to the load balancer, not to the real that sent the
// too-big packet. Unless the LB forwards the ICMP to that real, the real
// never lowers its path MTU and the connection stalls (a PMTU black hole).
//
// The quoted datagram inside the ICMP is the real's reply, VIP -> client.
// So the VIP is the inner SOURCE, and the flow the LB tracks (client -> VIP)
// is the inner header with source and destination swapped.
// ------------------------------------------------------------------------
static __always_inline int handle_icmp(struct xdp_md *ctx, struct iphdr *iph,
                                       void *data_end,
                                       const struct pb_config *cfg)
{
    struct icmphdr *icmp = (void *)(iph + 1);
    if ((void *)(icmp + 1) > data_end)
        return drop_global(PB_CNT_DROP_SHORT);

    if (!(cfg->flags & PB_CFG_F_ICMP_PMTU) ||
        icmp->type != ICMP_DEST_UNREACH || icmp->code != ICMP_FRAG_NEEDED)
        return pass_global();

    struct iphdr *inner = (void *)(icmp + 1);
    if ((void *)(inner + 1) > data_end)
        return drop_global(PB_CNT_DROP_SHORT);
    if (inner->ihl != 5 ||
        (inner->protocol != IPPROTO_TCP && inner->protocol != IPPROTO_UDP))
        return pass_global();

    // RFC 792 guarantees the first 8 bytes of the inner L4 header; both
    // ports are in the first 4 for TCP and UDP.
    __u16 *ports = (void *)(inner + 1);
    if ((void *)(ports + 2) > data_end)
        return drop_global(PB_CNT_DROP_SHORT);

    struct pb_vip_key vk = {
        .addr = inner->saddr,
        .port = ports[0],
        .proto = inner->protocol,
    };
    struct pb_vip_value *vip = bpf_map_lookup_elem(&vip_map, &vk);
    if (!vip)
        return pass_global();

    struct pb_flow f = {
        .saddr = inner->daddr,     // client
        .daddr = inner->saddr,     // VIP
        .sport = ports[1],         // client port
        .dport = ports[0],         // VIP port
        .proto = inner->protocol,
    };
    __u32 len = bpf_ntohs(iph->tot_len);
    return forward(ctx, &f, vip, cfg, 0, 0, 1, len, iph->tos);
}

SEC("xdp")
int xdp_packetbalance(struct xdp_md *ctx)
{
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return drop_global(PB_CNT_DROP_SHORT);
    // ARP, IPv6, VLAN-tagged, LLDP ...: not ours, hand to the stack.
    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return pass_global();

    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return drop_global(PB_CNT_DROP_SHORT);
    if (iph->version != 4)
        return drop_global(PB_CNT_DROP_OTHER);
    // Fixed 20-byte header only. Options are rare, slow-pathed by routers,
    // and would make every offset below variable. Katran drops them too.
    if (iph->ihl != 5)
        return drop_global(PB_CNT_DROP_OPTS);
    // Fragments: non-first fragments carry no ports, so they cannot be
    // mapped to a flow. Drop every fragment, first included (MF set or a
    // non-zero offset), rather than send the pieces of one datagram to
    // different reals.
    if (iph->frag_off & bpf_htons(PB_IP_MF | PB_IP_OFFSET))
        return drop_global(PB_CNT_DROP_FRAG);

    __u32 tot_len = bpf_ntohs(iph->tot_len);
    // A header that claims more bytes than the frame carries is truncated.
    // Frames may be longer than tot_len (Ethernet minimum-size padding).
    if (tot_len < sizeof(*iph) || (void *)iph + tot_len > data_end)
        return drop_global(PB_CNT_DROP_SHORT);

    __u32 zero = 0;
    struct pb_config *cfg = bpf_map_lookup_elem(&config, &zero);
    if (!cfg)
        return drop_global(PB_CNT_DROP_OTHER);

    struct pb_flow f = {
        .saddr = iph->saddr,
        .daddr = iph->daddr,
        .proto = iph->protocol,
    };
    int bare_syn = 0;

    if (iph->protocol == IPPROTO_TCP) {
        struct tcphdr *th = (void *)(iph + 1);
        if ((void *)(th + 1) > data_end)
            return drop_global(PB_CNT_DROP_SHORT);
        f.sport = th->source;
        f.dport = th->dest;
        bare_syn = th->syn && !th->ack;
    } else if (iph->protocol == IPPROTO_UDP) {
        struct udphdr *uh = (void *)(iph + 1);
        if ((void *)(uh + 1) > data_end)
            return drop_global(PB_CNT_DROP_SHORT);
        f.sport = uh->source;
        f.dport = uh->dest;
    } else if (iph->protocol == IPPROTO_ICMP) {
        return handle_icmp(ctx, iph, data_end, cfg);
    } else {
        return pass_global();
    }

    struct pb_vip_key vk = {
        .addr = f.daddr,
        .port = f.dport,
        .proto = f.proto,
    };
    struct pb_vip_value *vip = bpf_map_lookup_elem(&vip_map, &vk);
    if (!vip)
        return pass_global();

    return forward(ctx, &f, vip, cfg, bare_syn, 1, 0, tot_len, iph->tos);
}
