# PacketBalance capacity model

How many load balancer hosts a given traffic target needs, derived from the per-core
packet rate measured in Experiment 1. The number this produces is less important than
the method. The measured inputs come from a VM, and the model says exactly where that
makes them wrong.

The measured values below are filled in from `results/` by `lab/fill_numbers.py --fill
--basis received` (the template is `docs/templates/CAPACITY.md`). Everything else is
either arithmetic or a stated assumption.

## Inputs

| Symbol | Input | Value | Source |
|---|---|---|---|
| `P` | Packets per second per core that arrived at the reals, PacketBalance native XDP, 64-byte UDP, 10,000 flows, lab VM on veth (mean of 3) | `142 kpps` | Experiment 1 |
| `P_gen` | The same, generic XDP | `343 kpps` | Experiment 1 |
| `P_ipvs` | The same, IPVS `mh` in DR mode | `544 kpps` | Experiment 1 |
| `t_prog` | XDP program run time per packet, from `bpftool prog show` with `kernel.bpf_stats_enabled=1` | not measured | Method only, see "Why the VM number is a lower bound" |
| `T` | Target peak packet rate across the tier | 10 Mpps (the worked example) | Requirement |
| `U` | Target utilization at peak, for CPU and for the link | 0.6 | Policy |
| `S` | Packet size distribution at the load balancer | 64-byte frames, then a mix (below) | Assumption |
| `L` | NIC line rate per host | 25 Gb/s | Assumption, typical current server NIC |
| `c` | Physical cores per host | 16 | Assumption |
| `r` | Cores reserved for the daemon (health checks, API, metrics), the OS and interrupts not tied to RX queues | 1 | Assumption |
| `q` | NIC receive queues bound to cores | at least `c - r` | Assumption |
| Redundancy | Hosts beyond what peak needs | N+2 | Policy |

`P` counts packets received at the reals, not the program's `tx` counter. On the lab's
veth, native XDP_TX verdicts outnumber delivered frames by about 2.6 to 1 because the
veth driver drops frames the program has already transmitted (README, Experiment 1). A
capacity model must be built on what arrives. Each `P` charges the whole VM's busy CPU
(generator, bridge and reals included) to the load balancer, so it is an estimate and a
lower bound. In this VM native XDP is the slowest of the three, for reasons specific to
veth (below).

**Why 60%.** Headroom for traffic spikes inside the measurement interval, for the extra
load when a peer fails and ECMP moves its share (the N+2 covers whole-host loss, the 60%
covers the minutes before anyone reacts), and for the cost that grows with the connection
table working set, which a 10,000-flow benchmark does not show.

**Why N+2.** One host can be out for planned work (a kernel upgrade, a new BPF object,
draining for a NIC swap) while a second fails unplanned, and the tier still carries peak.
Because the tier is stateless (DESIGN.md, "When the load balancer dies"), losing a host
moves its flows without breaking them, so N+2 is a capacity requirement, not a
connection-preservation one.

## The formula

A host's forwarding capacity is the smaller of what its cores can process and what its
link can carry.

```
cores_fwd = min(c - r, q)
H_cpu     = cores_fwd * P * U                        packets per second, CPU bound
H_link    = L * U / (8 * W_tx)                       packets per second, link bound
H         = min(H_cpu, H_link)
hosts     = ceil(T / H) + 2
```

`W_tx` is the average number of bytes a packet occupies on the wire in the transmit
direction. The load balancer receives each packet and transmits it back out the same
NIC. The link is full duplex, so receive and transmit are separate budgets, and transmit
is the larger one because every packet leaves 20 bytes bigger (the outer IPv4 header).

```
W_rx = frame + 20            preamble (8) and inter-frame gap (12)
W_tx = frame + 20 + 20       plus the outer IPv4 header
```

where `frame` includes the Ethernet header and FCS. A 64-byte frame occupies 84 bytes on
the wire arriving and 104 leaving.

`P` is assumed not to depend on packet size. An XDP program reads headers and prepends
20 bytes. It does not touch the payload and does not copy it, so its per-packet cost is
roughly constant with size. That is an assumption, and Experiment 1 at 64 bytes does not
test it. A run at 1,500 bytes would.

## Worked example: 10 Mpps at 60% with N+2

With the assumed host (16 cores, 1 reserved, 25 Gb/s) and 64-byte frames:

```
cores_fwd = 15
H_cpu     = 15 * P * 0.6                    = 9 * P
H_link    = 25e9 * 0.6 / (8 * 104)          = 18.03 Mpps
H         = min(9 * P, 18.03 Mpps)
hosts     = ceil(10 Mpps / min(9 * P, 18.03 Mpps)) + 2
```

The host is CPU bound whenever `P` is below 18.03 / 9 = 2.0 Mpps per core, and link
bound above it.

With the lab's measured per-core rates (received basis, means of three windows, rounded
to the nearest thousand; the filled table above has the current values):

```
native  P      = 142 kpps   H = 9 * 0.142 = 1.28 Mpps   hosts = ceil(10 / 1.28) + 2 = 8 + 2 = 10
generic P_gen  = 343 kpps   H = 9 * 0.343 = 3.09 Mpps   hosts = ceil(10 / 3.09) + 2 = 4 + 2 = 6
IPVS mh P_ipvs = 544 kpps   H = 9 * 0.544 = 4.90 Mpps   hosts = ceil(10 / 4.90) + 2 = 3 + 2 = 5
```

Read literally, the VM says native XDP needs twice the hosts IPVS does. That is not a
prediction for a physical tier, and the reason is the point of this document. In the VM,
native XDP on veth runs in a NAPI poll on the generator's own CPU and loses most of its
packets in the veth driver's 256-slot rings before and after the program (README,
Experiment 1, packet accounting), while two of the six vCPUs stay idle. Behind a
multi-queue NIC the program runs on the driver's receive ring on the IRQ CPUs, and
XDP_TX goes to the NIC's own transmit ring. The veth losses have no equivalent there. So
the method is what carries over: measure `P` on the target hardware, as packets that
arrived, then apply the formula. The lab numbers show the arithmetic working, and they
show that a per-core figure taken from a program's own counters, or from a VM, can be
wrong by a factor that decides the hardware bill.

To show how the answer responds to `P`, here is the arithmetic for a range of inputs.
**These are inputs to the formula, not measurements of PacketBalance.**

| `P` (Mpps per core) | `H_cpu` | `H_link` (64 B) | `H` | Hosts for peak | With N+2 |
|---:|---:|---:|---:|---:|---:|
| 0.25 | 2.25 Mpps | 18.03 Mpps | 2.25 Mpps | 5 | 7 |
| 0.5 | 4.5 Mpps | 18.03 Mpps | 4.5 Mpps | 3 | 5 |
| 1.0 | 9.0 Mpps | 18.03 Mpps | 9.0 Mpps | 2 | 4 |
| 2.0 | 18.0 Mpps | 18.03 Mpps | 18.0 Mpps | 1 | 3 |
| 3.0 | 27.0 Mpps | 18.03 Mpps | 18.03 Mpps | 1 | 3 |

Two things fall out. Above 2 Mpps per core, a faster data plane buys nothing on this
host at 64 bytes, because the NIC is the limit. And at the low end, a data plane twice as
slow costs hosts roughly in proportion, which is where the gap between XDP and IPVS
becomes a hardware bill.

## Sensitivity to packet size

The formula has two bounds and packet size moves only one of them. `H_cpu` is in packets
and, under the constant-cost assumption, does not change. `H_link` is in bytes and
falls as packets grow.

| Frame (bytes, incl. FCS) | `W_tx` | 10 Gb/s at 100% | 25 Gb/s at 100% | 25 Gb/s at 60% |
|---:|---:|---:|---:|---:|
| 64 | 104 | 12.02 Mpps | 30.05 Mpps | 18.03 Mpps |
| 512 | 552 | 2.26 Mpps | 5.66 Mpps | 3.40 Mpps |
| 1,518 (1,500-byte IP) | 1,558 | 0.80 Mpps | 2.01 Mpps | 1.20 Mpps |
| Mix, 50% 64 / 30% 512 / 20% 1,518 by count | 529.2 | 2.36 Mpps | 5.91 Mpps | 3.54 Mpps |

For the mix, the average frame is 489.2 bytes and the average transmit wire size is
0.5 × 104 + 0.3 × 552 + 0.2 × 1,558 = 529.2 bytes. At 25 Gb/s and 60%, the link carries
3.54 Mpps, so the host is link bound whenever `P` exceeds 3.54 / 9 = 0.39 Mpps per core.
For 10 Mpps of that mix the tier needs ceil(10 / 3.54) + 2 = 3 + 2 = 5 hosts, and the
per-core rate stops mattering. A mix like that should be planned in bytes per second,
not packets per second.

The 1,518-byte row assumes the MTU between the load balancers and the reals has been
raised to fit 1,520-byte packets. With a 1,500-byte path the largest client packet is
1,480 bytes (DESIGN.md, "MTU and ICMP").

The same arithmetic reproduces the Maglev paper's line-rate figures, which is a useful
check on the method. A 1,500-byte IP packet occupies 1,538 bytes on the wire, and
10 Gb/s / (8 × 1,538) = 813 kpps. A 100-byte IP packet occupies 138, and
10 Gb/s / (8 × 138) = 9.06 Mpps. The paper gives both numbers.

Which mix is realistic depends on direction. With DSR the load balancer carries only
client-to-server packets. For a service that mostly serves content, that direction is
small requests and a stream of ACKs, and 64 to 100 bytes is close to the truth. That is
why the CPU bound usually matters for an L4 load balancer and why Experiment 1 uses
64-byte packets. Upload-heavy services (media upload, backups) invert this, and for them
the link bound dominates.

## Sensitivity to connection table size

The connection table is a `BPF_MAP_TYPE_LRU_PERCPU_HASH`. It is preallocated in full at
load time, whether or not there is traffic, and it cannot be resized without recreating
it. Its memory, from the kernel's hash table layout on 6.8:

```
per entry, shared:   htab_elem header 48  + key 16 (sizeof(pb_ct_key)) + per-CPU pointer 8 = 72 bytes
per entry, per CPU:  value 16 (sizeof(pb_ct_value), rounded up to 8)                       = 16 bytes
per bucket:          list head + lock                                                      = 16 bytes
buckets:             entries rounded up to a power of two

memory(E, C) ~= E * 72 + E * 16 * C + roundup_pow2(E) * 16
```

`C` is the number of possible CPUs (`/sys/devices/system/cpu/possible`), not the number
online. `max_entries` for this map type is the total number of keys, shared by all CPUs,
not a per-CPU count.

| Entries `E` | `C` = 6 (the lab VM) | `C` = 16 | `C` = 32 | `C` = 64 |
|---:|---:|---:|---:|---:|
| 1,048,576 (1M, default) | 184 MiB | 344 MiB | 600 MiB | 1,112 MiB |
| 8,388,608 (8M) | 1,472 MiB | 2,752 MiB | 4,800 MiB | 8,896 MiB |

These are derived from the struct layout. The check is the kernel's own accounting,
`bpftool map show pinned <pin path>/conntrack`, whose `memlock` field is the charged
size. That check was not run: no result row records `memlock`, so the table above is
arithmetic, not measurement.

Two things follow. First, the per-CPU value term dominates on large hosts, and most of it
is wasted. Each flow is normally processed on one CPU (RSS), so of the `C` value copies
per entry, one is used. Katran's layout, one ordinary `LRU_HASH` per CPU selected by CPU
number, stores one value per entry and would hold the same number of flows in about
E × 80 bytes plus buckets, independent of `C`. That is a design cost PacketBalance
accepts for having a single map, and on a 64-core host at 8M entries it is several GiB.

Second, how many entries are needed is a Little's law question.

```
E >= new flows per second * seconds a flow must stay resident
```

A flow must stay resident for as long as a ring change could arrive during its life and
it would matter. For TCP that is the connection's lifetime plus the idle time until its
last packet. An undersized table does not fail loudly. Evicted flows fall back to the
ring and survive unless the ring changes, so the failure appears only as extra broken
connections during backend churn. Experiment 6 measures the other side of the trade,
whether a larger table costs forwarding rate (Katran found hashing can be cheaper than
the lookup). Per-core rates (received basis, native XDP only) at 64K, 1M and 8M entries
and with conntrack off are `147 kpps`, `109 kpps`,
`114 kpps` and `173 kpps`. The direction is
consistent with Katran's observation (the table costs measurable rate), but the repeats
spread 30 to 40% at the large sizes and the 1M row disagrees with Experiment 1's identical
configuration, so the sizes are not ranked against each other here. In generic mode the
table size made no visible difference (1.07 to 1.26 Mpps received at every setting),
because the skb allocation dominates that path.

The rings are small by comparison. Each VIP's ring is 65,537 × 4 bytes = 256 KiB, so 64
VIPs are 16 MiB.

## Why the VM number is a lower bound, and where it is not

Experiment 1 runs in one VM on an M3 Pro (Lima, Apple Virtualization framework, aarch64),
with the traffic generator, both load balancers and the reals all in network namespaces
on the same vCPUs, and the load balancer attached to a veth. A physical host running
PacketBalance in native mode on a real NIC avoids several costs the lab pays.

- **No skb at all.** On a veth, the frame starts as an skb built by the sender's stack
  (pktgen, in Experiment 1), and the veth converts it to an XDP buffer, copying when the
  skb is shared or short of headroom. A NIC driver in native mode runs the program on the
  DMA buffer before any skb exists. Generic mode, in the lab and anywhere else, always
  pays for the skb.
- **No hypervisor.** vCPUs are threads on the host, subject to scheduling and exits,
  and the VM has no dedicated cores.
- **RSS across real queues.** A multi-queue NIC hashes flows to queues in hardware, each
  served by its own core with no contention. In the VM, where the program runs is decided
  by where the sending side schedules the veth's NAPI, which is the generator's own CPU:
  in Experiment 1, CPUs 4 and 5 stayed idle while the generator's CPUs saturated.
- **No veth rings.** Native XDP on veth has a 256-slot ring in front of the program and
  another between XDP_TX and the peer. In a 30 s window at saturation in the lab, about
  100 M of 141 M offered frames were dropped before the program ran and about 21 M of its
  40.5 M XDP_TX verdicts were dropped handing them to the peer (`xdp_tx_errors`). That is why native XDP delivered
  less than IPVS in this VM (582 kpps against 2.21 Mpps), and it is a property of the
  veth driver, not of XDP on a NIC.
- **No generator on the same cores.** pktgen and the load balancer compete for the same
  six vCPUs. Every cycle pktgen spends is a cycle the load balancer cannot. Attributing
  CPU time to the load balancer rather than the generator is the hardest part of the
  measurement, and each result row documents how it was done.
- **Encapsulation offloads.** Some NICs parse IPIP for RSS and checksum on the receive
  side of the real, which moves work off the reals. This does not change the load
  balancer's number but changes how many reals a load balancer can feed without hurting
  them.

Where the VM number could overstate a real host:

- **No DMA and no PCIe.** On a physical host, packet data arrives by DMA and the first
  touch of the headers can miss in cache. On a veth, the data was just written by another
  core and is often still in a shared cache.
- **A small working set.** 10,000 flows fit in cache along with their connection table
  entries. Millions of concurrent flows will not, and each table lookup can cost a cache
  miss. Experiment 6 probes this only partly, because the table size changes but the flow
  count does not.
- **Uniform synthetic traffic.** pktgen varies source ports evenly. Real traffic has heavy
  hitters, SYN floods, fragments and junk, each with its own path through the program.

So the lab number is a lower bound on the data plane's own cost per packet and a floor
for the forwarding rate of the program, not a prediction of a production host.
The way to bracket the measurement from the other side is `t_prog`, the program's own
run time per packet (`kernel.bpf_stats_enabled=1`, then `run_time_ns / run_cnt` from
`bpftool prog show`), which gives a program-only ceiling of `1e9 / t_prog` packets per
second per core. It was not measured in this lab, so no such ceiling is quoted. No number
here is compared with Katran's published production figures, and none should be.

## Assumptions, stated once

1. `P` is measured at 64-byte UDP with 10,000 flows and applies to TCP traffic of the
   same size. The program does the same work for both, except for connection table
   inserts on SYN, which a SYN-heavy workload would pay more often.
2. Per-packet cost does not depend on packet size (untested above 64 bytes).
3. Throughput scales linearly with cores up to the number of RX queues. The XDP paper
   and Katran's README both report linear scaling. The lab has not verified it on more
   than six vCPUs.
4. One reserved core covers the daemon, health checks for every real of every VIP, the
   metrics endpoint and housekeeping. With thousands of reals the health checker needs
   its own sizing.
5. ECMP in front of the tier spreads flows evenly across load balancers. Uneven ECMP
   hashing needs extra headroom, which the 60% target partly covers.
6. The link bound uses the transmit direction and ignores the host's own traffic
   (health checks, metrics, management), which is small.
7. The NIC can transmit `XDP_TX` at line rate and has at least one TX queue per core.
8. Connection table memory is available in addition to whatever else runs on the host.
9. The VM measurement is a lower bound on the program's performance, with the exceptions
   listed in the previous section.
