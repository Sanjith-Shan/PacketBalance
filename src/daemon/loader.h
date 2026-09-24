// SPDX-License-Identifier: MIT
//
// Loads the XDP object through its libbpf skeleton, reuses or creates the
// pinned maps, and attaches the program to the interface.
//
// Lifetime is the whole point of this class. The XDP attachment is made with
// bpf_xdp_attach(), a netlink attachment owned by the interface, not a
// bpf_link owned by this process, and every map is pinned in bpffs. So when the
// daemon exits (or crashes) the program keeps forwarding and the connection
// table keeps its flows; the next daemon reuses the pinned maps and atomically
// replaces the program. Only detach_and_unpin() (--detach-on-exit) tears down.
#pragma once

#include <cstdint>
#include <string>

#include "packetbalance/config.h"

struct packetbalance_bpf;  // generated skeleton

namespace pb {

// Raw fds of the loaded maps. Owned by the skeleton, valid while Loader lives.
struct MapFds {
    int vip_map = -1;
    int rings = -1;
    int reals = -1;
    int neigh = -1;
    int conntrack = -1;
    int stats = -1;
    int real_stats = -1;
    int config = -1;
};

class Loader {
public:
    struct Options {
        std::string interface;
        XdpMode mode = XdpMode::Auto;
        uint32_t conntrack_size = PB_CT_DEFAULT_SIZE;
        std::string pin_path = PB_PIN_DIR;
        bool recreate_maps = false;  // unpin maps whose definition changed
    };

    // Opens, loads and attaches. Throws std::runtime_error with the reason.
    explicit Loader(const Options& opts);
    // Frees the skeleton only: the program stays attached, the maps pinned.
    ~Loader();

    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    const MapFds& maps() const { return maps_; }
    int ifindex() const { return ifindex_; }
    // Native or Generic: the mode actually in effect, never Auto.
    XdpMode attached_mode() const { return attached_mode_; }
    // True when this start found pinned maps and reused them (a restart).
    bool reused_pins() const { return reused_pins_; }
    // True when the connection table itself was inherited, i.e. flows survive.
    bool reused_conntrack() const { return reused_conntrack_; }

    // Detaches the program from the interface and removes the pins. The maps
    // die with the last reference, taking every tracked flow with them.
    void detach_and_unpin();

private:
    void check_pinned_maps();
    void attach();
    bool try_attach(XdpMode mode);

    Options opts_;
    packetbalance_bpf* skel_ = nullptr;
    int ifindex_ = 0;
    XdpMode attached_mode_ = XdpMode::Auto;
    bool reused_pins_ = false;
    bool reused_conntrack_ = false;
    MapFds maps_;
};

}  // namespace pb
