// SPDX-License-Identifier: MIT
#include "loader.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <stdexcept>
#include <string>

#include "log.h"
#include "packetbalance.skel.h"
#include "unique_fd.h"

namespace pb {

namespace {

// libbpf's own diagnostics (including the verifier log on a failed load) go
// through our logger so they carry timestamps in the journal.
int libbpf_print(enum libbpf_print_level level, const char* fmt, va_list args) {
    char buf[4096];
    int n = std::vsnprintf(buf, sizeof buf, fmt, args);
    std::string_view msg(buf, n > 0 ? std::min<size_t>(n, sizeof buf - 1) : 0);
    while (!msg.empty() && msg.back() == '\n') msg.remove_suffix(1);
    switch (level) {
        // libbpf prefixes its own messages with "libbpf: ".
        case LIBBPF_WARN: log::warn("{}", msg); break;
        case LIBBPF_INFO: log::info("{}", msg); break;
        case LIBBPF_DEBUG: log::debug("{}", msg); break;
    }
    return n;
}

std::string errstr(int err) { return std::strerror(err < 0 ? -err : err); }

__u32 xdp_flags(XdpMode mode) {
    return mode == XdpMode::Native ? XDP_FLAGS_DRV_MODE : XDP_FLAGS_SKB_MODE;
}

// What is attached to the interface right now, before we touch it.
XdpMode current_attach_mode(int ifindex, bool* attached) {
    LIBBPF_OPTS(bpf_xdp_query_opts, q);
    *attached = false;
    if (bpf_xdp_query(ifindex, 0, &q) != 0) return XdpMode::Auto;
    if (q.attach_mode == XDP_ATTACHED_DRV) { *attached = true; return XdpMode::Native; }
    if (q.attach_mode == XDP_ATTACHED_SKB) { *attached = true; return XdpMode::Generic; }
    return XdpMode::Auto;
}

// Magic number of bpffs (include/uapi/linux/magic.h, BPF_FS_MAGIC).
constexpr unsigned long kBpfFsMagic = 0xcafe4a11;

// Pinned maps only work on a bpffs mount, and libbpf creates just one level of
// missing directory. Create the whole pin path, then check the filesystem.
// The check exists because of a real failure: `ip netns exec` gives the process
// a private mount namespace with a fresh sysfs at /sys, so the host's bpffs at
// /sys/fs/bpf is not there and mkdir fails with ENOENT. The fix on the operator's
// side is a bpffs mounted somewhere the process can see (mount -t bpf bpf DIR).
void ensure_pin_path(const std::string& pin_path) {
    std::string cur;
    for (size_t i = 1; i <= pin_path.size(); ++i) {
        if (i == pin_path.size() || pin_path[i] == '/') {
            cur = pin_path.substr(0, i);
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
                throw std::runtime_error("mkdir " + cur + ": " + std::strerror(errno) +
                                         " (is a bpffs mounted here? under `ip netns exec` /sys is a fresh"
                                         " sysfs, so use a bpffs mounted outside /sys, e.g."
                                         " mount -t bpf bpf /run/packetbalance/bpf)");
        }
    }
    struct statfs st {};
    if (statfs(pin_path.c_str(), &st) != 0)
        throw std::runtime_error("statfs " + pin_path + ": " + std::strerror(errno));
    if (static_cast<unsigned long>(st.f_type) != kBpfFsMagic)
        throw std::runtime_error("pin path " + pin_path + " is not on a bpffs mount; pinned maps"
                                 " need one (mount -t bpf bpf DIR). Note that `ip netns exec`"
                                 " replaces /sys, so /sys/fs/bpf is not visible there.");
}

}  // namespace

Loader::Loader(const Options& opts) : opts_(opts) {
    libbpf_set_print(libbpf_print);
    ensure_pin_path(opts_.pin_path);

    ifindex_ = static_cast<int>(if_nametoindex(opts_.interface.c_str()));
    if (ifindex_ == 0) throw std::runtime_error("no such interface: " + opts_.interface);

    // pin_root_path makes every LIBBPF_PIN_BY_NAME map live at
    // <pin_path>/<map name>. If that file exists, libbpf reuses the pinned map
    // instead of creating a new one: this is how a restarted daemon inherits
    // the previous one's connection table.
    LIBBPF_OPTS(bpf_object_open_opts, open_opts, .pin_root_path = opts_.pin_path.c_str());
    skel_ = packetbalance_bpf__open_opts(&open_opts);
    if (!skel_) throw std::runtime_error("open BPF skeleton: " + errstr(errno));

    // The connection table's size is fixed when the map is created, so it has
    // to be set between open and load.
    if (int err = bpf_map__set_max_entries(skel_->maps.conntrack, opts_.conntrack_size)) {
        packetbalance_bpf__destroy(skel_);
        throw std::runtime_error("set conntrack max_entries: " + errstr(err));
    }

    try {
        check_pinned_maps();
    } catch (...) {
        packetbalance_bpf__destroy(skel_);
        throw;
    }

    if (int err = packetbalance_bpf__load(skel_)) {
        packetbalance_bpf__destroy(skel_);
        throw std::runtime_error("load BPF object: " + errstr(err) +
                                 " (the verifier log, if any, is above)");
    }

    maps_ = MapFds{
        .vip_map = bpf_map__fd(skel_->maps.vip_map),
        .rings = bpf_map__fd(skel_->maps.rings),
        .reals = bpf_map__fd(skel_->maps.reals),
        .neigh = bpf_map__fd(skel_->maps.neigh),
        .conntrack = bpf_map__fd(skel_->maps.conntrack),
        .stats = bpf_map__fd(skel_->maps.stats),
        .real_stats = bpf_map__fd(skel_->maps.real_stats),
        .config = bpf_map__fd(skel_->maps.config),
    };

    try {
        attach();
    } catch (...) {
        packetbalance_bpf__destroy(skel_);
        throw;
    }
}

Loader::~Loader() {
    if (skel_) packetbalance_bpf__destroy(skel_);
}

// libbpf refuses to reuse a pinned map whose type, key/value size, max_entries
// or flags differ from the object's definition, and the error it gives
// ("couldn't reuse pinned map ... parameter mismatch", -EINVAL) does not say
// which parameter. The realistic case is conntrack.size changed in the config.
// Check first and either fail with a precise message or, with --recreate-maps,
// unpin the stale map so libbpf creates a fresh one. We do not do that silently:
// recreating the connection table drops every tracked flow.
void Loader::check_pinned_maps() {
    struct bpf_map* map;
    bpf_object__for_each_map(map, skel_->obj) {
        const std::string path = opts_.pin_path + "/" + bpf_map__name(map);
        struct stat st;
        if (::stat(path.c_str(), &st) != 0) continue;

        UniqueFd fd(bpf_obj_get(path.c_str()));
        if (!fd) throw std::runtime_error("open pinned map " + path + ": " + errstr(errno));
        bpf_map_info info{};
        __u32 len = sizeof info;
        if (int err = bpf_map_get_info_by_fd(fd.get(), &info, &len))
            throw std::runtime_error("inspect pinned map " + path + ": " + errstr(err));
        std::string diff;
        auto cmp = [&](const char* what, uint64_t pinned, uint64_t wanted) {
            if (pinned != wanted)
                diff += std::format("{}{} pinned={} wanted={}", diff.empty() ? "" : ", ", what,
                                    pinned, wanted);
        };
        cmp("type", info.type, bpf_map__type(map));
        cmp("key_size", info.key_size, bpf_map__key_size(map));
        cmp("value_size", info.value_size, bpf_map__value_size(map));
        cmp("max_entries", info.max_entries, bpf_map__max_entries(map));
        cmp("map_flags", info.map_flags, bpf_map__map_flags(map));
        if (diff.empty()) {
            reused_pins_ = true;
            if (std::string(bpf_map__name(map)) == PB_MAP_CONNTRACK) reused_conntrack_ = true;
            continue;
        }

        if (!opts_.recreate_maps)
            throw std::runtime_error(
                "pinned map " + path + " does not match this build (" + diff +
                "). It may hold live state, so it is not replaced silently. Restart with "
                "--recreate-maps to unpin and recreate it" +
                (std::string(bpf_map__name(map)) == PB_MAP_CONNTRACK
                     ? " (drops every tracked flow), or set conntrack.size back to " +
                           std::to_string(info.max_entries)
                     : std::string()));
        log::warn("pinned map {} does not match ({}); --recreate-maps given, unpinning it", path,
                  diff);
        if (::unlink(path.c_str()) != 0)
            throw std::runtime_error("unlink " + path + ": " + errstr(errno));
    }
}

bool Loader::try_attach(XdpMode mode) {
    const int prog_fd = bpf_program__fd(skel_->progs.xdp_packetbalance);
    // No XDP_FLAGS_UPDATE_IF_NOEXIST: an existing program in the same mode is
    // replaced atomically, which is what makes a daemon restart hitless.
    int err = bpf_xdp_attach(ifindex_, prog_fd, xdp_flags(mode), nullptr);
    if (err) {
        log::warn("xdp attach {} mode on {} failed: {}", xdp_mode_name(mode), opts_.interface,
                  errstr(err));
        return false;
    }
    attached_mode_ = mode;
    return true;
}

void Loader::attach() {
    bool attached = false;
    const XdpMode current = current_attach_mode(ifindex_, &attached);

    XdpMode first = opts_.mode == XdpMode::Generic ? XdpMode::Generic : XdpMode::Native;
    if (opts_.mode == XdpMode::Auto && attached) {
        // A previous daemon already chose. Keep its mode so the replacement is
        // a single atomic swap, not a detach followed by an attach.
        first = current;
    } else if (attached && current != first) {
        // The kernel will not hold a native and a generic program at once, so
        // switching modes means a gap in forwarding. Say so.
        log::warn("{} has an XDP program in {} mode; detaching it to attach in {} mode "
                  "(not hitless)",
                  opts_.interface, xdp_mode_name(current), xdp_mode_name(first));
        bpf_xdp_detach(ifindex_, xdp_flags(current), nullptr);
        attached = false;
    }

    bool ok = try_attach(first);
    if (!ok && opts_.mode == XdpMode::Auto) {
        const XdpMode other = first == XdpMode::Native ? XdpMode::Generic : XdpMode::Native;
        log::info("xdp_mode auto: falling back to {} mode", xdp_mode_name(other));
        ok = try_attach(other);
    }
    if (!ok) throw std::runtime_error("could not attach XDP program to " + opts_.interface);
    log::info("attached {} to {} (ifindex {}) in {} mode{}", PB_PROG_NAME, opts_.interface,
              ifindex_, xdp_mode_name(attached_mode_), attached ? ", replacing the previous program" : "");
}

void Loader::detach_and_unpin() {
    if (int err = bpf_xdp_detach(ifindex_, xdp_flags(attached_mode_), nullptr))
        log::warn("xdp detach from {}: {}", opts_.interface, errstr(err));
    else
        log::info("detached XDP program from {}", opts_.interface);
    if (int err = bpf_object__unpin_maps(skel_->obj, nullptr))
        log::warn("unpin maps: {}", errstr(err));
    else
        log::info("unpinned maps under {}", opts_.pin_path);
    ::rmdir(opts_.pin_path.c_str());  // only succeeds if nothing else is pinned there
}

}  // namespace pb
