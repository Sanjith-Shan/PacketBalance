// SPDX-License-Identifier: MIT
//
// PacketBalance XDP data plane.
//
// STUB. Declares every map with its final name, type, key and value so the
// control plane can be built against the generated skeleton. The packet path
// is XDP_PASS until the real program lands.
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

SEC("xdp")
int xdp_packetbalance(struct xdp_md *ctx)
{
    (void)ctx;
    return XDP_PASS;
}
