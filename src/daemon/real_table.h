// SPDX-License-Identifier: MIT
//
// Owns the `reals` and `neigh` arrays: which real_id means which IPv4 address,
// and which MAC an encapsulated packet for that real is sent to.
//
// One real_id per ADDRESS, shared by every VIP that uses the real, with a
// reference count. The id is what the connection table stores, so it must be
// stable for as long as flows might reference it:
//   - a restarted daemon adopts the ids it finds in the pinned `reals` map
//     instead of renumbering (renumbering would send every tracked flow to
//     whichever real inherited its old id);
//   - a freed id is not reused immediately. Allocation walks a cursor around
//     the array, so a stale conntrack entry for a deleted real is very likely
//     evicted from the LRU before its id means someone else.
// real_id 0 is never allocated: an all-zero per-CPU conntrack value then
// unambiguously means "this CPU has no copy of the flow".
//
// Not thread-safe; LbState serialises access.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "packetbalance/abi.h"

namespace pb {

class MapReader;

struct Mac {
    std::array<uint8_t, 6> bytes{};
    static std::optional<Mac> parse(const std::string& s);  // "aa:bb:cc:dd:ee:ff"
    std::string str() const;
    bool is_zero() const;
    bool operator==(const Mac&) const = default;
};

class RealTable {
public:
    RealTable(int reals_fd, int neigh_fd, const MapReader& reader);

    // Reads ids and MACs left in the pinned maps by a previous daemon. Adopted
    // entries have no references until acquire(); drop_unreferenced() frees
    // the ones the new configuration no longer uses.
    void adopt();
    void drop_unreferenced();

    // Returns the address's id, allocating and writing `reals` on first use.
    // Throws std::runtime_error("too many reals ...") when the array is full.
    uint32_t acquire(uint32_t addr);
    // Drops one reference; the last one clears the `reals` and `neigh` slots.
    void release(uint32_t addr);

    std::optional<uint32_t> id_of(uint32_t addr) const;
    std::optional<uint32_t> addr_of(uint32_t real_id) const;
    std::vector<uint32_t> addrs() const;

    std::optional<Mac> mac_of(uint32_t addr) const;
    // Writes neigh[id]. Returns true if the MAC changed (including first set).
    bool set_mac(uint32_t addr, const Mac& mac);

private:
    struct Slot {
        uint32_t addr = 0;  // 0: free
        uint32_t refs = 0;
        std::optional<Mac> mac;
    };
    void write_slot(uint32_t id);

    int reals_fd_;
    int neigh_fd_;
    const MapReader& reader_;
    std::array<Slot, PB_MAX_REALS> slots_{};
    std::map<uint32_t, uint32_t> by_addr_;  // addr -> id
    uint32_t cursor_ = 1;                   // next id to try
};

}  // namespace pb
