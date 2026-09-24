// SPDX-License-Identifier: MIT
#include "neighbor_resolver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <fstream>
#include <sstream>
#include <thread>

#include "log.h"
#include "unique_fd.h"

namespace pb {

using namespace std::chrono_literals;

NeighborResolver::NeighborResolver(LbState& state, std::string interface)
    : state_(state), interface_(std::move(interface)) {}

void NeighborResolver::start() {
    thread_.start([this] { return step(); });
}

void NeighborResolver::stop() { thread_.stop(); }

// UDP to the discard port. The payload is irrelevant; the point is that the
// kernel must ARP for the address before it can transmit.
void NeighborResolver::probe(uint32_t ip) const {
    UniqueFd fd(::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!fd) return;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);
    sa.sin_addr.s_addr = ip;
    const char byte = 0;
    ::sendto(fd.get(), &byte, 1, 0, reinterpret_cast<const sockaddr*>(&sa), sizeof sa);
}

// /proc/net/arp:
//   IP address       HW type     Flags       HW address            Mask     Device
//   10.0.0.21        0x1         0x2         aa:bb:cc:dd:ee:ff     *        veth0
// Flags 0x2 (ATF_COM) means the entry is complete. Only entries on our own
// interface count: XDP_TX can only send out of the interface it runs on.
std::map<uint32_t, Mac> read_arp_table(const std::string& interface) {
    std::map<uint32_t, Mac> out;
    std::ifstream in("/proc/net/arp");
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string ip, hwtype, flags, hw, mask, dev;
        if (!(ss >> ip >> hwtype >> flags >> hw >> mask >> dev)) continue;
        if (dev != interface || !(std::stoul(flags, nullptr, 16) & 0x2)) continue;
        in_addr a{};
        auto mac = Mac::parse(hw);
        if (inet_pton(AF_INET, ip.c_str(), &a) == 1 && mac && !mac->is_zero()) out[a.s_addr] = *mac;
    }
    return out;
}

std::chrono::milliseconds NeighborResolver::step() {
    const auto now = std::chrono::steady_clock::now();
    const bool full = now - last_full_refresh_ >= 30s;
    const std::vector<uint32_t> targets = full ? state_.neighbor_targets() : state_.unresolved_neighbors();
    if (targets.empty()) return 1000ms;
    if (full) last_full_refresh_ = now;

    for (uint32_t ip : targets) probe(ip);
    std::this_thread::sleep_for(100ms);  // one LAN round trip, generously
    const auto table = read_arp_table(interface_);

    for (uint32_t ip : targets) {
        if (auto it = table.find(ip); it != table.end()) {
            failures_.erase(ip);
            state_.neighbor_resolved(ip, it->second);
            continue;
        }
        const unsigned n = ++failures_[ip];
        if (n == 3 || n % 30 == 0)
            log::warn("neigh: no ARP entry for {} on {} after {} tries (a real never resolved "
                      "gets no ring slots; a known MAC is kept)",
                      ipv4_to_string(ip), interface_, n);
    }
    return 1000ms;
}

}  // namespace pb
