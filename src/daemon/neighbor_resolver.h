// SPDX-License-Identifier: MIT
//
// Finds the destination MAC for encapsulated packets: each real's own MAC when
// the reals share our L2 segment (the lab), or the router's MAC for every real
// when `next_hop` is configured (an L3 deployment).
//
// XDP_TX bypasses the kernel's neighbour subsystem, so the data plane cannot
// ARP for itself; the control plane has to do it and write `neigh`. We let the
// kernel do the actual ARP: sending one UDP datagram to the address makes the
// kernel resolve it, and /proc/net/arp then has the answer. (The health
// checker's TCP connects have the same side effect.) Unresolved addresses are
// retried every second; resolved ones are re-read every 30 s so a replaced
// real's new MAC is picked up.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "lb_state.h"
#include "periodic_thread.h"

namespace pb {

// Reads the kernel's ARP table (/proc/net/arp) for complete entries on
// `interface`. No probing, so it is cheap enough to call under LbState's lock.
std::map<uint32_t, Mac> read_arp_table(const std::string& interface);

class NeighborResolver {
public:
    NeighborResolver(LbState& state, std::string interface);
    void start();
    void stop();
    void wake() { thread_.wake(); }  // a real was added: resolve it now

private:
    std::chrono::milliseconds step();
    void probe(uint32_t ip) const;

    LbState& state_;
    std::string interface_;
    std::map<uint32_t, unsigned> failures_;  // ip -> consecutive unresolved rounds
    std::chrono::steady_clock::time_point last_full_refresh_{};
    PeriodicThread thread_;
};

}  // namespace pb
