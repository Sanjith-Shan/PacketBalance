// SPDX-License-Identifier: MIT
//
// The per-VIP Maglev rings live in `rings`, a BPF_MAP_TYPE_ARRAY_OF_MAPS whose
// slot vip_id holds an inner ARRAY[PB_RING_SIZE] of real_id.
//
// Why a new inner map per change instead of rewriting the live one: a ring is
// 65,537 entries. Rewriting them in place takes thousands of syscalls (or a few
// batched ones) during which the data plane hashes packets against a ring that
// is part old and part new, so a flow could be sent to a real that is in
// neither version. Building a complete map off to the side and then storing
// its fd into the outer array is ONE bpf_map_update_elem on the outer map: the
// kernel swaps an RCU-protected pointer, and each packet sees either the whole
// old ring or the whole new one.
#pragma once

#include <cstdint>
#include <vector>

namespace pb {

// Creates, fills and installs a new ring at rings[vip_id]. The old inner map
// is freed by the kernel once no program run still references it.
void install_ring(int rings_fd, uint32_t vip_id, const std::vector<uint32_t>& ring);

// Empties rings[vip_id]. The data plane then finds no ring for that VIP.
void remove_ring(int rings_fd, uint32_t vip_id);

// Reads the ring currently installed at rings[vip_id]; empty if none.
std::vector<uint32_t> read_ring(int rings_fd, uint32_t vip_id);

}  // namespace pb
