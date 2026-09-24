# PacketBalance

[![CI](https://github.com/Sanjith-Shan/packetbalance/actions/workflows/ci.yml/badge.svg)](https://github.com/Sanjith-Shan/packetbalance/actions/workflows/ci.yml)

PacketBalance is a Layer 4 load balancer built on XDP and eBPF. An XDP program in C picks a
backend for each packet sent to a virtual IP with Maglev consistent hashing and a per-CPU
LRU connection table, wraps the packet in IP-in-IP, and sends it back out the interface it
came in on. Backends reply to clients directly (direct server return), so replies never
pass through the load balancer. A C++20 daemon loads the program, builds the hash rings,
health-checks the backends and exports Prometheus metrics, and `pbctl` drives it over a
Unix socket. The design is the one Meta published for its Katran load balancer, and
[docs/DESIGN.md](docs/DESIGN.md) credits it, explains every decision, and lists where
PacketBalance differs. It is measured against Linux IPVS, including IPVS's own Maglev
scheduler.

## The lab

Everything runs in one Linux VM, with network namespaces as hosts, built by `lab/up.sh`.

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

  VIP 198.51.100.1, routed by the client via lb1 or lb2 (the harness flips it)
  each real: VIP on lo, tunl0 for IPIP decap, rp_filter off, nginx :80, UDP echo :5000,
             conncheck-server :7000
```

## How it works

- **XDP.** The program runs in the driver's receive path before the kernel allocates an
  skb, and transmits with `XDP_TX` out the same interface ("load balancer on a stick").
  Native mode is tried first, generic mode is the fallback, and every result says which
  ran.
- **Parse and filter.** Ethernet, IPv4, TCP, UDP. Anything that is not a VIP goes to the
  host stack. Fragments and IP options are dropped and counted, as Katran does.
- **Maglev.** Each VIP has a 65,537-slot ring (prime, as in the Maglev paper) filled by
  weighted Maglev hashing and keyed by backend address, so two load balancers with the
  same configuration build the same ring. Removing one of N backends moves about 1/N of
  flows.
- **Connection table.** A per-CPU LRU hash remembers which backend each 5-tuple went to,
  so backend changes do not move established connections. A SYN always goes to the ring.
  A table miss falls through to the ring, which is deterministic.
- **IPIP direct server return.** The packet is wrapped in an outer IPv4 header addressed
  to the backend, with the outer source drawn from a prefix by flow hash so the backend's
  NIC can spread flows across receive queues. The outer checksum is computed in the
  program.
- **Atomic ring updates.** Rings live in a map-in-map. A backend change builds a complete
  new ring and swaps one pointer, so no packet sees a half-written ring.
- **Health checks and hitless restarts.** TCP checks with hysteresis (3 failures down, 2
  successes up) rebuild the ring. Maps are pinned in bpffs and the program stays attached
  when the daemon exits, so a restart or a new build keeps every flow.
- **Operable.** Per-CPU counters summed into Prometheus metrics, drop counters by reason,
  `pbctl flows` to inspect the connection table, `pbctl ring show` for slot shares, a
  [runbook](docs/RUNBOOK.md), and `lab/diagnose.sh` for the "VIP is black-holing"
  checklist.

## Results

### What the numbers say

- **Native XDP lost to IPVS in this VM.** At saturation the reals received 582 kpps
  through PacketBalance in native mode, 1.39 Mpps in generic mode and 2.21 Mpps through
  IPVS `mh`. The packets were lost in the veth driver's rings on both sides of the
  program, not in the program (its drop counters read 0). This is a property of veth in
  a VM and says nothing about native XDP on a physical NIC.
- **Losing a load balancer broke nothing, given consistent hashing.** A failover to a
  second load balancer with an empty connection table broke 0 of 10,000 connections for
  PacketBalance (Maglev and modulo alike) and for IPVS `mh` with `sloppy_tcp=1`, and 87.3%
  for IPVS `rr`. A daemon restart broke 0 of 1,000 connections in all three runs.
- **Removing a backend broke only that backend's connections**, about 2,500 of 10,000,
  on every forwarding plane. A modulo ring broke 73 to 569 more.
- **Adding a backend exposed the per-CPU connection table's cost.** PacketBalance broke
  228 of 10,000 connections (2.3%) where IPVS, with one shared table, broke 0. Without the
  table it broke 24.8%, against the 20% any hash must move.
- **Latency and table-size cost are published but cannot rank anything.** Repeats of the
  wrk runs and of the connection table sizes spread 30 to 70% even with the host
  contention guard.

Every table names the VM, the kernel and the XDP mode, and the packet-rate tables name the
packet size and flow count. All of them come from one Lima VM (Apple Virtualization
framework) on an Apple M3 Pro: 6 vCPUs, 8 GiB, Ubuntu 24.04, kernel 6.8.0-139-generic,
aarch64. The load balancer, the traffic generator and the backends share those vCPUs, and
the load balancer is attached to a veth, not a physical NIC. The numbers are comparable to
each other and to the IPVS baseline on the same VM, and to nothing else, least of all
Katran's production figures. Each row is the mean of three repeats with the range in
parentheses, unless `n` says otherwise. Every packet-rate window and every wrk run passed
a host contention guard (a CPU canary before and after the window). The 15 windows of the
measurement that passed it at the start but failed it at the end are kept in
`results/rejected.jsonl`, and each was retried. [results/README.md](results/README.md)
defines every field. [docs/CAPACITY.md](docs/CAPACITY.md) turns Experiment 1 into a
capacity model.

### Experiment 1: packet rate at saturation

`pktgen` (4 kernel threads) sends 64-byte UDP frames to the VIP, unthrottled, over 10,000
flows for 30 s. The forwarding number is packets **received at the reals**, not the load
balancer's own count, and pps per core divides it by the busy CPU of the whole VM (about
68% for every plane: the four generator threads saturate CPUs 0 to 3), so it is an
estimate and a lower bound.

Native XDP lost to IPVS in this VM. Received rates: PacketBalance native 582 kpps (555 to
602 k), generic 1.39 Mpps, IPVS `mh` (DR) 2.21 Mpps, IPVS `mh` in IPIP tunnel mode (the
like-for-like comparison, same encapsulation) 1.66 Mpps, IPVS `rr` 2.13 Mpps, no load
balancer 5.45 Mpps. The packet accounting of a 30 s native window shows where the rest
went (from the two `standard` rows in `results/exp1_mitigations.jsonl`, which carry the
full accounting): the client offered about 141 M frames, about 100 M were dropped before
the program ran because the LB veth's 256-slot receive ring was full, the program XDP_TX'd
about 40.5 M, about 21 M of those were lost handing them to the bridge-side peer
(`xdp_tx_errors` on the LB veth, `rx_dropped` on the peer), and about 19.5 M arrived; less
than 0.1% of the offered frames is unaccounted for. The program's own drop counters were 0
and its conntrack hit rate was above 99.9%.

Native XDP on a veth runs in a NAPI poll on the CPU of the pktgen thread that sent the
frame, so the generator and the load balancer fight for CPUs 0 to 3 while CPUs 4 and 5 sit
idle. IPVS and generic XDP go through the stack's backlog, which paces the generator and
loses less. On a physical NIC the program runs on the driver's receive ring on the NIC's
IRQ CPUs and XDP_TX goes to the NIC's own transmit ring, so none of this transfers, which
is why the VM number is only a lower bound. The throttled sweep shows the knee: every
plane is lossless up to 1 Mpps offered, and at 2 Mpps offered native delivers 1.64 M,
generic 1.72 M and IPVS `mh` 1.82 M. The mitigation rows show the lab cannot fix this
neutrally: steering the peer's skb work to CPUs 4 and 5 with RPS lifts native to 971 kpps
but lowers IPVS `mh` to 1.55 Mpps, two generator threads make generic and IPVS
generator-bound, and GRO on the peer instead of an XDP_PASS program stops XDP_TX frames
from being delivered at all. PacketBalance's `tx` counter counts XDP_TX verdicts, which is
why the "LB counted" column overstates native by 2.6x.

<!-- results:exp1 -->
64-byte UDP frames (60-byte skb), 10,000 flows, 30 s windows. Host: Apple M3 Pro via Lima vz (6 vCPU, 8 GiB), 6 CPUs, kernel 6.8.0-139-generic, aarch64. Dates: 2026-09-24 to 2026-09-24. Generator: pktgen.

| Configuration | n | Offered pps | LB counted pps | Received at reals pps | VM CPU busy % | Received pps per core (est.) | LB drops / veth XDP_TX errors pps |
|---|---:|---:|---:|---:|---:|---:|---:|
| PacketBalance native | 3 | 5.41 M (5.04 M-5.89 M) | 1.50 M (1.39 M-1.66 M) | 582 k (555 k-602 k) | 68.1 (67.8-68.3) | 142 k (136 k-147 k) | 0 (0-0) / 920 k (844 k-1.07 M) |
| PacketBalance generic | 3 | 2.55 M (2.14 M-2.99 M) | 2.55 M (2.14 M-2.98 M) | 1.39 M (1.35 M-1.43 M) | 67.7 (67.7-67.9) | 343 k (331 k-352 k) | 0 (0-0) / 0 (0-0) |
| IPVS mh (DR) | 3 | 2.23 M (2.14 M-2.28 M) | 2.22 M (2.13 M-2.26 M) | 2.21 M (2.10 M-2.28 M) | 67.7 (67.6-67.7) | 544 k (518 k-561 k) | n/a |
| IPVS rr (DR) | 3 | 2.19 M (2.03 M-2.29 M) | 2.19 M (2.03 M-2.29 M) | 2.13 M (1.93 M-2.25 M) | 67.6 (67.5-67.7) | 524 k (476 k-555 k) | n/a |
| IPVS mh (IPIP tunnel) | 3 | 1.98 M (1.95 M-2.04 M) | 1.98 M (1.93 M-2.03 M) | 1.66 M (1.61 M-1.71 M) | 67.7 (67.6-67.7) | 409 k (396 k-421 k) | n/a |
| no LB (direct to real1) | 3 | 5.45 M (4.91 M-5.79 M) | n/a | 5.45 M (4.91 M-5.79 M) | 67.7 (67.6-67.7) | 1.34 M (1.21 M-1.43 M) | n/a |

Received = frames that arrived at the reals' veth0 (rx_packets), the forwarding number. LB counted = PacketBalance's `tx` counter (XDP_TX verdicts) or IPVS InPkts; for native XDP on a veth it overstates, because a frame XDP_TX'd into a full peer ring is counted and then lost (veth `xdp_tx_errors`, last column). pps per core = received pps / (busy fraction x CPUs), over the whole VM, which also runs the generator and the reals: an estimate and a lower bound; see results/README.md.

#### Offered load sweep (10 s per point, one repeat)

| Configuration | Target pps | Offered pps | Forwarded pps | Received pps | VM CPU busy % |
|---|---:|---:|---:|---:|---:|
| IPVS mh (DR) | 100 k | 100 k | 100 k | 100 k | 67.0 |
| IPVS mh (DR) | 250 k | 250 k | 250 k | 250 k | 67.3 |
| IPVS mh (DR) | 500 k | 499 k | 500 k | 499 k | 67.0 |
| IPVS mh (DR) | 1.00 M | 1000 k | 1.00 M | 1000 k | 67.1 |
| IPVS mh (DR) | 2.00 M | 1.85 M | 1.84 M | 1.82 M | 67.1 |
| IPVS mh (DR) | 4.00 M | 1.95 M | 1.95 M | 1.88 M | 66.9 |
| IPVS mh (DR) | unthrottled | 2.08 M | 2.08 M | 2.05 M | 67.2 |
| IPVS mh (IPIP tunnel) | 100 k | 100 k | 100 k | 100 k | 67.1 |
| IPVS mh (IPIP tunnel) | 250 k | 252 k | 249 k | 253 k | 67.2 |
| IPVS mh (IPIP tunnel) | 500 k | 500 k | 499 k | 500 k | 67.0 |
| IPVS mh (IPIP tunnel) | 1.00 M | 996 k | 999 k | 995 k | 66.9 |
| IPVS mh (IPIP tunnel) | 2.00 M | 1.86 M | 1.79 M | 1.85 M | 67.0 |
| IPVS mh (IPIP tunnel) | 4.00 M | 1.52 M | 1.48 M | 1.44 M | 66.8 |
| IPVS mh (IPIP tunnel) | unthrottled | 1.95 M | 1.99 M | 1.78 M | 67.1 |
| IPVS rr (DR) | 100 k | 100 k | 100 k | 100 k | 67.2 |
| IPVS rr (DR) | 250 k | 250 k | 251 k | 251 k | 67.0 |
| IPVS rr (DR) | 500 k | 501 k | 499 k | 501 k | 67.0 |
| IPVS rr (DR) | 1.00 M | 1000 k | 998 k | 999 k | 67.2 |
| IPVS rr (DR) | 2.00 M | 1.83 M | 1.82 M | 1.78 M | 67.3 |
| IPVS rr (DR) | 4.00 M | 1.98 M | 2.00 M | 1.91 M | 67.5 |
| IPVS rr (DR) | unthrottled | 2.19 M | 2.15 M | 2.13 M | 67.1 |
| PacketBalance generic | 100 k | 101 k | 100 k | 101 k | 66.7 |
| PacketBalance generic | 250 k | 250 k | 250 k | 250 k | 67.1 |
| PacketBalance generic | 500 k | 500 k | 500 k | 500 k | 67.2 |
| PacketBalance generic | 1.00 M | 1000 k | 995 k | 996 k | 67.1 |
| PacketBalance generic | 2.00 M | 1.75 M | 1.74 M | 1.72 M | 67.2 |
| PacketBalance generic | 4.00 M | 2.09 M | 2.09 M | 1.40 M | 67.3 |
| PacketBalance generic | unthrottled | 2.32 M | 2.31 M | 1.63 M | 67.2 |
| PacketBalance native | 100 k | 100 k | 101 k | 100 k | 67.0 |
| PacketBalance native | 250 k | 250 k | 249 k | 249 k | 67.0 |
| PacketBalance native | 500 k | 500 k | 499 k | 500 k | 67.0 |
| PacketBalance native | 1.00 M | 1.00 M | 997 k | 1000 k | 67.0 |
| PacketBalance native | 2.00 M | 1.87 M | 1.71 M | 1.64 M | 67.1 |
| PacketBalance native | 4.00 M | 3.00 M | 1.75 M | 625 k | 67.2 |
| PacketBalance native | unthrottled | 5.82 M | 1.64 M | 601 k | 67.0 |
| no LB (direct to real1) | 100 k | 100 k | n/a | 100 k | 67.1 |
| no LB (direct to real1) | 250 k | 250 k | n/a | 250 k | 67.1 |
| no LB (direct to real1) | 500 k | 500 k | n/a | 500 k | 67.0 |
| no LB (direct to real1) | 1.00 M | 1.00 M | n/a | 1.00 M | 67.0 |
| no LB (direct to real1) | 2.00 M | 2.00 M | n/a | 2.00 M | 66.3 |
| no LB (direct to real1) | 4.00 M | 4.00 M | n/a | 4.00 M | 67.1 |
| no LB (direct to real1) | unthrottled | 5.23 M | n/a | 5.23 M | 67.1 |

#### Where native XDP on veth loses packets, and lab mitigations (30 s windows)

Same load as the table above. `standard` is the published configuration. `rps-pb-lb1-cpus4-5` steers the skb work after pb-lb1 (the LB veth's bridge-side peer) to the two CPUs the generator does not use (`rps_cpus` = 0x30); it changes the path for IPVS too, so IPVS is measured under it as well. `pktgen-2-threads` halves the generator. Lost before program = pb-lb1 `tx_dropped` (lb1's veth receive ring full); lost at XDP_TX = lb1 veth0 `tx_dropped` (peer ring full, `xdp_tx_errors` in native mode).

| Variant | Configuration | n | Offered pps | LB counted pps | Received pps | VM CPU busy % | CPU 4-5 busy % | Received pps per core (est.) | Lost before program pps | Lost at XDP_TX pps |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| standard | PacketBalance native | 2 | 4.61 M (4.59 M-4.63 M) | 1.33 M (1.32 M-1.33 M) | 637 k (629 k-644 k) | 68.1 (68.0-68.1) | 5 (5-5) | 156 k (154 k-158 k) | 3.32 M (3.31 M-3.34 M) | 697 k (691 k-703 k) |
| rps-pb-lb1-cpus4-5 | PacketBalance native | 2 | 4.40 M (4.27 M-4.53 M) | 1.86 M (1.83 M-1.90 M) | 971 k (959 k-983 k) | 86.7 (86.6-86.9) | 57 (56-58) | 187 k (184 k-189 k) | 2.61 M (2.52 M-2.69 M) | 792 k (755 k-829 k) |
| rps-pb-lb1-cpus4-5 | PacketBalance generic | 3 | 2.05 M (1.97 M-2.13 M) | 2.03 M (1.97 M-2.07 M) | 1.75 M (1.68 M-1.84 M) | 97.9 (97.7-98.1) | 94 (93-94) | 297 k (286 k-313 k) | 0 (0-1) | 45 k (44 k-46 k) |
| rps-pb-lb1-cpus4-5 | IPVS mh (DR) | 3 | 1.73 M (1.67 M-1.84 M) | 1.71 M (1.68 M-1.75 M) | 1.55 M (1.47 M-1.66 M) | 93.3 (92.8-93.9) | 81 (80-83) | 276 k (262 k-295 k) | n/a | n/a |
| pktgen-2-threads | PacketBalance native | 2 | 1.86 M (1.77 M-1.96 M) | 763 k (705 k-820 k) | 770 k (719 k-820 k) | 34.8 (34.7-35.0) | 3 (3-4) | 368 k (342 k-394 k) | 1.11 M (1.07 M-1.15 M) | 279 (220-338) |
| pktgen-2-threads | PacketBalance generic | 3 | 947 k (943 k-953 k) | 946 k (942 k-953 k) | 947 k (943 k-953 k) | 34.6 (34.5-34.6) | 3 (3-3) | 456 k (454 k-459 k) | 0 (0-0) | 39 (23-60) |
| pktgen-2-threads | IPVS mh (DR) | 3 | 905 k (815 k-1.00 M) | 896 k (806 k-988 k) | 905 k (815 k-1.00 M) | 34.8 (34.6-35.0) | 3 (3-4) | 434 k (391 k-482 k) | n/a | n/a |
<!-- /results:exp1 -->

### Experiment 2: latency and throughput through the load balancer

`wrk -t4 -c256 -d30s --latency` against nginx on the four backends through each forwarding
plane (PacketBalance in native mode). Median latency through any load balancer was 425 to
509 us. The repeats spread 30 to 70% in requests per second and p50, and far more in p99,
even with the host guard, so the table is published but cannot rank the planes: every load
balancer's range overlaps every other's. The "no LB" row is a different experiment, not a
floor: wrk goes straight to one nginx on real1 instead of four sharing the load, so its
1.36 ms p50 and lower request rate say how one backend saturates, not what a load balancer
adds.

<!-- results:exp2 -->
`wrk -t4 -c256 -d30s --latency` from the client namespace against nginx. Host: Apple M3 Pro via Lima vz (6 vCPU, 8 GiB), 6 CPUs, kernel 6.8.0-139-generic, aarch64. Dates: 2026-09-24 to 2026-09-24.

| Configuration | n | Requests/s | p50 | p99 | Socket errors |
|---|---:|---:|---:|---:|---:|
| PacketBalance native | 3 | 265735 (190544-366794) | 509 us (325 us-674 us) | 20.10 ms (6.12 ms-45.26 ms) | 0 (0-0) |
| PacketBalance generic | 3 | 289964 (237442-394392) | 459 us (300 us-540 us) | 9.06 ms (4.62 ms-13.33 ms) | 0 (0-0) |
| IPVS mh (DR) | 3 | 321572 (227419-391444) | 434 us (319 us-627 us) | 51.98 ms (5.37 ms-144.63 ms) | 0 (0-0) |
| IPVS rr (DR) | 3 | 287040 (237584-341160) | 435 us (349 us-519 us) | 38.66 ms (26.91 ms-49.90 ms) | 0 (0-0) |
| IPVS mh (IPIP tunnel) | 3 | 297313 (264361-361699) | 425 us (336 us-482 us) | 26.48 ms (7.25 ms-64.26 ms) | 0 (0-0) |
| no LB (direct to real1) | 3 | 200846 (183021-209948) | 1.36 ms (1.25 ms-1.47 ms) | 21.65 ms (2.93 ms-59.08 ms) | 0 (0-0) |
<!-- /results:exp2 -->

### Experiment 3: connection survival through backend churn

`conncheck` holds 10,000 long-lived TCP connections through the VIP, each sending a
heartbeat every 100 ms that the backend answers with its id; PacketBalance runs in native
mode. **Remove:** real3 is removed at t=10 s and added back at t=20 s. Every plane except
the modulo ring broke only the connections that were on real3, about 2,500 of 10,000
(2,395 to 2,509 across planes and repeats; at most 5 elsewhere). Without the connection
table PacketBalance broke the same number, because Maglev moves only real3's slots and
gives them back to real3 when it returns. The modulo ring, with the table on, broke 73 to
569 connections on other reals as well: a heartbeat that lands on a CPU whose table has no
entry for its flow (next paragraph) falls back to a ring that moved most flows.

**Add:** real5 joins at t=10 s and leaves at t=20 s, on live traffic to real1..real4. With
its connection table PacketBalance broke 228 connections (20 to 350, 2.3%); without it
2,483 (24.8%, against the 20% minimum a hash alone must move); the modulo ring with the
table 2,547; IPVS `mh` 0. The non-zero default row is the per-CPU LRU trade-off, measured:
a flow's entry exists only on the CPUs that have seen it, a heartbeat that lands on
another CPU falls through to the hash, and while real5 holds a fifth of the ring about a
fifth of those land on real5, which answers with a reset. In this lab the CPU a packet
lands on is whichever CPU the conncheck thread sending it ran on, so cross-CPU misses are
far more common than behind a NIC with RSS, which keeps a flow on one queue. IPVS broke 0
because its connection table is one table shared by all CPUs, paid for with
synchronization between CPUs on the fast path; that is the same trade-off made in the
other direction ([docs/DESIGN.md](docs/DESIGN.md), "What per-CPU misses").

<!-- results:exp3 -->
conncheck holds 10,000 TCP connections (heartbeat 100 ms, timeout 2 s). real3 is removed at t=10 s and added back at t=20 s. Correct: only the connections on real3 break. Host: Apple M3 Pro via Lima vz (6 vCPU, 8 GiB), 6 CPUs, kernel 6.8.0-139-generic, aarch64. Dates: 2026-09-24 to 2026-09-24.

| Configuration | n | Established | On real3 at start | Broken | Broken on real3 | Broken elsewhere | rst / eof / timeout / wrong backend |
|---|---:|---:|---:|---:|---:|---:|---|
| IPVS mh (DR) | 3 | 10000 (10000-10000) | 2455 (2395-2505) | 2455 (2395-2505) | 2455 (2395-2505) | 0 (0-0) | 2442 (2380-2494) / 0 (0-0) / 13 (11-15) / 0 (0-0) |
| IPVS rr (DR) | 3 | 10000 (10000-10000) | 2500 (2500-2500) | 2500 (2500-2500) | 2500 (2500-2500) | 0 (0-0) | 2483 (2479-2485) / 0 (0-0) / 17 (15-21) / 0 (0-0) |
| PacketBalance native | 3 | 10000 (10000-10000) | 2492 (2462-2508) | 2493 (2464-2509) | 2492 (2462-2508) | 1 (0-2) | 2489 (2463-2509) / 0 (0-0) / 4 (0-11) / 0 (0-0) |
| PacketBalance native no conntrack | 3 | 10000 (10000-10000) | 2474 (2448-2491) | 2478 (2450-2496) | 2474 (2448-2491) | 4 (2-5) | 2475 (2450-2489) / 0 (0-0) / 2 (0-7) / 0 (0-0) |
| PacketBalance native modulo | 3 | 10000 (10000-10000) | 2497 (2478-2517) | 2763 (2590-3064) | 2497 (2478-2517) | 267 (73-569) | 2763 (2590-3064) / 0 (0-0) / 0 (0-0) / 0 (0-0) |

#### Adding a backend to live traffic (scenario `add`)

conncheck holds 10,000 connections on real1..real4. real5 is added at t=10 s and removed at t=20 s. Correct: nothing breaks, because the connection table keeps every existing flow on its real; a hash alone moves at least 1/5 of them to real5 (Maglev about 20%, modulo more). Host: Apple M3 Pro via Lima vz (6 vCPU, 8 GiB), 6 CPUs, kernel 6.8.0-139-generic, aarch64. Dates: 2026-09-24 to 2026-09-24.

| Configuration | n | Established | Broken | Broken % | Minimum a hash alone moves % | On real5 at end | rst / eof / timeout / wrong backend |
|---|---:|---:|---:|---:|---:|---:|---|
| PacketBalance native | 3 | 10000 (10000-10000) | 228 (20-350) | 2.3 (0.2-3.5) | 20 | 0 (0-0) | 228 (20-350) / 0 (0-0) / 0 (0-0) / 0 (0-0) |
| PacketBalance native no conntrack | 3 | 10000 (10000-10000) | 2483 (1964-3454) | 24.8 (19.6-34.5) | 20 | 0 (0-0) | 1880 (1646-2030) / 0 (0-0) / 603 (0-1808) / 0 (0-0) |
| PacketBalance native modulo | 3 | 10000 (10000-10000) | 2547 (533-3566) | 25.5 (5.3-35.7) | 20 | 0 (0-0) | 2537 (533-3556) / 0 (0-0) / 10 (0-19) / 0 (0-0) |
| IPVS mh (DR) | 3 | 10000 (10000-10000) | 0 (0-0) | 0.0 (0.0-0.0) | 20 | 0 (0-0) | 0 (0-0) / 0 (0-0) / 0 (0-0) / 0 (0-0) |
<!-- /results:exp3 -->

### Experiment 4: connection survival through load balancer failover

`conncheck` holds 10,000 connections through lb1 and the harness moves the client's route
to lb2, which has the same configuration and an empty connection table. PacketBalance
(native) broke 0.0%, and so did its modulo ring, because both load balancers build the
same ring from the same backends. IPVS `mh` broke 0.0% too, which is the expected result:
it is Maglev in the kernel, and consistent hashing is what makes a stateless tier work,
whoever implements it. IPVS `rr` broke 87.3%. Both IPVS schedulers broke 100% with
`net.ipv4.vs.sloppy_tcp=0`, the kernel default, under which IPVS creates a connection
entry only from a SYN; lb2 never saw the handshakes, so it passed every mid-stream packet
to its own stack, which answered with a reset. The 0.0% and 87.3% IPVS rows use
`sloppy_tcp=1`, which lets IPVS create an entry from a mid-stream packet. **Drift** (real5
configured on lb2 only): Maglev broke 19.8% (19.4 to 20.4) against the 20% minimum, IPVS
`mh` 21.3%, and the modulo ring 80.1%.

<!-- results:exp4 -->
conncheck holds 10,000 TCP connections through lb1; at t=10 s the client's route flips to lb2, which has the same configuration and an empty connection table. Drift: lb2 also has real5. Host: Apple M3 Pro via Lima vz (6 vCPU, 8 GiB), 6 CPUs, kernel 6.8.0-139-generic, aarch64. Dates: 2026-09-24 to 2026-09-24.

| Configuration | Drift | n | Established | Broken | Broken % | Minimum possible % |
|---|---|---:|---:|---:|---:|---:|
| IPVS rr (DR), sloppy_tcp=1 | none | 3 | 10000 (10000-10000) | 8726 (8290-9069) | 87.3 (82.9-90.7) | 0 |
| IPVS mh (DR), sloppy_tcp=1 | none | 3 | 10000 (10000-10000) | 0 (0-0) | 0.0 (0.0-0.0) | 0 |
| IPVS rr (DR), sloppy_tcp=0 | none | 3 | 10000 (10000-10000) | 10000 (10000-10000) | 100.0 (100.0-100.0) | 0 |
| IPVS mh (DR), sloppy_tcp=0 | none | 3 | 10000 (10000-10000) | 10000 (10000-10000) | 100.0 (100.0-100.0) | 0 |
| IPVS mh (DR), sloppy_tcp=1 | real5 on lb2 | 3 | 10000 (10000-10000) | 2130 (2091-2159) | 21.3 (20.9-21.6) | 20 |
| PacketBalance native | none | 3 | 10000 (10000-10000) | 0 (0-0) | 0.0 (0.0-0.0) | 0 |
| PacketBalance native modulo | none | 3 | 10000 (10000-10000) | 0 (0-0) | 0.0 (0.0-0.0) | 0 |
| PacketBalance native | real5 on lb2 | 3 | 10000 (10000-10000) | 1984 (1935-2045) | 19.8 (19.4-20.4) | 20 |
| PacketBalance native modulo | real5 on lb2 | 3 | 10000 (10000-10000) | 8011 (7988-8025) | 80.1 (79.9-80.2) | 20 |
<!-- /results:exp4 -->

### Experiment 5: hash quality

One million synthetic 5-tuples, hashed with the data plane's own flow hash, go through a
Maglev ring and a modulo ring with backend weights 1:1:2:4. Both rings match the weights
to within 0.07 percentage points. Maglev moves the minimum possible share of flows when a
backend is removed (25.09% against 25.00%) or added (11.11% against 11.11%); modulo moves
58% and 69%. This runs in the control plane's test tool and is the same on any machine.

<!-- results:exp5 -->
1000000 synthetic flows through the control plane's rings (size 65537), date 2026-09-24.

| Mode | Real | Weight | Expected share | Observed share |
|---|---|---:|---:|---:|
| maglev | 10.0.0.21 | 1 | 12.50% | 12.52% |
| maglev | 10.0.0.22 | 1 | 12.50% | 12.50% |
| maglev | 10.0.0.23 | 2 | 25.00% | 25.05% |
| maglev | 10.0.0.24 | 4 | 50.00% | 49.93% |
| modulo | 10.0.0.21 | 1 | 12.50% | 12.47% |
| modulo | 10.0.0.22 | 1 | 12.50% | 12.48% |
| modulo | 10.0.0.23 | 2 | 25.00% | 25.01% |
| modulo | 10.0.0.24 | 4 | 50.00% | 50.03% |

| Mode | Event | Minimum possible | Flows moved | Slots moved |
|---|---|---:|---:|---:|
| maglev | remove 10.0.0.23 (w=2) | 25.00% | 25.09% | 25.04% |
| maglev | add 10.0.0.25 (w=1) | 11.11% | 11.11% | 11.17% |
| modulo | remove 10.0.0.23 (w=2) | 25.00% | 58.36% | 58.34% |
| modulo | add 10.0.0.25 (w=1) | 11.11% | 69.41% | 69.43% |
<!-- /results:exp5 -->

### Experiment 6: what the connection table costs

Experiment 1's load (64-byte UDP, 10,000 flows, native XDP) with the connection table off
and at 64K, 1M and 8M entries. Received rates: off 719 kpps, 64K 610 kpps, 1M 453 kpps, 8M
465 kpps. The direction is published: in this VM the table costs measurable forwarding
rate, consistent with Katran's observation that hashing can be cheaper than the lookup.
The sizes are not trustworthy: the large-table repeats spread 30 to 40%, and the 1M row
(453 kpps) disagrees with Experiment 1's identical configuration (582 kpps) because these
windows were host-contended. In generic mode the table made no difference that stands out
from the spread: received rates stayed between 1.07 and 1.26 Mpps at every setting (off,
64K, 1M, 8M), with no XDP_TX errors, because generic mode's cost is dominated by the skb
allocation that native mode avoids, so a hash lookup more or less does not show.

<!-- results:exp6 -->
Experiment 1's load through PacketBalance with the connection table off and at three sizes. Host: Apple M3 Pro via Lima vz (6 vCPU, 8 GiB), 6 CPUs, kernel 6.8.0-139-generic, aarch64. Dates: 2026-09-24 to 2026-09-24. Generator: pktgen.

| XDP mode | Connection table | n | LB counted pps | Received at reals pps | VM CPU busy % | Received pps per core (est.) | LB drops / veth XDP_TX errors pps | Notes |
|---|---|---:|---:|---:|---:|---:|---:|---|
| native | off | 3 | 1.39 M (946 k-2.00 M) | 719 k (685 k-769 k) | 69.3 (68.0-71.8) | 173 k (159 k-188 k) | 0 (0-0) / 679 k (267 k-1.25 M) |  |
| native | 65,536 flows (--conntrack-size) | 3 | 1.26 M (1.04 M-1.59 M) | 610 k (580 k-649 k) | 69.1 (68.2-70.8) | 147 k (136 k-159 k) | 0 (0-0) / 663 k (465 k-951 k) |  |
| native | 1,048,576 flows (--conntrack-size) | 3 | 894 k (669 k-1.11 M) | 453 k (390 k-524 k) | 69.2 (68.0-71.3) | 109 k (95 k-128 k) | 0 (0-0) / 448 k (231 k-589 k) |  |
| native | 8,388,608 flows (--conntrack-size) | 3 | 1.09 M (869 k-1.36 M) | 465 k (365 k-541 k) | 68.1 (67.9-68.3) | 114 k (90 k-133 k) | 0 (0-0) / 635 k (513 k-847 k) |  |
| generic | off | 3 | 1.52 M (1.29 M-1.76 M) | 1.07 M (911 k-1.25 M) | 66.5 (65.2-67.9) | 267 k (233 k-306 k) | 0 (0-0) / 0 (0-0) |  |
| generic | 65,536 flows (--conntrack-size) | 3 | 1.51 M (1.26 M-1.69 M) | 1.13 M (970 k-1.22 M) | 67.9 (66.9-68.4) | 276 k (242 k-298 k) | 0 (0-0) / 0 (0-0) |  |
| generic | 1,048,576 flows (--conntrack-size) | 3 | 1.38 M (1.17 M-1.53 M) | 1.09 M (922 k-1.23 M) | 67.4 (67.0-67.7) | 269 k (229 k-304 k) | 0 (0-0) / 0 (0-0) |  |
| generic | 8,388,608 flows (--conntrack-size) | 3 | 1.68 M (1.37 M-2.08 M) | 1.26 M (1.06 M-1.45 M) | 68.0 (67.8-68.2) | 309 k (261 k-355 k) | 0 (0-0) / 0 (0-0) |  |
<!-- /results:exp6 -->

### What was not done

A third native repeat in each mitigation variant (those rows have n=2). Reruns of the Experiment 2 and 6 rows
with high spread. The XDP program's own run time per packet (`bpf_stats`) and the
connection table's charged memory (`memlock`), which docs/CAPACITY.md describes as methods
only.

## Reproduce

On a Mac with Apple silicon (any Linux host with kernel 6.8 or later works without the VM):

```sh
brew install lima
limactl start --name=packetbalance lab/lima.yaml
limactl shell packetbalance
cd ~/Documents/PacketBalance

cmake -B build -G Ninja && cmake --build build && ctest --test-dir build
sudo lab/up.sh
sudo lab/experiments.sh all
python3 lab/render_tables.py --write
```

`lab/experiments.sh` writes one JSON row per run to `results/`, and `render_tables.py`
fills the tables above from those rows. `sudo lab/down.sh` removes everything the lab
created. `ctest` needs root for the data-plane suite, which loads the XDP program and
pushes crafted frames through it with `BPF_PROG_TEST_RUN` (no interface, no namespaces),
and for the daemon suite.

The same thing runs in CI on every push: the `linux-full` job builds and runs every test
on a stock x86-64 GitHub runner, and the `e2e` job brings up the namespaced lab there,
attaches the XDP program, and curls the VIP through PacketBalance and through IPVS. A
fresh x86-64 cloud VM reproduces the full experiment suite with `lab/cloud-run.sh`, see
[docs/DEVELOPING.md](docs/DEVELOPING.md).

## Layout

| Path | Contents |
|---|---|
| `bpf/` | The XDP program |
| `include/packetbalance/` | The ABI shared by both planes, the flow hash, Maglev |
| `src/core/` | Maglev and modulo rings, config parsing |
| `src/daemon/`, `src/pbctl/` | The daemon and the CLI |
| `tools/` | `conncheck`, `hashquality`, bpftrace scripts |
| `tests/` | Google Test for the core, `BPF_PROG_TEST_RUN` tests for the data plane |
| `lab/` | The VM, the namespaces, the IPVS baseline and the experiments |
| `deploy/` | systemd unit and example config |
| `results/` | Measured rows, committed |
| [docs/DESIGN.md](docs/DESIGN.md) | How it works and why, packet walk, what it does not do |
| [docs/RUNBOOK.md](docs/RUNBOOK.md) | Operating procedures and diagnosis checklists |
| [docs/CAPACITY.md](docs/CAPACITY.md) | The capacity model |
| [docs/API.md](docs/API.md) | Control socket protocol, `pbctl`, metrics |
| [docs/READING.md](docs/READING.md) | The papers and posts it was designed from |
| [docs/DEVELOPING.md](docs/DEVELOPING.md) | Building and conventions |

## What it does not do

No Layer 7, no TLS, no QUIC connection-ID routing, no IPv6, no fragments or IP options, no
BGP or VIP announcement, no health checks through the tunnel, and no multi-host control
plane. It has run in one VM and never on a physical NIC or production traffic.
[DESIGN.md](docs/DESIGN.md#what-packetbalance-does-not-do) has the full list and what
Katran does beyond it.

## Credits

- The design is Katran's, as described in Meta Engineering's "Open-sourcing Katran, a
  scalable network load balancer" (2018) and the `facebookincubator/katran` repository.
- The consistent hashing is from Eisenbud et al., "Maglev: A Fast and Reliable Software
  Network Load Balancer", NSDI 2016.
- The data path is XDP, from Høiland-Jørgensen et al., "The eXpress Data Path", CoNEXT
  2018.

## License

MIT. See [LICENSE](LICENSE). The XDP program is dual-licensed GPL-2.0-only or MIT, and
declares a GPL license to the kernel.
