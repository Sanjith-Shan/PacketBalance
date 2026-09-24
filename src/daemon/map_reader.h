// SPDX-License-Identifier: MIT
//
// Read-only views of the maps the data plane writes: per-CPU counters and the
// per-CPU connection table.
//
// Why the sums: `stats`, `real_stats` and `conntrack` are per-CPU maps. Each
// CPU increments its own copy with no atomics and no shared cache line, which
// is what lets the data plane scale with RX queues. The price is paid here, on
// the slow path: a lookup returns one value per possible CPU and we add them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "counters.h"
#include "loader.h"
#include "packetbalance/abi.h"

namespace pb {

struct FlowRow {
    pb_ct_key key;
    pb_ct_value value;
    uint32_t cpu;
};

class MapReader {
public:
    explicit MapReader(const MapFds& fds);

    // stats[index] summed over CPUs. index is a vip_id or PB_STATS_GLOBAL.
    Counters counters(uint32_t index) const;
    pb_real_stats real_counters(uint32_t real_id) const;

    // Zeroes every CPU's copy, for a newly allocated vip_id or real_id.
    void reset_counters(uint32_t index) const;
    void reset_real_counters(uint32_t real_id) const;

    // One row per (key, CPU) whose per-CPU value is in use. The LRU is
    // per-CPU, so the same 5-tuple can appear on several CPUs; each copy is a
    // separate entry and is reported separately.
    using FlowFilter = std::function<bool(const pb_ct_key&, const pb_ct_value&)>;
    std::vector<FlowRow> flows(const FlowFilter& keep, size_t limit) const;

    // Number of keys in the connection table. O(entries): a get_next_key walk.
    size_t conntrack_entries() const;

    int num_cpus() const { return ncpus_; }
    int rings_fd() const { return fds_.rings; }

private:
    MapFds fds_;
    int ncpus_;
    uint32_t ct_max_entries_;
};

}  // namespace pb
