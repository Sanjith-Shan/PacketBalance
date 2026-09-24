// SPDX-License-Identifier: MIT
//
// Consistent hashing for PacketBalance. Builds the lookup ring the data plane
// indexes with flow_hash % ring_size.
//
// Two algorithms:
//   Maglev  - the permutation-and-fill algorithm from Eisenbud et al., NSDI 2016,
//             section 3.4, with Katran's weighted variant (a backend with weight w
//             takes w turns per round of the fill). Removing one backend of N moves
//             about 1/N of the slots.
//   Modulo  - slot i -> backends[i % N]. The naive baseline. Removing one backend
//             renumbers nearly every slot. Exists so Experiments 3, 4 and 5 can show
//             what consistent hashing buys.
//
// Both are deterministic in the SET of backends: the ring depends only on
// (addr, weight) pairs, never on real_id or insertion order, because two load
// balancers must build identical rings from the same config (Experiment 4).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "packetbalance/abi.h"

namespace pb {

struct Backend {
    uint32_t real_id;  // index into the data plane's reals array, written into slots
    uint32_t addr;     // IPv4, network byte order. Identity for hashing and ordering.
    uint32_t weight;   // 0 means draining: takes no slot
};

enum class HashMode { Maglev, Modulo };

HashMode parse_hash_mode(const std::string& s);   // "maglev" | "modulo", throws
const char* hash_mode_name(HashMode m);

// Returns a ring of `ring_size` slots, each a real_id or PB_REAL_NONE.
// Every backend with weight > 0 gets a share of slots within 1% of
// weight / sum(weights) (Maglev), or within one slot per backend (Modulo).
// If no backend has weight > 0 every slot is PB_REAL_NONE.
// Throws std::invalid_argument if two weighted backends share an address.
std::vector<uint32_t> build_ring(const std::vector<Backend>& backends,
                                 HashMode mode,
                                 uint32_t ring_size = PB_RING_SIZE);

// Slot chosen for a flow. Same arithmetic as the XDP program.
inline uint32_t ring_index(uint32_t flow_hash, uint32_t ring_size = PB_RING_SIZE) {
    return flow_hash % ring_size;
}

// Fraction of slots whose real_id differs between two rings of equal size.
double ring_disruption(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b);

}  // namespace pb
