# PacketBalance design

PacketBalance is a Layer 4 load balancer. An XDP program written in C picks a backend
for every packet addressed to a virtual IP (VIP), wraps the packet in an outer IPv4
header addressed to that backend, and transmits it back out the interface it arrived on.
A C++20 daemon owns the configuration, builds the lookup tables, health-checks the
backends and exports metrics. The backends ("reals") answer clients directly, so return
traffic never passes through the load balancer.

None of the design is new. It is the design Meta published for **Katran** in 2018
("Open-sourcing Katran, a scalable network load balancer", engineering.fb.com) and in the
`facebookincubator/katran` repository, built on the consistent hashing algorithm from
**Maglev** (Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load
Balancer", NSDI 2016, section 3.4), running on **XDP** (Høiland-Jørgensen et al., "The
eXpress Data Path", CoNEXT 2018). PacketBalance was written from those descriptions, not
from Katran's source. Where PacketBalance departs from what Katran actually does, this
document says so. The departures are listed together in
[Where PacketBalance differs from Katran](#where-packetbalance-differs-from-katran).

Everything runs in one Linux VM (see `lab/lima.yaml` and `lab/up.sh`). Every measured
number in this repository names the VM, the kernel, the XDP mode, the packet size and
the flow count. Nothing here has seen production traffic or a physical NIC.

## Contents

1. [The pieces](#the-pieces)
2. [A packet, end to end](#a-packet-end-to-end)
3. [The data-plane packet path, in code order](#the-data-plane-packet-path-in-code-order)
4. [Maps](#maps)
5. [Why XDP](#why-xdp)
6. [Why DSR with IPIP, not NAT](#why-dsr-with-ipip-not-nat)
7. [Encapsulation details](#encapsulation-details)
8. [Why Maglev](#why-maglev)
9. [The connection table](#the-connection-table)
10. [TCP and UDP handling](#tcp-and-udp-handling)
11. [Updating a ring without dropping packets](#updating-a-ring-without-dropping-packets)
12. [One flow hash for both planes](#one-flow-hash-for-both-planes)
13. [When the load balancer dies](#when-the-load-balancer-dies)
14. [What breaks without each mechanism](#what-breaks-without-each-mechanism)
15. [MTU and ICMP](#mtu-and-icmp)
16. [Health checking](#health-checking)
17. [Pinned maps and hitless restart](#pinned-maps-and-hitless-restart)
18. [What PacketBalance does not do](#what-packetbalance-does-not-do)
19. [Where PacketBalance differs from Katran](#where-packetbalance-differs-from-katran)
20. [Sources](#sources)

## The pieces

| Piece | File | Role |
|---|---|---|
| XDP program | `bpf/packetbalance.bpf.c` | Parse, pick a real, encapsulate, `XDP_TX`. Compiled to `packetbalance.bpf.o` and embedded in the daemon as a libbpf skeleton |
| Shared ABI | `include/packetbalance/abi.h` | Every map key, value, counter and flag both planes use |
| Flow hash | `include/packetbalance/hash.h` | The 5-tuple hash and the Maglev backend hashes, included by both planes |
| Maglev | `src/core/maglev.cpp` | Builds a 65,537-slot ring from a set of (address, weight) pairs |
| Daemon | `src/daemon/` | Loader, map state, health checker, neighbor resolver, JSON API, `/metrics` |
| CLI | `src/pbctl/` | `pbctl`, a client for the daemon's Unix socket (see `docs/API.md`) |
| Test clients | `tools/conncheck/`, `tools/hashquality/` | Connection survival (Experiments 3 and 4), hash quality (Experiment 5) |
| Lab | `lab/` | Namespaces, bridge, reals, the IPVS baseline, the experiments |

The lab topology, built by `lab/up.sh`:

```
                 +------------------------------------------------------------+
                 |  Linux VM (root netns holds bridge br0, 10.0.0.1/24)       |
                 |                                                            |
  netns client   |  10.0.0.10  veth0 ---+                                     |
                 |                      |                                     |
  netns lb1      |  10.0.0.2   veth0 ---+---- br0 ---+--- veth0 10.0.0.21  netns real1
                 |  (XDP attached here) |            +--- veth0 10.0.0.22  netns real2
  netns lb2      |  10.0.0.3   veth0 ---+            +--- veth0 10.0.0.23  netns real3
                 |                                   +--- veth0 10.0.0.24  netns real4
                 |                                   +--- veth0 10.0.0.25  netns real5
                 +------------------------------------------------------------+

  VIP 198.51.100.1 (TEST-NET-2, deliberately off-subnet)
  client routes 198.51.100.0/24 via 10.0.0.2 (lb1) or 10.0.0.3 (lb2), flipped by the harness
  each real: VIP on lo, tunl0 up for IPIP decap, rp_filter off, arp_ignore=1, arp_announce=2,
             nginx on :80, UDP echo on :5000, conncheck-server on :7000
  real5 is only used for the drift case of Experiment 4
```

The client's route to the VIP carries `mtu 1480` (see [MTU and ICMP](#mtu-and-icmp)).
In production the client-facing router would reach the load balancers through ECMP over
BGP-announced VIPs. The lab replaces that with a static route the harness flips.

## A packet, end to end

This walks one TCP connection from `connect()` and `send()` on the client to `recv()` on
real1 and back. The addresses are the lab's.

**1. Client stack.** The application calls `connect()` to 198.51.100.1 port 80. The
kernel picks an ephemeral source port, builds a SYN, and looks up the route. The route
for 198.51.100.0/24 points at 10.0.0.2 (lb1) with `mtu 1480`, so TCP advertises an MSS
of 1440. The client resolves 10.0.0.2 with ARP and puts lb1's MAC in the Ethernet
destination. The VIP's own address is never ARPed for. That is why the VIP can be
off-subnet and why no host on the segment has to answer for it.

**2. The wire.** The frame leaves the client's `veth0`, appears on its peer `pb-client`
in the root namespace, and the bridge forwards it by destination MAC to `pb-lb1`, whose
peer is lb1's `veth0`.

**3. XDP on lb1.** The frame arrives on lb1's `veth0`. In native mode the XDP program
runs from the veth's NAPI poll, before lb1's network stack does anything with the
packet. In generic mode it runs at the start of `netif_receive_skb`, after an `sk_buff`
exists. Either way the program sees raw bytes starting at the Ethernet header. It
matches (198.51.100.1, 80, TCP) in `vip_map`, sees a bare SYN, hashes the 5-tuple,
indexes the VIP's Maglev ring, gets real1, records the choice in the connection table,
prepends 20 bytes of outer IPv4, rewrites both MACs and returns `XDP_TX`.

This is the frame before and after, for a Linux SYN with 20 bytes of TCP options:

```
As it arrives on lb1 veth0 (74 bytes)          As it leaves lb1 veth0 (94 bytes)

off  field               value                 off  field               value
---  ------------------  --------------------  ---  ------------------  ------------------------
  0  eth dst             02:00:0a:00:00:02 lb1   0  eth dst             02:00:0a:00:00:15 real1
  6  eth src             02:00:0a:00:00:0a cli   6  eth src             02:00:0a:00:00:02 lb1
 12  ethertype           0x0800                 12  ethertype           0x0800
 14  ver/ihl             0x45                   14  ver/ihl             0x45          outer IPv4
 15  tos                 0x00                   15  tos                 copied from inner
 16  total length        60                     16  total length        80
 18  id                  (client's)             18  id                  0
 20  flags/frag offset   DF, 0                  20  flags/frag offset   0 (DF clear)
 22  ttl                 64                     22  ttl                 64
 23  protocol            6 (TCP)                23  protocol            4 (IPIP)
 24  header checksum     (client's)             24  header checksum     computed in XDP
 26  saddr               10.0.0.10              26  saddr               10.99.0.<flow hash bits>
 30  daddr               198.51.100.1           30  daddr               10.0.0.21
 34  tcp sport           ephemeral              34  inner IPv4 header   bytes 14..33 of the
 36  tcp dport           80                         (20 bytes)          original, unchanged,
 38  seq                 client ISN                                     TTL not decremented
 42  ack                 0                      54  inner TCP header    bytes 34..73 of the
 46  data offset/flags   10 words, SYN              + options           original, unchanged
 48  window              ...
 50  checksum            ...
 52  urgent              0
 54  options             MSS 1440, SACK, TS,
                         WS
 74  end                                        94  end
```

`bpf_xdp_adjust_head(ctx, -20)` moves the start of the packet 20 bytes earlier into the
driver's headroom. After the move, the old Ethernet header sits at offsets 20 to 33,
under what becomes the outer IP header. The program does not move it. It writes a fresh
Ethernet header at offset 0 (both MACs are new anyway) and the outer IP header over the
old Ethernet bytes. The inner IP header and everything after it are not touched, so no
inner checksum changes.

**4. Back onto the wire.** `XDP_TX` hands the frame back to lb1's `veth0` transmit side.
It appears on `pb-lb1`, and the bridge forwards it by destination MAC to `pb-real1` and
into real1. On a veth, `XDP_TX` only works if the peer device runs NAPI, which on kernel
6.8 means the peer has an XDP program or GRO enabled. `lab/up.sh` turns GRO on for
`pb-lb1` and `pb-lb2` for this reason. Without it, native-mode `XDP_TX` frames vanish
without a counter in PacketBalance's stats.

**5. Decapsulation on real1.** real1's IP layer receives a packet addressed to its own
address 10.0.0.21 with protocol 4. The `ipip` module hands it to the fallback tunnel
device `tunl0` (local any, remote any), which strips the outer header and injects the
inner packet as if it had arrived on `tunl0`. Reverse-path filtering runs here. The inner
source is the client, 10.0.0.10, and real1's route to the client is out `veth0`, not
`tunl0`, so strict `rp_filter` would drop the packet. `rp_filter` is 0 on the reals.
The inner destination 198.51.100.1 is local because the VIP is configured on `lo`, so
the packet is delivered to TCP. nginx listens on the wildcard address, so its socket
matches.

**6. The reply.** real1's TCP answers with a SYN-ACK from 198.51.100.1 to 10.0.0.10.
The route to the client is directly out `veth0`, so real1 ARPs for 10.0.0.10 and sends
the frame across the bridge to the client. The load balancer never sees it. The
client's TCP sees a SYN-ACK from the address it connected to and completes the
handshake.

**7. The rest of the connection.** The client's ACK and every later client packet go to
lb1 again. None of them is a bare SYN, so the program looks the 5-tuple up in the
connection table first, finds real1, and encapsulates without consulting the ring.
`send()` on the client becomes segments that take the same path. `recv()` on real1
returns once TCP has queued the payload. Every response byte goes directly from real1
to the client.

## The data-plane packet path, in code order

This is the order of `xdp_packetbalance()` in `bpf/packetbalance.bpf.c`. Each drop is
counted under the reason named in `enum pb_counter` in `abi.h`.

1. **Ethernet.** Truncated header: drop, `short`. EtherType not IPv4 (ARP, IPv6, VLAN,
   LLDP): `XDP_PASS` to the host stack, counted as `pass`.
2. **IPv4 header checks.** Truncated: drop, `short`. Version not 4: drop, `other`.
   `ihl != 5` (IP options): drop, `opts`. MF set or non-zero fragment offset: drop,
   `frag`. `tot_len` shorter than a header or longer than the frame: drop, `short`.
   These checks run before the VIP lookup, so they also apply to fragmented or
   option-carrying traffic addressed to the load balancer host itself.
3. **Config.** Read `config[0]` (LB MAC, encap source prefix, feature flags). Missing:
   drop, `other`.
4. **Layer 4.** TCP: read ports, set `bare_syn = SYN && !ACK`. UDP: read ports. ICMP:
   go to the PMTU path (step 11). Any other protocol: `XDP_PASS`.
5. **VIP lookup.** `vip_map[(daddr, dport, proto)]`. Miss: the packet is for the host,
   `XDP_PASS`.
6. **Hash and count.** Compute `pb_flow_hash(saddr, daddr, sport, dport, proto)`. Count
   `packets`, `bytes` and, for a bare SYN, `syn`.
7. **Length check.** Inner length plus 20 bytes of outer header above
   `PB_MAX_PACKET_LEN` (3,500): drop, `mtu`. This runs before the connection table so a
   packet that will be dropped does not create a flow entry.
8. **Pick the real.**
   - Conntrack disabled daemon-wide or for this VIP: ring lookup, count `hash`.
   - Otherwise, if not a bare SYN, look the 5-tuple up in `conntrack`. A hit with a
     non-zero timestamp whose real still has an address in `reals` refreshes the
     timestamp and returns the stored `real_id` (`ct_hit`). Anything else counts
     `ct_miss` and falls through: no entry, another CPU's zero-filled copy, or a stale
     entry whose real was deleted.
   - Ring lookup: `rings[vip_id][hash % 65537]`, count `hash`. Insert the result into
     `conntrack` (overwriting a stale entry) unless it is `PB_REAL_NONE`, the packet is
     ICMP, or the packet is a TCP RST.
9. **Resolve the real.** `real_id` out of range (including the `PB_REAL_NONE` sentinel),
   an empty `reals` slot, or a missing `neigh` entry: drop, `no_real`.
10. **Encapsulate and transmit.** `bpf_xdp_adjust_head(-20)` (failure: drop,
    `adj_head`), write Ethernet and the outer IPv4 header, compute the outer checksum,
    count `tx` and the per-real `real_stats`, return `XDP_TX`.
11. **ICMP.** If the PMTU feature flag is off, or the message is not "destination
    unreachable, fragmentation needed", `XDP_PASS`. Otherwise parse the quoted inner
    header, which is the real's reply (VIP to client). Swap it back into the client to
    VIP direction, look the VIP up, and send the ICMP message through steps 6 to 10 to
    the real that owns the flow, without inserting into the connection table. Counted
    as `icmp_pmtu_fwd`.

## Maps

| Map | Type | Key → Value | Written by |
|---|---|---|---|
| `vip_map` | `HASH` | (daddr, dport, proto) → vip_id, flags | control plane |
| `rings` | `ARRAY_OF_MAPS`, one inner `ARRAY[65537]` of `u32 real_id` per VIP | vip_id → ring | control plane, swapped atomically |
| `reals` | `ARRAY` | real_id → ipv4, flags | control plane |
| `neigh` | `ARRAY` | real_id → mac | control plane |
| `conntrack` | `LRU_PERCPU_HASH` | 5-tuple → real_id, ts | data plane |
| `stats` | `PERCPU_ARRAY` | vip_id → counters | data plane |
| `real_stats` | `PERCPU_ARRAY` | real_id → packets, bytes | data plane |
| `config` | `ARRAY[1]` | lb mac, encap source prefix, feature flags | control plane |

Sizes come from `abi.h`. There are at most 64 VIPs and 512 reals, each ring has 65,537
slots, and the connection table defaults to 2^20 entries (`conntrack.size`, fixed when
the map is created). The size counts keys, that is flows, across all CPUs, not per CPU.
real_id 0 is never allocated, and 511 reals are usable. Every map is pinned by name
under the daemon's pin path (default `/sys/fs/bpf/packetbalance`). The pin path must be
on a bpffs mount. The daemon creates the directory and refuses to start if `statfs`
says it is not bpffs. That matters under `ip netns exec`, which mounts a fresh sysfs on
`/sys` and so hides the host's `/sys/fs/bpf`. The lab therefore mounts its own bpffs at
`/run/pblab/bpf` and gives each LB a directory there (`/run/pblab/bpf/lb1`,
`/run/pblab/bpf/lb2`). Addresses and ports are stored in network byte order, as
they are on the wire, so the lookup path never byte-swaps.

`stats` has `PB_MAX_VIPS + 1` entries. The last one, `PB_STATS_GLOBAL`, collects
counters for packets that never matched a VIP (passes, and drops that happen before the
VIP lookup). The daemon sums every per-CPU copy when it reads.

## Why XDP

A Linux host forwarding a packet through its stack allocates an `sk_buff`, runs GRO,
netfilter hooks, a routing lookup and qdisc, and frees the skb. For a load balancer that
only needs to read 54 bytes of headers and prepend 20, almost all of that is overhead.
XDP runs a verified eBPF program on the raw receive buffer, in the driver, before any of
it happens. The program decides the packet's fate with a return code, and `XDP_TX` puts
the same buffer on the same device's transmit ring. No skb, no copy, no stack.

The XDP paper measured this directly. On one core, an XDP program dropping packets ran
at 24 Mpps where the Linux stack's fastest mode (raw table drop) reached 4.8 Mpps, and
its load-balancer test (Katran's XDP program against IPVS, Table 2) showed both scaling
linearly with cores with XDP about 4.3 times faster. Those are the paper's numbers on
its hardware, not PacketBalance's.

The alternative with similar speed is kernel bypass (DPDK, netmap, the model the Maglev
paper describes). It takes the NIC away from the kernel and needs a dedicated
busy-polling core. XDP shares the NIC with the host stack (`XDP_PASS` for anything not a
VIP), costs nothing when idle, and lets a load balancer run on the same host as other
services. Meta's post gives that as a reason Katran could be colocated with backends.

There are three attach modes, and they are not the same system.

| Mode | `ip link` shows | Where it runs | Cost |
|---|---|---|---|
| Native (`xdpdrv`) | `prog/xdp id N` | In the driver's NAPI poll, on the DMA buffer, before an skb exists | The fast path. Needs driver support and headroom. `XDP_TX` needs a TX queue per CPU on real NICs |
| Generic (`xdpgeneric`) | `prog/xdpgeneric id N` | In `netif_receive_skb`, after the skb is allocated and after GRO | Works on any device. Pays for the skb allocation, may have to linearize or expand the skb for headroom, and can see GRO-coalesced frames larger than the MTU. It is a compatibility mode |
| Offload (`xdpoffload`) | `prog/xdpoffload id N` | On the NIC itself (Netronome NFP) | No host CPU at all, but only a subset of maps and helpers. PacketBalance's map-in-map and per-CPU LRU are not offloadable, and neither veth nor virtio supports it |

PacketBalance tries native first and falls back to generic (`xdp_mode: auto`). Every
result row records which mode ran, because generic mode's cost is the honest cost of a
device without driver support. If generic mode does not beat IPVS in Experiment 1, that
is published.

The lab has a caveat of its own. PacketBalance attaches to a veth. Native XDP on a veth
runs in the veth's NAPI context, but the frame started life as an skb on the sending
side, and the veth converts it to an XDP buffer, copying if the skb is shared or lacks
headroom. So "native" on veth avoids the receiving stack but not the skb. A physical NIC
in native mode avoids both. [CAPACITY.md](CAPACITY.md) covers what that means for the
numbers.

## Why DSR with IPIP, not NAT

A NAT load balancer rewrites the destination of each client packet to the backend and
must rewrite the source of each reply back to the VIP. That puts both directions of
every connection through the load balancer. Web responses are usually far larger than
requests, so the load balancer carries most of the bytes, and it needs per-connection
state to reverse the translation, so losing a load balancer loses its connections unless
state is replicated. Source NAT on top of that hides the client's address from the
backend.

Direct server return (DSR) removes the return path. The load balancer delivers the
client's packet to the backend unchanged, the backend owns the VIP locally, and it
replies to the client itself with the VIP as the source. The load balancer carries only
the client-to-server direction, which in a request/response workload is mostly small
requests and ACKs. That is how an L4 load balancer can front far more bandwidth than it
forwards.

There are two ways to deliver the unchanged packet. Layer 2 DSR (IPVS `-g`, "gatewaying")
rewrites only the destination MAC, which requires the load balancer and every real to
share one L2 segment. IPIP (IPVS `-i`, Katran, PacketBalance) wraps the packet in an
outer IP header addressed to the real, which works across any routed network. Katran
requires an L3 topology and sends every encapsulated packet to the first router. In the
lab everything is on one bridge, so PacketBalance writes the real's own MAC from `neigh`
where an L3 deployment would write the router's MAC. The lab's IPVS baseline uses `-g`,
which does less work per packet than PacketBalance does (no encapsulation, no outer
checksum). That favors IPVS in Experiment 1 and is stated with the results.

What DSR costs is configuration on every real. Each real needs:

| Setting | Why |
|---|---|
| `ip addr add 198.51.100.1/32 dev lo` | The decapsulated packet is addressed to the VIP. The real must consider it local, or the stack drops or forwards it |
| `modprobe ipip` and `ip link set tunl0 up` | `tunl0` is the fallback IPIP device that accepts IPIP from any source to a local address and decapsulates it |
| `net.ipv4.conf.tunl0.rp_filter=0` and `net.ipv4.conf.all.rp_filter=0` | The inner source is the client, whose route is not via `tunl0`. Strict reverse-path filtering drops every decapsulated packet. The effective value is the maximum of `all` and the device, so both must be 0 (or 2, loose) |
| `net.ipv4.conf.all.arp_ignore=1` | Linux answers ARP for any local address on any interface. With the VIP on `lo`, a real on the same segment as the router would answer ARP for the VIP and steal traffic. `1` answers only for addresses on the receiving interface |
| `net.ipv4.conf.all.arp_announce=2` | Never use the VIP as the source address of the real's own ARP requests, which would poison neighbors' caches |
| Services bound to the wildcard address or to the VIP | The SYN arrives addressed to the VIP |

In the lab the VIP is off-subnet, so nobody ARPs for it and the ARP settings are not
load-bearing. They are set anyway because they are load-bearing the moment a VIP shares
a subnet with anything.

What DSR also costs is visibility. The load balancer never sees a response, so it
cannot measure backend latency, cannot see a reset from the server, and cannot do
anything at Layer 7.

## Encapsulation details

### The outer source address

The real's NIC spreads incoming packets across receive queues with RSS, which hashes
packet headers. For an IPIP packet most NICs hash only the outer addresses, because the
ports are behind the inner header. If every encapsulated packet from a load balancer had
the same outer source (the LB's address), the outer (source, destination) pair would be
constant, every flow from that load balancer would land on one receive queue on the real,
and one of the real's cores would do all the decapsulation and TCP receive work.

Katran's answer, which PacketBalance copies, is to make the outer source a function of
the flow. PacketBalance uses `encap_src_prefix | (flow_hash & ~encap_src_mask)`, a /24 by
default (`10.99.0.0/24`), so one load balancer produces 256 distinct outer sources. The
same flow always gets the same source, so it always lands on the same queue and is never
reordered. Katran uses `172.16.0.0/16` with the low 16 bits derived from the client's
source port and address.

The outer source is synthetic. Nothing routes back to it. That has two consequences. The
real's reverse-path filter must not check it (`rp_filter` off on the receiving device, or a
route for the prefix), and an ICMP error about the outer packet can never reach the load
balancer, which is why the outer header does not set DF (below). In the lab the reals'
veths have one queue per CPU but no hardware RSS, so this trick has no measurable effect
here. It is in the design because a real NIC needs it.

### The outer header fields

- **TTL 64.** The outer header needs its own TTL for the routers between LB and real.
  The inner TTL is left as the client sent it. The load balancer does not act as a
  router hop for the inner packet.
- **DF clear, id 0.** If a router between the LB and the real cannot forward the outer
  packet, it may fragment it and the real reassembles before decapsulating. With DF set,
  it would send ICMP to the synthetic outer source and the packet would disappear.
  Katran also clears DF on the outer header.
- **TOS copied** from the inner header, so DSCP marking survives.
- **Checksum computed in the program.** The IPv4 header checksum covers only the 20
  bytes of the outer header, and the receiver's IP layer drops a packet whose header
  checksum is wrong. An XDP frame carries no skb, so there is no checksum-offload
  metadata to ask the NIC to fill it in. The program sums the ten 16-bit words and folds
  the carry twice (ten words cannot overflow more than that), a fixed and small cost. The
  inner TCP or UDP checksum does not change, because its pseudo-header uses the inner
  addresses and the inner packet is untouched. This is a quiet advantage of
  encapsulation over NAT, which must patch both the IP and the L4 checksum on every
  packet.

## Why Maglev

The ring answers "which real gets this flow" from the flow's hash alone. Three
properties matter. Every real's share of flows must match its weight (balance). When a
real is added or removed, as few flows as possible should change real (disruption). And
the lookup runs per packet in a verified program, so it must be cheap and bounded.

**Modulo** (`real = reals[hash % N]`) balances well and is one instruction, but when N
changes, almost every flow's `hash % N` changes. Removing one of four reals moves about
three quarters of all flows, not one quarter. PacketBalance keeps a modulo ring
(`hash: modulo`) as the naive baseline so Experiments 3, 4 and 5 can show that. In
`results/exp5_hash_quality.md` (one million synthetic flows, weights 1:1:2:4), removing
the weight-2 real moved 58.36% of flows under modulo, against a minimum possible of 25%.

**Ring (Karger or ketama) consistent hashing** places each real at many points on a
circle and sends a flow to the next point clockwise. Disruption is minimal, but balance
depends on the number of points per real, and the lookup is a binary search over all
points. The Maglev paper measured that with 1,000 backends and a 65,537-entry table,
Karger hashing needed backends overprovisioned by 29.7% to absorb its imbalance.

**Rendezvous (highest random weight) hashing** scores every real for each flow and picks
the highest score. Disruption is minimal and there is no table to build, but the lookup
is O(N) hash computations per packet. In an XDP program that is a loop over every real
of the VIP for every packet, which is both slow and a bounded-loop cost the verifier has
to accept for the maximum N. The Maglev paper's own comparison precomputes rendezvous
into a table of the same size and still found it needed 49.5% overprovisioning at 65,537
entries. Maglev's authors picked their algorithm for balance, not lookup speed, because
both alternatives can be tabulated.

**Maglev** gives each real a pseudo-random permutation of the table slots and lets the
reals take turns claiming their most-preferred empty slot until the table is full. Each
real ends up with either floor(M/N) or ceil(M/N) slots, so shares differ by at most one
slot. Lookup is one array index, O(1), no loop. The cost is slightly more than minimal
disruption. When a real leaves, its slots are redistributed, and a few slots that
belonged to surviving reals also change hands, because the turn order shifts. In
`results/exp5_hash_quality.md`, removing the weight-2 real moved 25.09% of flows against
the 25% minimum, and adding a weight-1 real moved 11.11% against 11.11%. Maglev's
authors accept that trade explicitly. A small number of extra disruptions is tolerable
because the connection table protects existing flows (next section).

### The algorithm

From section 3.4 of the paper, as implemented in `src/core/maglev.cpp`:

```
offset_i = h1(addr_i) mod M
skip_i   = h2(addr_i) mod (M - 1) + 1
permutation_i[j] = (offset_i + j * skip_i) mod M
```

Each backend keeps a cursor into its permutation. In each round every backend advances
its cursor to the first slot still empty and claims it, until all M slots are claimed.
The expected number of probes for the whole fill is about M ln M, independent of N.
The permutations are never materialized. Each backend keeps only its current position
and adds `skip`.

### Why M is prime

`permutation_i` visits every slot exactly once only if `skip_i` and M share no common
factor. With M prime, every `skip` in 1 to M-1 qualifies, so every backend's preference
list is a full permutation and the fill always terminates. With a composite M, a backend
whose skip shared a factor with M would cycle through a fraction of the table and could
spin forever looking for an empty slot.

The size is 65,537 (2^16 + 1, a prime), Maglev's own default. The paper recommends
M > 100 × N to keep the imbalance under 1%. At 65,537 slots that holds up to about 655
reals per VIP, which covers PacketBalance's limit of 511 usable reals.
A larger M improves resilience to many simultaneous failures but costs build time (the
paper measured 1.8 ms at 65,537 against 22.9 ms at 655,373) and memory (4 bytes per
slot, 256 KiB per VIP).

### Weights

The paper says weights are done "by altering the relative frequency of the backends'
turns" and does not give details. PacketBalance gives a real of weight w exactly w turns
per round, interleaved. A round is `w_max` passes over the backends, and on pass t a
backend takes a turn only if t < w. Over the whole fill each backend claims
w / sum(w) of the slots. Experiment 5 checks the shares against the weights.

Katran has two versions. Its original `MaglevHash` gives a backend `weight` turns in the
first round only and one turn per round after that. `MaglevHashV2` accumulates weight
each round and grants a turn when the accumulator passes the maximum weight, which gives
proportional shares. PacketBalance's scheme is closer to V2 in effect.

Weight 0 means the real takes no slots. That is how draining works.

### Keyed by address, not by real_id

`h1` and `h2` hash the real's IPv4 address (`pb_backend_hash_offset`,
`pb_backend_hash_skip` in `hash.h`), and the backends are sorted by address before the
fill. The ring is therefore a function of the set of (address, weight) pairs and nothing
else. `real_id` is an allocation artifact. It depends on the order reals were added and
on which ids were freed earlier, and two load balancers, or one daemon before and after
a restart, can number the same reals differently. Keying the permutation by `real_id`
would give them different rings from the same configuration, and the failover argument
below would fall apart. The ring's slot values are `real_id`s, which are local to each
load balancer, but the address each slot resolves to is the same everywhere. Katran also
keys the permutation by a hash of the real's address.

IPVS `mh` is order-dependent in a way PacketBalance is not. It keys the permutation by
address and port, but fills in the order destinations were added, so two IPVS instances
agree only if their reals were added in the same order.

## The connection table

### Why keep one if the hash is consistent

Consistent hashing keeps most flows on the same real when the ring changes. Most is not
all. When a real is removed, added, or reweighted, a fraction of slots change owner, and
every established connection that hashes to one of those slots would be sent to a real
that has no socket for it. That real answers with a RST. The connection table records
the real chosen when the connection started, so an established flow keeps going to its
original real regardless of what the ring does later. Maglev's paper uses the two the
same way. Consistent hashing is the fallback, with "connection tracking as the primary
means of protection".

The table is also why a ring change is safe to make at any time. It limits the damage of
a change to flows that were not in the table, which are flows whose packets never passed
through this load balancer's current table (new, evicted, or arriving from a different
load balancer).

Katran found something else, and Meta's post states it plainly. "Computing the hash is
computationally easier than looking up the local state table." A jhash over three words
and one array index can be cheaper than a hash-table lookup that misses in cache,
especially for large tables. Katran therefore treats the table as an LRU cache with a
tunable size, "a tunable parameter to strike a balance between computation and lookup",
and never depends on it for correctness when the ring has not changed. Experiment 6
measures whether that holds for PacketBalance by running Experiment 1 with conntrack off
and at 64K, 1M and 8M entries.

### Why per-CPU and LRU

**Per-CPU.** The table is written on the fast path (a new flow inserts, every hit
refreshes a timestamp). A shared table would need atomic operations or locks that bounce
cache lines between cores. A per-CPU value means each CPU writes only its own copy, so
the program needs no synchronization and throughput scales with receive queues, which is
the property Katran's README calls "performance scales linearly with a number of NIC's
RX queues". The Maglev forwarder does the same thing, one connection table per packet
thread.

**LRU.** The table has a fixed size and must never fail to accept a new flow. An LRU map
evicts the least recently used entry on insert when full, so idle flows age out without
any timer or garbage collector. That is also why FIN and RST do not delete entries (see
[TCP and UDP handling](#tcp-and-udp-handling)).

### What per-CPU misses

`BPF_MAP_TYPE_LRU_PERCPU_HASH` shares keys across CPUs and keeps one value per CPU. When
CPU A inserts a key from a BPF program, the kernel zero-fills the value slot of every
other CPU. If a later packet of the same flow is processed on CPU B, the lookup finds the
key and returns B's all-zero value. The program treats a zero `last_seen_ns` as a miss,
because `bpf_ktime_get_ns()` is never zero after boot, and falls through to the ring.
The control plane never allocates real_id 0 either, so a zero value could not be
mistaken for a real even without the timestamp test. `pbctl flows` skips the zero-filled
copies the same way. On the miss, CPU B inserts its own value for the key; the update
touches only B's copy, so A's entry is unchanged.

A flow whose packets land on different CPUs therefore misses the table on the second
CPU. That is harmless as long as the ring has not changed since the flow started,
because the ring lookup is deterministic and returns the same real CPU A chose. It is
harmful only when both happen at once, a CPU change and a ring change during the flow's
lifetime. RSS makes that rare. The NIC hashes each flow to one receive queue, each queue
is served by one CPU, and so all packets of a flow are normally processed on the same
CPU. It changes when RSS indirection tables or IRQ affinity are reconfigured, or when a
queue's interrupt moves.

Katran gets per-CPU tables differently, with an `ARRAY_OF_MAPS` of ordinary `LRU_HASH`
maps indexed by CPU number. See
[Where PacketBalance differs from Katran](#where-packetbalance-differs-from-katran) for
what that changes in memory.

### Sizing

The LRU is preallocated at load time, and its size cannot change without recreating the
map (which loses the table). The size is the number of keys shared by all CPUs. Each key
carries one value per possible CPU, so memory grows with the CPU count, but the flow
capacity does not. [CAPACITY.md](CAPACITY.md) computes its memory for 1M and 8M
entries. Too small a table evicts established flows, which then fall back to the ring and
survive unless the ring changes. So an undersized table fails quietly and only under
churn.

## TCP and UDP handling

**SYN goes to the hash.** A bare SYN (SYN set, ACK clear) is a new connection by
definition. The program does not consult the table for it. It hashes, and inserts or
overwrites the entry with `BPF_ANY`. This means a SYN always gets a placement from the
current ring, and a reused 5-tuple (a new connection from a port that was recently in
TIME_WAIT) does not inherit a stale placement.

**Everything else goes to the table first.** SYN-ACK never reaches the LB (it is a reply).
ACKs, data, FIN and RST from the client look up the table, and on a miss fall through to
the ring. ACKs, data and FIN insert the result. That fall-through is what lets a second
load balancer with an empty table pick up an established connection.

**A deleted real is a miss.** An entry can name a real that has since been deleted
(its `reals` slot is empty). The program treats that as a miss, hashes the packet to a
live real and overwrites the entry. The connection was already lost with its backend;
this way the client hears a RST from the new real at once instead of timing out while
the LB black-holes the flow until LRU eviction. real_ids are per address and shared
between VIPs, so a real deleted from one VIP but still serving another keeps its slot,
and that VIP's tracked flows keep reaching it. A real that is only down (failed health
checks) keeps its slot too, see [Health checking](#health-checking).

**FIN and RST do not delete.** Deleting on FIN buys nothing, since the LRU evicts idle
entries when it needs space. It costs correctness. The last ACK of a close, a
retransmitted FIN, or a RST sent after a FIN arrives after the delete, misses, and
re-hashes. If the ring changed during the connection, that packet goes to a real that has
never heard of the connection and answers with a RST of its own. Katran does not delete
on FIN either.

**RST does not insert.** A RST that misses the table (a stray, a scan, a reset for a flow
another LB carried) is forwarded by hash but creates no entry, so a flood of RSTs cannot
fill the table with dead flows. A RST that hits follows the entry like any other packet.
Katran does the same.

**Half-open connections.** A SYN inserts an entry immediately, before the handshake
completes. A SYN flood fills the table with entries for connections that never complete,
and the LRU evicts established flows to make room. Those flows then depend on the ring.
Katran protects against this with a per-CPU new-connection rate limit. Above
`MAX_CONN_RATE` (125,000 per second by default) it stops inserting into the LRU.
PacketBalance does not.

**Sequence numbers are not tracked.** The table is keyed on the 5-tuple only. It does not
know about TCP state, sequence numbers, or window. It is not a firewall conntrack and does
not validate anything. It remembers a routing decision.

**UDP is treated as flows.** A UDP "flow" is the 5-tuple, and there is no handshake to
mark a start. The first packet of a 5-tuple hashes, later ones hit the table, and the
LRU evicts idle ones. Katran additionally expires UDP entries after 30 seconds of
inactivity (`LRU_UDP_TIMEOUT`), so a quiet UDP flow re-hashes against the current ring
when it resumes. PacketBalance relies on LRU eviction alone, so an idle UDP entry can
outlive a ring change and keep a resumed flow on a drained real.

## Updating a ring without dropping packets

A backend change produces a new 65,537-entry table. If the daemon wrote it slot by slot
into the live ring, packets arriving during the write would be routed against a mix of
old and new slots. PacketBalance avoids that with `BPF_MAP_TYPE_ARRAY_OF_MAPS`. The outer
map `rings` holds one inner `ARRAY[65537]` per VIP. The daemon creates a fresh inner map,
fills it completely, and replaces the VIP's slot in the outer map with one
`bpf_map_update_elem` call. A BPF program reads the outer map with
`bpf_map_lookup_elem`, which returns a pointer to one inner map, so every packet sees
either the whole old ring or the whole new ring. The old inner map is freed when the
last reference drops. Each swap increments the VIP's `generation`, which `pbctl ring show`
and `pb_ring_generation` expose, so ring churn is visible.

The daemon also orders the writes so the data plane never sees a VIP without a ring. A new
VIP gets its ring before its `vip_map` entry, and a deleted VIP loses its `vip_map` entry
before its ring.

This is not what Katran does. Katran keeps all rings in one flat `ARRAY` (`ch_rings`,
indexed by `vip_num * RING_SIZE + slot`) and batch-writes only the slots that changed.
That is not atomic across slots, and it does not need to be. A flow reads exactly one
slot, each 4-byte slot write is atomic, and the connection table protects established
flows. The only effect of a half-written ring is that, for a few milliseconds, some new
flows are placed by the old ring and some by the new one. Katran's approach writes about
1/N of the table per change instead of all of it. PacketBalance's approach costs a full
256 KiB write and a map allocation per change, and buys a single generation number, a
simple invariant to test, and no reasoning about mixed rings. At the change rates a
health checker produces, the cost is small. It is a choice, not a correctness
requirement.

## One flow hash for both planes

`include/packetbalance/hash.h` defines `pb_flow_hash`, a Jenkins lookup3 final mix over
(saddr, daddr, ports) with a seed that folds in the protocol. The XDP program and the
control plane both include this header. There is exactly one definition.

Two things depend on it.

- **Load balancer agreement.** lb1 and lb2 pick the same real for a flow only if they
  compute the same hash, index a ring of the same size, and the rings have the same
  contents. Same config plus address-keyed permutations gives the same ring contents.
  Same header gives the same hash. Change the hash on one load balancer (a different
  seed, or the kernel's `jhash_3words`, which pre-mixes differently) and every flow that
  moves between them lands on a different real. `docs/bugs/core.md` records that the
  header's comment once claimed to match the kernel's jhash, and it does not.
  `tests/core/hash_test.cpp` pins the function's output so a change is caught in CI.
- **Experiment 5.** The hash-quality tool uses `pb_flow_hash` so its share and
  disruption numbers describe what the data plane will actually do.

Ring agreement has one more input people forget, which is the set of reals each load
balancer considers eligible. A real is in the ring only if it is up (by that load
balancer's own health checks), has weight above zero, and has a resolved MAC. If lb1 sees
real3 as down and lb2 sees it as up, their rings differ, and flows moving between them can
break. Independent health checking per load balancer is simple and has no shared state,
and this is its price.

## When the load balancer dies

In production, routers spread traffic for a VIP across several load balancers with
ECMP, each load balancer announcing the VIP over BGP. When one dies, the routers
withdraw its route and rehash its flows onto the survivors. The survivor has never seen
those flows. Every packet misses its connection table and falls through to the ring. The
survivor's ring was built from the same set of (address, weight) pairs with the same
algorithm and the same hash, so it names the same real the dead load balancer used, and
the connection survives. Nothing was synchronized between the two load balancers. That
is what "stateless load-balancer tier" means. The state that matters, the mapping from
flow to real, is recomputable from the packet and the configuration.

What that buys operationally:

- Any load balancer can be restarted, upgraded, or drained from ECMP without warning its
  peers and without breaking connections.
- Capacity is added by adding load balancers, with no state migration.
- There is no replication protocol, no split-brain, and no cold-standby failover
  procedure.

What still breaks:

- Flows whose placement came from the connection table and differs from the current ring,
  meaning connections that started before a ring change. On the surviving load balancer
  they re-hash to the new ring's owner. Experiment 4's drift case adds real5 to lb2 only
  before the flip, and Maglev predicts about 1/N of flows break, with N the number of
  reals after the change. Modulo predicts almost all.
- Flows on a real that differs in eligibility between the two load balancers (above).
- Non-resilient ECMP rehashes flows between the surviving load balancers too when the set
  changes. The same argument covers them.

IPVS `rr` cannot do this at all. A second IPVS instance round-robins each connection it
has not seen, so a flow lands on a random real and roughly (N-1)/N of connections break
(with `sloppy_tcp=1`, which lets IPVS accept a mid-connection packet. Without it, IPVS
does not schedule non-SYN packets and every moved connection breaks). IPVS `mh` is Maglev
inside the kernel and should survive the flip as well as PacketBalance, provided both
IPVS instances use `mh-port` (by default `mh` hashes only the source address, which puts
every connection from one client on one real) and `sloppy_tcp`. The lab configures both.
If IPVS `mh` matches PacketBalance in Experiment 4, that is the expected result. It shows
that consistent hashing plus a connection table is what makes a stateless tier possible,
whichever forwarding plane implements it.

## What breaks without each mechanism

Experiment 3 holds 10,000 long-lived connections (`tools/conncheck`) and removes then
re-adds real3. Experiment 4 moves the same connections from lb1 to lb2.

| Configuration | Expected in Experiment 3 (real3 removed, re-added) | Expected in Experiment 4 (flip to lb2) |
|---|---|---|
| Maglev + conntrack (default) | Only real3's connections break | Near zero break, about 1/N in the drift case |
| Maglev, `--no-conntrack` | real3's connections break, plus flows on slots that Maglev reassigned among survivors, plus, when real3 comes back, every flow now hashing back to real3 | Same as default, since lb2 has no table either way |
| Modulo + conntrack | Only real3's while lb1 keeps its table | Almost all, because lb2 re-hashes every flow and the modulo ring changed |
| Modulo, `--no-conntrack` | Almost all, twice (on removal and on re-add) | Almost all in the drift case |

The first row is the design. The other rows are the argument for each mechanism.
Conntrack protects flows from ring changes on one load balancer. Consistent hashing
protects flows from changes of load balancer. Neither covers the other's case. Results
are in the README under Experiments 3 and 4.

## MTU and ICMP

Encapsulation adds 20 bytes. A client packet of 1,500 bytes becomes 1,520, which does not
fit a 1,500-byte link toward the real. XDP cannot fragment, and neither PacketBalance nor
Katran tries. On a veth the oversized frame is dropped by the receiving peer's length
check. On a physical network a switch or router drops it. Either way small packets work
and full-sized ones vanish, which is the classic PMTU black hole. The first symptom is
usually a connection that handshakes and then stalls on the first large write.

There are three fixes, and a deployment uses the first two together.

1. **Raise the MTU between load balancers and reals** by at least 20 bytes. Katran lists
   this as a requirement. PacketBalance's hard ceiling, `PB_MAX_PACKET_LEN`, is 3,500
   bytes, the same 3.5 KB limit Katran documents. Anything longer is dropped and counted
   as `mtu`.
2. **Advertise a smaller MSS** so clients never send full-sized segments. Katran's README
   recommends advertising 1450 instead of 1460 even when the MTU is raised, because it
   also helps clients behind PPPoE. The lab does it on the client side with `mtu 1480` on
   the route to the VIP, which makes the client advertise and use MSS 1440. On real hosts
   it is `advmss` on the reals' routes or an MSS clamp. UDP has no MSS, so UDP
   applications must keep datagrams under the path MTU minus 20.
3. **Forward ICMP "fragmentation needed" to the right real** (the `icmp_pmtu_fwd` path,
   off unless the `PB_CFG_F_ICMP_PMTU` flag is set, which the daemon does for
   `icmp_pmtu: true` or `--icmp-pmtu`). This solves a different problem, PMTU
   discovery on the return path. The real sends responses directly to the client with
   the VIP as the source. When a router on that path cannot forward a DF packet, it sends
   ICMP frag-needed to the packet's source, the VIP, which is routed to the load balancer,
   not to the real that sent the packet. Unless the load balancer forwards the ICMP to
   that real, the real never lowers its path MTU and the connection stalls on large
   responses. The program reads the quoted header inside the ICMP message (VIP to
   client), reverses it to the forward direction (client to VIP), and runs it through the
   same hash and table as the flow, so it reaches the same real even on a load balancer
   that has never seen the flow. It does not insert into the table, because the ICMP
   message is about a flow, not part of it. Katran does the same, and can also generate
   ICMP "packet too big" toward the client when a packet exceeds its maximum size
   (`ICMP_TOOBIG_GENERATION`). PacketBalance does not generate ICMP.

## Health checking

The daemon runs a TCP connect check against each real's own address on the VIP's
service port every `interval_ms` (1,000 by default), with a `timeout_ms` of 500. All
checks of a round are started at once and waited for with one `poll`, so a round takes
at most `timeout_ms` however many reals there are. UDP VIPs are not checked, because
there is no handshake to test. Their reals are always up and are reported with
`"checked": false`. `--no-health-check` (or `health_check.enabled: false`) turns
checking off for every VIP; that setting takes effect on restart only. A real
goes down after `fall` (3) consecutive failures and comes back after `rise` (2)
consecutive successes. Any transition rebuilds that VIP's ring and swaps it, and logs one
line with a timestamp (`UP->DOWN after 3 failures`, `DOWN->UP after 2 successes`), which
Experiment 3 reads.

The asymmetric thresholds are hysteresis. A single slow probe does not take a real out,
because the ring would change twice in two seconds and every change is a chance to break
flows that are not in a connection table. A real that alternates between passing and
failing never accumulates three failures in a row and stays in. Detection of a real that
is truly dead takes between two and three intervals plus a timeout. With the defaults,
that is about 2.5 to 3.5 seconds.

When a real goes down, established connections to it are not purged from the connection
table. If the real is dead they fail anyway. If the health check was wrong (the service
port is overloaded but established connections are fine), those connections keep
working, and only new connections avoid the real. That asymmetry is deliberate.

New reals start up, with no successful check yet, and leave the ring after `fall`
failures if they are not healthy. After a daemon restart, reals adopted from the pinned
maps also start up. A daemon that started every real down and waited for `rise` successes would
install an empty ring for two seconds and drop every new connection.

The concession is that the health check connects to the real's own address, not through the
tunnel to the VIP. A real whose `tunl0` is down, or whose `rp_filter` is wrong, passes
its health check and black-holes its share of the VIP. Katran ships a separate BPF
program (`healthchecking_ipip`) that sends health checks through the same encapsulation
as real traffic. PacketBalance does not.

## Pinned maps and hitless restart

Every map is pinned under the pin path with `LIBBPF_PIN_BY_NAME`. When the daemon
starts and finds a pinned map with the same type, key size, value size, `max_entries`
and flags, libbpf reuses it instead of creating a new one. The daemon compares those
fields itself before loading, so a mismatch fails with the map and the field named
rather than libbpf's generic "parameter mismatch". The XDP program is
attached with `bpf_xdp_attach` over netlink, which is not tied to the daemon's file
descriptors, so it stays attached after the daemon exits. Stopping the daemon therefore
leaves forwarding running, with the maps, the rings and the connection table intact.
Only health checking, neighbor resolution, the API and metrics stop.

On restart the daemon:

1. Reuses every pinned map.
2. Adopts the existing `real_id` assignments from the pinned `reals` and `neigh` maps.
   This matters because every connection-table entry stores a `real_id`. A daemon that
   renumbered reals on restart would silently send every established flow to a
   different real.
3. Adopts existing `vip_id` assignments from `vip_map`, so rings and counters stay with
   their VIPs.
4. Rebuilds each ring from the configuration and swaps it in. If the configuration and
   eligibility have not changed, the new ring is identical to the old one.
5. Loads the program from its embedded skeleton and attaches it with `bpf_xdp_attach`
   without `XDP_FLAGS_UPDATE_IF_NOEXIST`, which replaces the running program atomically.
   There is no instant with no program attached.

Rolling a new data-plane object is the same operation, since the object is embedded in
the daemon binary. It is hitless as long as the new object's map definitions match the
pinned ones. If they do not (a different conntrack size, or a changed struct in
`abi.h`), the daemon refuses to start and says which map differs. `--recreate-maps`
unpins the mismatched maps and creates new ones. That loses the connection table, and
established flows then depend on the ring alone, which is safe exactly when the ring does
not change at the same time. Switching between native and generic mode is not hitless,
because the kernel will not hold both at once. The daemon logs a warning when it has to
detach first.

`--detach-on-exit` makes SIGTERM detach the program and unpin the maps, for tearing a
host down. It is off by default, because the default should be that a crashed or
restarted daemon does not take traffic down with it.

The risk in the default is the window while the daemon is not running. The rings are
frozen. A real that dies during that window keeps its ring slots, and new connections to
it are black-holed until the daemon comes back. The runbook alerts on the daemon being
down for that reason.

## What PacketBalance does not do

- **Layer 7 anything.** No HTTP, no header or path routing, no TLS termination, no
  retries. It forwards packets. The reals, which would be L7 load balancers in Meta's
  deployment, do all of that.
- **QUIC connection-ID routing.** QUIC is carried as UDP and balanced by 5-tuple, which
  breaks QUIC connection migration (a client whose address changes gets a new 5-tuple
  and a new real). Katran routes QUIC by a server ID embedded in the connection ID.
- **IPv6.** VIPs and reals are IPv4 only. IPv6 packets are passed to the host stack.
- **Fragments and IP options.** Dropped and counted, as Katran does. Non-first fragments
  carry no ports, so they cannot be mapped to a flow without a fragment table (Maglev
  keeps one, section 4 of the paper). The check runs before the VIP lookup, so fragments
  and options addressed to the load balancer host itself are dropped too.
- **VIP announcement.** No BGP, no ECMP. The lab uses a static route that the harness
  flips. Withdrawing a VIP from a load balancer before maintenance is an operator step
  this project cannot do.
- **Multi-host anything.** One VM, namespaces for hosts. The control plane of each load
  balancer is independent and configured by file. There is no config distribution, so
  keeping lb1 and lb2 identical is the operator's job.
- **Health checks through the tunnel**, SYN-flood protection for the connection table,
  UDP entry expiry, ICMP generation, and ICMP echo replies for VIPs (Katran answers
  ICMP echo requests in its XDP program, PacketBalance passes them to the host).
- **Physical NICs.** Nothing has been measured on one.

What Katran does that PacketBalance does not, beyond the list above:

- IPv6 VIPs and reals, IPv4-in-IPv6 and IPv6-in-IPv6 encapsulation, and ICMPv6 "packet
  too big" handling by hash.
- GUE (UDP) encapsulation as an alternative to IPIP.
- QUIC connection-ID routing and TCP server-ID routing from a header option.
- Per-core LRU tuning with separate per-CPU maps, a fallback LRU, and a global LRU option.
- Source-based routing overrides (LPM on the client prefix), destination-port-only and
  no-source-port hashing modes per VIP.
- A new-connection rate limit that protects the LRU from floods.
- Production scale. Katran has been running Meta's L4 layer since before its open-source
  release. PacketBalance has run in one VM.

## Where PacketBalance differs from Katran

For a reader who knows Katran, the places where the similarity stops:

| Topic | Katran | PacketBalance |
|---|---|---|
| Connection table | `ARRAY_OF_MAPS` of plain `LRU_HASH` maps, one per CPU, selected with `bpf_get_smp_processor_id()`, plus a fallback map | One `LRU_PERCPU_HASH`, shared keys with a value per CPU. Costs a value per CPU per entry in memory (see [CAPACITY.md](CAPACITY.md)) and shares one LRU list across CPUs unless `BPF_F_NO_COMMON_LRU` is set |
| Ring storage and update | One flat `ARRAY` for all VIPs, only changed slots batch-written | `ARRAY_OF_MAPS`, a whole new inner map swapped per change |
| Weights | `MaglevHash` (weight counts in the first round only) or `MaglevHashV2` (proportional) | Proportional, w turns per round |
| RST | Not inserted into the LRU | Not inserted into the LRU |
| UDP | 30 s idle expiry | LRU eviction only |
| Encap source | `172.16.0.0/16`, low 16 bits from source port XOR source address | Configurable prefix, default /24, host bits from the flow hash |
| Maximum packet | 1,514-byte frame by default, 3.5 KB maximum | 3,500-byte IP packet |
| Health checks | Encapsulated through the data path (separate BPF program) | TCP connect to the real's own address |

## Sources

- Meta Engineering, "Open-sourcing Katran, a scalable network load balancer", 2018-05-22.
- `facebookincubator/katran`, README and `katran/lib/bpf/` (`balancer.bpf.c`,
  `balancer_maps.h`, `balancer_consts.h`, `encap_helpers.h`, `handle_icmp.h`), and
  `katran/lib/` (`MaglevHash.cpp`, `MaglevHashV2.cpp`, `MaglevBase.cpp`, `KatranLb.cpp`).
  Read once for the comparison table above, after PacketBalance's design was fixed.
- Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load Balancer", NSDI
  2016, sections 3.3, 3.4 and 5.3.
- Høiland-Jørgensen et al., "The eXpress Data Path: Fast Programmable Packet Processing
  in the Operating System Kernel", CoNEXT 2018.
- Linux `net/netfilter/ipvs/ip_vs_mh.c` and `ip_vs_proto_tcp.c`, and
  `Documentation/networking/ipvs-sysctl.rst` (`sloppy_tcp`).
- Linux `Documentation/bpf/map_of_maps.rst` and `Documentation/bpf/map_hash.rst`.
- [READING.md](READING.md) summarizes each and lists the question its authors would ask.
