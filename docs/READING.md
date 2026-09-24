# Reading list

The seven sources PacketBalance was designed from. For each, what it is, the points from
it that shaped the design, and the question its authors would most likely ask about
PacketBalance. Summaries are from the sources themselves. Numbers quoted are theirs, on
their hardware.

## 1. Meta Engineering, "Open-sourcing Katran, a scalable network load balancer" (2018)

https://engineering.fb.com/2018/05/22/open-source/open-sourcing-katran-a-scalable-network-load-balancer/

**What it is.** The post that released Katran. It describes Meta's first-generation L4
load balancer (IPVS for forwarding, ExaBGP to announce VIPs, consistent hashing, DSR with
IP-in-IP) and why it was replaced with an XDP data plane.

**What shaped PacketBalance.**

- The first generation could not share a host with backends, which forced dedicated load
  balancer machines. XDP fixed that because it "allows us to run Katran alongside any
  application without any performance penalties on the same host". This is the argument
  for XDP over kernel bypass in DESIGN.md.
- The finding that "computing the hash is computationally easier than looking up the
  local state table", and the resulting design of the connection table as an LRU cache
  whose size is "a tunable parameter to strike a balance between computation and lookup".
  Experiment 6 exists to test it.
- An extended Maglev hash with unequal weights, so a hardware refresh can be absorbed "by
  simply setting appropriate weights". PacketBalance's weighted fill.
- Lockless, per-CPU maps, so "performance scales linearly with the number of the NIC's
  RX queues". The per-CPU connection table.
- RSS-friendly encapsulation. Different flows get different outer source addresses, and
  "packets in the same flow are always assigned the same outer source IP". The
  `encap_src_prefix` scheme.
- Consistency across load balancers without state sharing, because the hash depends only
  on the 5-tuple. The whole of Experiment 4.

**The question its authors would ask.** "Katran's lookup table is an optimization, not a
requirement. What exactly breaks in your system if I set the table size to zero, and did
you measure whether the table even pays for itself?"

## 2. `facebookincubator/katran`, README and source

https://github.com/facebookincubator/katran

**What it is.** The open-source library and BPF program. The README lists requirements
and features. The BPF source is in `katran/lib/bpf/`, the Maglev implementations in
`katran/lib/`.

**What shaped PacketBalance.**

- The environment requirements, which PacketBalance adopts as its own limits. DSR only,
  an L3 topology where every encapsulated packet goes to the first router, no
  fragmentation support, no IP options, a maximum packet size of 3.5 KB (1.5 KB by
  default), and the "load balancer on a stick" model with one interface for both
  directions.
- The MTU advice. Katran cannot fragment, and recommends raising the network MTU and
  also advertising a smaller TCP MSS (1450 instead of 1460) to help clients behind PPPoE.
  The lab's `mtu 1480` route is the same fix.
- The failure scenario, which the README states directly. Because only packet headers
  feed the hash, "different L4 lbs are consistent in real server selection, even w/o
  explicit state sharing".
- From the source, read after PacketBalance's design was fixed and used only for the
  comparison in DESIGN.md. The connection table is an `ARRAY_OF_MAPS` of per-CPU
  `LRU_HASH` maps with a fallback map, not an `LRU_PERCPU_HASH`. Rings are one flat array
  updated slot by slot. RST packets never create LRU entries. UDP entries expire after
  30 s. A new-connection rate limit (`MAX_CONN_RATE`) stops LRU inserts under a flood.
  ICMP "fragmentation needed" is routed by the quoted inner header.
- Two Maglev versions. `MaglevHash` applies weights only in the first round of the fill.
  `MaglevHashV2` accumulates weight per round and gives proportional shares.

**The question its authors would ask.** "You used one `LRU_PERCPU_HASH` where Katran uses
one `LRU_HASH` per CPU in a map-in-map. What does your choice cost in memory on a 64-core
host, and what does a lookup from the wrong CPU return?"

## 3. Eisenbud et al., "Maglev: A Fast and Reliable Software Network Load Balancer" (NSDI 2016)

https://www.usenix.org/system/files/conference/nsdi16/nsdi16-paper-eisenbud.pdf

**What it is.** Google's software L4 load balancer, a kernel-bypass userspace forwarder
behind ECMP routers, encapsulating in GRE to backends that reply directly. Section 3.4 is
the consistent hashing algorithm that everyone now calls Maglev hashing.

**What shaped PacketBalance.**

- The two-part backend selection in section 3.3. Pick with consistent hashing, then
  "record the selection in a local connection tracking table". The table handles backend
  changes. Consistent hashing handles the case the table cannot, a packet arriving at a
  different Maglev because the router's ECMP changed.
- One connection table per packet thread, "to avoid access contention". The same
  reasoning as per-CPU maps.
- The algorithm itself. Offset and skip from two hashes of the backend's name, a
  permutation `(offset + j × skip) mod M`, and a round-robin fill of each backend's most
  preferred empty slot. "M must be a prime number so that all values of skip are
  relatively prime to it." Each backend gets floor(M/N) or ceil(M/N) entries, and M
  should be above 100 × N for under 1% imbalance.
- The choice of balance over minimal disruption. With 1,000 backends and a 65,537-entry
  table, Karger and Rendezvous hashing needed 29.7% and 49.5% overprovisioning. Maglev
  accepts slightly more disruption because "we still have connection tracking as the
  primary means of protection".
- The default size of 65,537 and its cost. Table generation took 1.8 ms at 65,537 and
  22.9 ms at 655,373. Weights are done "by altering the relative frequency of the
  backends' turns", details not given.
- Line-rate arithmetic. 10 Gb/s is 813 kpps at 1,500-byte packets and 9.06 Mpps at 100
  bytes. CAPACITY.md reproduces both.

**The question its authors would ask.** "Maglev gives up some disruption to get balance.
Show me the case where your conntrack does not cover the extra disruption, and tell me
how many connections it costs."

## 4. Høiland-Jørgensen et al., "The eXpress Data Path" (CoNEXT 2018)

https://github.com/xdp-project/xdp-paper

**What it is.** The design paper for XDP, by the people who built it, with measurements
against DPDK and the Linux stack and three example applications, one of which is
Katran.

**What shaped PacketBalance.**

- Where it runs. "At the earliest possible moment after a packet is received from the
  hardware, before the kernel allocates its per-packet sk_buff data structure." There are
  four verdicts, drop, transmit out the same interface, pass to the stack, and redirect.
- Cost numbers for the stack. 24 Mpps on one core for XDP drop against 4.8 Mpps for the
  stack's raw-table drop, and 13.3 ns of overhead for an XDP program that only counts and
  passes.
- `XDP_TX` is cheaper than `XDP_REDIRECT` to another NIC, because buffers belong to the
  receiving interface and must be returned to it when forwarded elsewhere. That is part of
  why "load balancer on a stick" is the fast configuration.
- The load balancer benchmark (Table 2). Katran's XDP program against IPVS configured the
  same way, both scaling linearly with cores, XDP about 4.3 times faster.
- The verifier. It proves termination and memory safety. At publication it did this by
  rejecting loops outright (bounded loops came later), and it requires the program to
  bounds-check every packet access against `data_end`. Every `if ((void *)(x + 1) >
  data_end)` in `bpf/packetbalance.bpf.c` is there for it.
- Maps in global and per-CPU variants, and generic XDP as the fallback that runs "at
  reduced performance" when a driver lacks support.

**The question its authors would ask.** "You measured on a veth. Which parts of the XDP
fast path does a veth actually exercise, and which of your numbers would change on a
driver with native support?"

## 5. Linux `pktgen` documentation and the IPVS `mh` scheduler

https://docs.kernel.org/networking/pktgen.html,
`net/netfilter/ipvs/ip_vs_mh.c`

**What it is.** `pktgen` is the kernel's packet generator, used for Experiment 1. `mh`
is the Maglev-hashing scheduler inside IPVS, the baseline that makes the comparison fair.

**What shaped PacketBalance.**

- pktgen creates one kernel thread per CPU and is driven through `/proc/net/pktgen`.
  Devices are added to threads, and `device@N` lets one device be driven from several
  threads for multi-queue tests.
- Its knobs change what is measured. `clone_skb` and `burst` reuse one skb for many
  transmissions, `flows` and `flowlen` and the `UDPSRC_RND` flag vary the 5-tuple, and
  `pkt_size` sets the frame size without the FCS (60 bytes is a 64-byte frame). The
  default NIC settings are "not tuned for pktgen's artificial overload type of
  benchmarking".
- IPVS `mh` is Maglev hashing in the kernel since 4.18, with a prime table size chosen
  from a fixed list (4,093 by default, `CONFIG_IP_VS_MH_TAB_INDEX`), hsiphash with fixed
  keys, and weights as turns per round scaled by their greatest common divisor.
- By default `mh` hashes **only the source address**. The port is included only with the
  `mh-port` scheduler flag. Every lab connection comes from one client address, so
  without `-b mh-port` IPVS would send all of them to one real. The lab always sets it.
- The fill follows the order destinations were added, so two IPVS instances agree only if
  their reals were added in the same order. PacketBalance sorts by address.
- IPVS schedules a TCP connection only on a SYN unless `net.ipv4.vs.sloppy_tcp=1`. A
  failover target without it drops every established connection it receives. The lab sets
  it for both schedulers.

**The question its authors would ask.** "IPVS `mh` gives the same failover survival as
your XDP program. So apart from packets per second, what does PacketBalance give an
operator that `ipvsadm -s mh -b mh-port` does not?"

## 6. libbpf documentation, and the kernel's BPF map documentation

https://libbpf.readthedocs.io, `Documentation/bpf/map_of_maps.rst`,
`Documentation/bpf/map_hash.rst`

**What it is.** libbpf is the C library that loads BPF objects. The kernel documents the
semantics of each map type.

**What shaped PacketBalance.**

- The object lifecycle, open, load, attach, tear down, with a window after open to adjust
  maps before they are created. The daemon resizes `conntrack` in that window.
- Skeletons from `bpftool gen skeleton`, which embed the object in the daemon and give
  typed access to maps and programs. That is why rolling the data plane means rolling
  the daemon binary.
- CO-RE and BTF, so one compiled object runs across kernel versions without recompiling
  on the target.
- Map-in-map. An outer `ARRAY_OF_MAPS` holds inner maps of one type. Only user space can
  update the outer map. A program can only look an inner map up. That asymmetry is exactly
  what makes the atomic ring swap safe.
- `LRU_PERCPU_HASH` keeps a value per CPU, but its LRU list is shared across CPUs unless
  the map is created with `BPF_F_NO_COMMON_LRU`, and when an update needs space the
  kernel may steal free nodes from other CPUs' lists. Per-CPU values do not make eviction
  per-CPU.
- Pinning by name, so a map outlives the process that created it and a new process can
  reuse it if type, sizes and `max_entries` match.

**The question its authors would ask.** "What happens when you change `max_entries` of a
pinned map and restart, and how would you migrate the old table's contents instead of
dropping it?"

## 7. Cloudflare, "How to receive a million packets per second" and "Kernel bypass" (2015)

https://blog.cloudflare.com/how-to-receive-a-million-packets/,
https://blog.cloudflare.com/kernel-bypass/

**What it is.** Two posts about measuring and achieving high packet rates on Linux,
written from practice rather than theory.

**What shaped PacketBalance.**

- A naive single-threaded UDP receiver managed 197k to 370k pps. Getting past 1 Mpps
  took multiple receive queues, `SO_REUSEPORT` and NUMA-aware placement, reaching 1.4 Mpps
  under ideal conditions. Cross-NUMA placement cost up to 4 times.
- The test NIC's RSS hashed only on IP addresses, not ports, so packets from one sender
  to one address all landed on one queue. A benchmark with too few distinct flows
  measures one core, not the system. That is why pktgen runs with 10,000 flows and why
  the encap source trick matters on the real.
- The posts measure at more than one layer (the application, and the NIC's own counters
  through `ethtool`). PacketBalance does the same. It counts forwarded packets at the load
  balancer's `tx` counter and received packets at the reals' interfaces, and reports
  both, so a gap between them is visible.
- The stack's ceiling. "Vanilla Linux can do only about 1M pps." An iptables raw-table
  DROP reached 1.4 Mpps on one core, and fell to 480 kpps per core when spread over four
  queues. A 10 Gb/s NIC delivered 12 Mpps in the same test.
- Kernel bypass (PF_RING, Snabb, DPDK, netmap) takes over the whole NIC, which Cloudflare
  could not accept. XDP, which arrived after these posts, is the partial bypass they were
  asking for.

**The question its authors would ask.** "Your generator, your load balancer and your
receivers share one VM's CPUs. How do you know Experiment 1 measured the load balancer
and not pktgen?"
