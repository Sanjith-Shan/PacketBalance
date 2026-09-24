// SPDX-License-Identifier: MIT
#include "ring_map.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <cerrno>
#include <cstring>
#include <numeric>
#include <stdexcept>
#include <string>

#include "packetbalance/abi.h"
#include "unique_fd.h"

namespace pb {

namespace {

std::string errstr(int err) { return std::strerror(err < 0 ? -err : err); }

const std::vector<uint32_t>& slot_keys() {
    static const std::vector<uint32_t> keys = [] {
        std::vector<uint32_t> k(PB_RING_SIZE);
        std::iota(k.begin(), k.end(), 0u);
        return k;
    }();
    return keys;
}

}  // namespace

void install_ring(int rings_fd, uint32_t vip_id, const std::vector<uint32_t>& ring) {
    if (ring.size() != PB_RING_SIZE) throw std::logic_error("ring has the wrong size");

    // Must match the `struct ring_map` template in packetbalance.bpf.c exactly
    // (type, key size, value size, max_entries, flags), or the kernel rejects
    // the outer-map update with EINVAL. No BTF is needed for an inner map.
    UniqueFd inner(bpf_map_create(BPF_MAP_TYPE_ARRAY, "pb_ring", sizeof(uint32_t),
                                  sizeof(uint32_t), PB_RING_SIZE, nullptr));
    if (!inner) throw std::runtime_error("create ring map: " + errstr(errno));

    // One batched syscall for all 65,537 slots instead of 65,537 updates.
    __u32 count = PB_RING_SIZE;
    if (int err = bpf_map_update_batch(inner.get(), slot_keys().data(), ring.data(), &count, nullptr))
        throw std::runtime_error("fill ring map: " + errstr(err) + " after " +
                                 std::to_string(count) + " slots");

    // The swap. From user space the value written into a map-in-map is the
    // inner map's fd; the kernel stores a reference to the map itself.
    const int inner_fd = inner.get();
    if (int err = bpf_map_update_elem(rings_fd, &vip_id, &inner_fd, BPF_ANY))
        throw std::runtime_error("swap ring for vip_id " + std::to_string(vip_id) + ": " + errstr(err));
    // `inner` closes our fd here; the outer map holds its own reference.
}

void remove_ring(int rings_fd, uint32_t vip_id) {
    int err = bpf_map_delete_elem(rings_fd, &vip_id);
    if (err && err != -ENOENT)
        throw std::runtime_error("remove ring for vip_id " + std::to_string(vip_id) + ": " + errstr(err));
}

std::vector<uint32_t> read_ring(int rings_fd, uint32_t vip_id) {
    // Looking up a map-in-map from user space yields the inner map's ID, not
    // an fd; turn it into an fd to read the ring.
    __u32 inner_id = 0;
    if (bpf_map_lookup_elem(rings_fd, &vip_id, &inner_id) != 0 || inner_id == 0) return {};
    UniqueFd inner(bpf_map_get_fd_by_id(inner_id));
    if (!inner) throw std::runtime_error("open ring map id " + std::to_string(inner_id) + ": " + errstr(errno));

    std::vector<uint32_t> keys(PB_RING_SIZE), values(PB_RING_SIZE);
    std::vector<uint32_t> ring(PB_RING_SIZE, PB_REAL_NONE);
    __u32 in_batch = 0, out_batch = 0;
    size_t got = 0;
    bool first = true;
    while (got < PB_RING_SIZE) {
        __u32 count = static_cast<__u32>(PB_RING_SIZE - got);
        int err = bpf_map_lookup_batch(inner.get(), first ? nullptr : &in_batch, &out_batch,
                                       keys.data() + got, values.data() + got, &count, nullptr);
        got += count;
        if (err == -ENOENT) break;  // end of map
        if (err) throw std::runtime_error("read ring map: " + errstr(err));
        in_batch = out_batch;
        first = false;
    }
    for (size_t i = 0; i < got; ++i)
        if (keys[i] < PB_RING_SIZE) ring[keys[i]] = values[i];
    return ring;
}

}  // namespace pb
