/* SPDX-License-Identifier: MIT
 *
 * PacketBalance data-plane / control-plane ABI.
 *
 * This header is included by BOTH the XDP program (C, compiled by clang to BPF)
 * and the C++20 control plane. It is the single source of truth for every map
 * key, map value, constant and flag the two sides share. Change it in one place.
 *
 * Rules:
 *   - C only. No C++ features, no function bodies other than static inline.
 *   - Fixed-width kernel types (__u8 ... __u64) from <linux/types.h>.
 *   - Every struct is explicitly padded to a multiple of 8 bytes so the BPF
 *     side and the C++ side agree on sizeof() regardless of ABI.
 *   - IPv4 addresses and ports are stored in NETWORK byte order, exactly as
 *     they appear on the wire, so the data plane never byte-swaps on lookup.
 */
#ifndef PACKETBALANCE_ABI_H
#define PACKETBALANCE_ABI_H

#if defined(__linux__) || defined(__BPF__) || defined(__bpf__)
#include <linux/types.h>
#else
/* Portable fallback so the control-plane core and its tests build on macOS. */
#include <stdint.h>
typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

/* ------------------------------------------------------------------------ */
/* Sizes                                                                     */
/* ------------------------------------------------------------------------ */

/* Maglev lookup table size. Prime, as in the Maglev paper (section 3.4). */
#define PB_RING_SIZE          65537u

/* Maximum number of VIPs (outer map size for rings, stats array size). */
#define PB_MAX_VIPS           64u

/* Maximum number of reals across all VIPs (reals/neigh/real_stats arrays). */
#define PB_MAX_REALS          512u

/* Default connection-table size. The daemon may override before load. */
#define PB_CT_DEFAULT_SIZE    (1u << 20)

/* Sentinel meaning "no real" in a ring slot or a lookup result. */
#define PB_REAL_NONE          0xFFFFFFFFu

/* Index into `stats` used for packets that are not for any VIP (host traffic,
 * non-IPv4, etc). Always PB_MAX_VIPS, i.e. one past the last VIP id. */
#define PB_STATS_GLOBAL       PB_MAX_VIPS

/* Largest packet the data plane will encapsulate. Anything longer is dropped
 * and counted as PB_CNT_DROP_MTU. Katran's limit is 3.5 KB for the same
 * reason: after adding 20 bytes of outer IPv4 the frame must still fit in the
 * path MTU toward the reals. The lab uses 1500-byte veths. */
#define PB_MAX_PACKET_LEN     3500u

/* ------------------------------------------------------------------------ */
/* Map names. Pinned under PB_PIN_DIR/<name>. The daemon and pbctl open the  */
/* pinned files, so a daemon restart sees the same maps and drops no flow.   */
/* ------------------------------------------------------------------------ */

#define PB_PIN_DIR            "/sys/fs/bpf/packetbalance"
#define PB_MAP_VIP            "vip_map"
#define PB_MAP_RINGS          "rings"
#define PB_MAP_REALS          "reals"
#define PB_MAP_NEIGH          "neigh"
#define PB_MAP_CONNTRACK      "conntrack"
#define PB_MAP_STATS          "stats"
#define PB_MAP_REAL_STATS     "real_stats"
#define PB_MAP_CONFIG         "config"
#define PB_PROG_NAME          "xdp_packetbalance"

/* ------------------------------------------------------------------------ */
/* vip_map: HASH, (daddr, dport, proto) -> vip_id + flags                     */
/* ------------------------------------------------------------------------ */

struct pb_vip_key {
    __u32 addr;    /* network order */
    __u16 port;    /* network order */
    __u8  proto;   /* IPPROTO_TCP or IPPROTO_UDP */
    __u8  pad;
};

/* Per-VIP flag: skip the connection table for this VIP (hash every packet).
 * Set by `pbctl vip add ... --no-conntrack` or by the daemon-wide flag. */
#define PB_VIP_F_NO_CONNTRACK (1u << 0)

struct pb_vip_value {
    __u32 vip_id;  /* 0 .. PB_MAX_VIPS-1, index into rings and stats */
    __u32 flags;   /* PB_VIP_F_* */
};

/* ------------------------------------------------------------------------ */
/* rings: ARRAY_OF_MAPS, vip_id -> inner ARRAY[PB_RING_SIZE] of __u32 real_id */
/* The control plane builds a complete inner map and swaps the outer slot in  */
/* one syscall, so the data plane never sees a half-filled ring.             */
/* ------------------------------------------------------------------------ */

/* Inner map: key __u32 slot (0..PB_RING_SIZE-1), value __u32 real_id or
 * PB_REAL_NONE. No struct needed. */

/* ------------------------------------------------------------------------ */
/* reals: ARRAY, real_id -> IPv4 + flags                                      */
/* ------------------------------------------------------------------------ */

struct pb_real {
    __u32 addr;    /* network order. 0 means the slot is unused. */
    __u32 flags;   /* reserved, 0 */
};

/* ------------------------------------------------------------------------ */
/* neigh: ARRAY, real_id -> destination MAC for the encapsulated frame.       */
/* In the L2 lab this is the real's own MAC. In an L3 deployment the control  */
/* plane writes the next-hop router's MAC into every slot instead.            */
/* ------------------------------------------------------------------------ */

struct pb_mac {
    __u8 mac[6];
    __u8 pad[2];
};

/* ------------------------------------------------------------------------ */
/* conntrack: LRU_PERCPU_HASH, 5-tuple -> real_id + last seen                 */
/* ------------------------------------------------------------------------ */

struct pb_ct_key {
    __u32 saddr;   /* network order */
    __u32 daddr;   /* network order (the VIP) */
    __u16 sport;   /* network order */
    __u16 dport;   /* network order */
    __u8  proto;
    __u8  pad[3];
};

struct pb_ct_value {
    __u32 real_id;
    __u32 vip_id;
    __u64 last_seen_ns;  /* bpf_ktime_get_ns() at last packet */
};

/* ------------------------------------------------------------------------ */
/* stats: PERCPU_ARRAY[PB_MAX_VIPS + 1] of counters, index = vip_id or        */
/* PB_STATS_GLOBAL. The control plane sums across CPUs when reading.          */
/* ------------------------------------------------------------------------ */

enum pb_counter {
    PB_CNT_PACKETS = 0,     /* packets that matched this VIP */
    PB_CNT_BYTES,           /* bytes of those packets, before encapsulation */
    PB_CNT_CT_HIT,          /* real found in the connection table */
    PB_CNT_CT_MISS,         /* looked up the table and missed, fell to hash */
    PB_CNT_HASH,            /* real chosen by hashing (SYN, miss, or no-ct) */
    PB_CNT_SYN,             /* TCP SYN packets seen (new connections) */
    PB_CNT_TX,              /* packets encapsulated and XDP_TX'd */
    PB_CNT_PASS,            /* XDP_PASS (only meaningful at PB_STATS_GLOBAL) */
    PB_CNT_DROP_FRAG,       /* IPv4 fragment */
    PB_CNT_DROP_OPTS,       /* IPv4 header with options (ihl != 5) */
    PB_CNT_DROP_NO_REAL,    /* ring slot was PB_REAL_NONE (no healthy real) */
    PB_CNT_DROP_ADJ_HEAD,   /* bpf_xdp_adjust_head failed */
    PB_CNT_DROP_MTU,        /* packet longer than PB_MAX_PACKET_LEN */
    PB_CNT_DROP_SHORT,      /* truncated header */
    PB_CNT_DROP_OTHER,      /* anything else */
    PB_CNT_ICMP_PMTU_FWD,   /* ICMP frag-needed forwarded to the right real */
    PB_CNT_MAX
};

struct pb_stats {
    __u64 c[PB_CNT_MAX];
};

/* real_stats: PERCPU_ARRAY[PB_MAX_REALS], index = real_id */
struct pb_real_stats {
    __u64 packets;
    __u64 bytes;
};

/* ------------------------------------------------------------------------ */
/* config: ARRAY[1], written by the daemon at load and on reload.            */
/* ------------------------------------------------------------------------ */

/* Daemon-wide: never consult the connection table (the --no-conntrack flag). */
#define PB_CFG_F_NO_CONNTRACK (1u << 0)
/* Forward ICMP "fragmentation needed" to the real that owns the inner flow. */
#define PB_CFG_F_ICMP_PMTU    (1u << 1)

struct pb_config {
    __u8  lb_mac[6];        /* source MAC for the encapsulated frame */
    __u8  pad0[2];
    __u32 encap_src_prefix; /* network order, e.g. 10.99.0.0 */
    __u32 encap_src_mask;   /* network order, e.g. 255.255.255.0. The outer
                             * source is prefix | (flow_hash & ~mask), so the
                             * real's NIC RSS spreads flows across queues. */
    __u32 flags;            /* PB_CFG_F_* */
    __u32 pad1;
};

/* ------------------------------------------------------------------------ */
/* Return codes of the XDP program, mirrored for the BPF_PROG_TEST_RUN tests */
/* ------------------------------------------------------------------------ */
/* XDP_ABORTED 0, XDP_DROP 1, XDP_PASS 2, XDP_TX 3 (from <linux/bpf.h>) */

#endif /* PACKETBALANCE_ABI_H */
