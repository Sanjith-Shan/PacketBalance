# Control-plane bugs

Real bugs hit while building and smoke-testing `packetbalance` (the daemon) in
the Lima VM (Ubuntu 24.04 aarch64, kernel 6.8, libbpf 1.3). Symptom, how it was
found, cause, fix.

## 1. A re-added real got no ring slots for up to a second

**Symptom.** A script that measures ring disruption read the live inner ring
map with `bpftool map lookup pinned .../rings key 0 0 0 0` (for the
`inner_map_id`) and `bpftool map dump id N`, removed a real, and dumped again.
For the second hash mode it reported the removed real held **0** slots. It
had held a quarter of them a moment earlier.

**How found.** That script's output (`removed real_id 2 held 0`), then the
daemon log: after `reload` put the real back, the ring was built with
`reals_in_ring=2/3`, and `neigh: real 10.77.0.3 -> ...` plus a second rebuild
came about 100 ms later.

**Cause.** The ring leaves out any real whose destination MAC is unknown,
since XDP_TX to a zero MAC would drop that real's share of traffic. The MAC
lives in the real's slot in `RealTable`. Deleting the real's last VIP frees the
slot and its MAC. Adding it back allocates a new slot with no MAC, and the ARP
resolver only fills it on its next pass (up to 1 s). Meanwhile the kernel's
ARP table still had the entry.

**Fix.** When a real is added, `LbState` looks the address (or the next hop)
up in `/proc/net/arp` right away. That read is cheap and never probes. The
resolver thread only handles addresses the kernel does not know yet. A re-added
real now goes into the ring in the same swap that adds it.

## 2. Restart could briefly publish an empty ring for a live VIP

**Symptom.** In the first draft, VIPs came up in this order: allocate
vip_id, build and install a ring with no reals, write `vip_map`, then add the
reals and rebuild. On a fresh start that is harmless. On a restart that reuses
pinned maps, the VIP is **already** in `vip_map`, so for a few milliseconds new
connections to it (SYNs, and conntrack misses) would hash into a ring of
`PB_REAL_NONE` and be dropped as `drop_no_real`.

**How found.** Code review of `create_vip_locked` while writing the adoption
path, before the first restart test. Afterwards the restart test showed one ring
build per VIP (`reason=startup (adopted)`, `reals_in_ring=3/3`) and the
synthetic conntrack entry survived.

**Cause.** The ring was published before it held the VIP's reals. That is
only safe while nothing can see the VIP yet.

**Fix.** `create_vip_locked` takes the initial reals and publishes one
complete ring, then writes `vip_map`. Deletion runs in the reverse order
(`vip_map` first, then the ring, then the real_ids). Removing a real works
the same way: rebuild the ring without it, then free its real_id, so a packet
can never hash to an id whose `reals` slot is already empty.

## 3. Restart logs claimed things that were no longer true

**Symptom.** A run with `--recreate-maps --xdp-mode generic` against a daemon
that had been native with a 65536-entry table logged two false lines:
`attached ... in generic mode, replacing the previous program` (the native
program had already been detached, so nothing was replaced) and
`reusing pinned maps ...: tracked flows are preserved` (conntrack had just
been unpinned and recreated empty).

**How found.** Reading the smoke-test log next to
`bpftool map show pinned /sys/fs/bpf/pbdev/conntrack`, which showed a new map
id with `max_entries 1024`.

**Cause.** The "was attached" flag came from before the mode-switch detach,
and "reused pins" was set for any pinned file found, including the one we
then removed.

**Fix.** Clear the flag after the detach, and count a map as reused only when
its definition matched. A new `reused_conntrack()` drives the "flows are
preserved" message. An operator reads these lines during an incident, so they
have to be exact.
