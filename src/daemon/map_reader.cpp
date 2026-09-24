// SPDX-License-Identifier: MIT
#include "map_reader.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace pb {

namespace {

// The kernel lays per-CPU values out at an 8-byte stride.
constexpr size_t percpu_stride(size_t value_size) { return (value_size + 7) & ~size_t{7}; }

template <typename T>
std::vector<T> lookup_percpu(int fd, uint32_t key, int ncpus) {
    static_assert(sizeof(T) % 8 == 0, "per-CPU value must be 8-byte padded");
    std::vector<T> values(static_cast<size_t>(ncpus));
    if (bpf_map_lookup_elem(fd, &key, values.data()) != 0) return {};
    return values;
}

template <typename T>
void zero_percpu(int fd, uint32_t key, int ncpus) {
    std::vector<T> zeros(static_cast<size_t>(ncpus));
    bpf_map_update_elem(fd, &key, zeros.data(), BPF_ANY);
}

}  // namespace

MapReader::MapReader(const MapFds& fds) : fds_(fds) {
    ncpus_ = libbpf_num_possible_cpus();
    if (ncpus_ <= 0) throw std::runtime_error("libbpf_num_possible_cpus failed");
    bpf_map_info info{};
    __u32 len = sizeof info;
    ct_max_entries_ = bpf_map_get_info_by_fd(fds_.conntrack, &info, &len) == 0 ? info.max_entries : 0;
}

Counters MapReader::counters(uint32_t index) const {
    Counters sum{};
    for (const pb_stats& cpu : lookup_percpu<pb_stats>(fds_.stats, index, ncpus_))
        for (size_t i = 0; i < sum.size(); ++i) sum[i] += cpu.c[i];
    return sum;
}

pb_real_stats MapReader::real_counters(uint32_t real_id) const {
    pb_real_stats sum{};
    for (const pb_real_stats& cpu : lookup_percpu<pb_real_stats>(fds_.real_stats, real_id, ncpus_)) {
        sum.packets += cpu.packets;
        sum.bytes += cpu.bytes;
    }
    return sum;
}

void MapReader::reset_counters(uint32_t index) const { zero_percpu<pb_stats>(fds_.stats, index, ncpus_); }
void MapReader::reset_real_counters(uint32_t real_id) const {
    zero_percpu<pb_real_stats>(fds_.real_stats, real_id, ncpus_);
}

// The table is walked with bpf_map_get_next_key while the data plane keeps
// inserting and the LRU keeps evicting. If the key we hold is evicted
// underneath us the kernel restarts from the first key, so the walk can repeat
// entries; bounding it by max_entries keeps it finite. For an inspection tool
// that is the right trade: no locking against the fast path.
std::vector<FlowRow> MapReader::flows(const FlowFilter& keep, size_t limit) const {
    static_assert(percpu_stride(sizeof(pb_ct_value)) == sizeof(pb_ct_value));
    std::vector<FlowRow> rows;
    std::vector<pb_ct_value> values(static_cast<size_t>(ncpus_));
    pb_ct_key key{}, next{};
    bool first = true;
    for (uint64_t walked = 0; walked <= ct_max_entries_ && rows.size() < limit; ++walked) {
        if (bpf_map_get_next_key(fds_.conntrack, first ? nullptr : &key, &next) != 0) break;
        first = false;
        key = next;
        if (bpf_map_lookup_elem(fds_.conntrack, &key, values.data()) != 0) continue;  // evicted
        for (int cpu = 0; cpu < ncpus_ && rows.size() < limit; ++cpu) {
            const pb_ct_value& v = values[static_cast<size_t>(cpu)];
            // A per-CPU hash entry exists on every CPU; only the CPUs that saw
            // the flow have written a value. The others are zero-filled. Same
            // test as the data plane's: last_seen_ns == 0 means "no copy here".
            if (v.last_seen_ns == 0) continue;
            if (keep && !keep(key, v)) continue;
            rows.push_back(FlowRow{key, v, static_cast<uint32_t>(cpu)});
        }
    }
    return rows;
}

size_t MapReader::conntrack_entries() const {
    size_t n = 0;
    pb_ct_key key{}, next{};
    bool first = true;
    while (n <= ct_max_entries_ &&
           bpf_map_get_next_key(fds_.conntrack, first ? nullptr : &key, &next) == 0) {
        first = false;
        key = next;
        ++n;
    }
    return n;
}

}  // namespace pb
