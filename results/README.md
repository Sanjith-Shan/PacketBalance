# results/

Every number in the README comes from a file in this directory. Files are
JSON Lines (one JSON object per line), appended by `lab/experiments.sh` and
`lab/measure.sh`, and rendered into tables by `python3 lab/render_tables.py`
(`--write` updates the README between its `<!-- results:expN -->` markers).
Rows are never edited by hand. A bad run is removed together with a note in the
commit message, never "corrected".

| File | Experiment | Written by |
|---|---|---|
| `exp1_pps.jsonl` | 1, packet rate at saturation | `lab/measure.sh` via `experiments.sh exp1` |
| `exp1_sweep.jsonl` | 1, offered-load sweep (the knee) | same, `--rate` set |
| `exp2_http.jsonl` | 2, wrk latency and throughput | `experiments.sh exp2` |
| `exp3_churn.jsonl` | 3, connection survival through backend churn | `experiments.sh exp3` (conncheck) |
| `exp4_failover.jsonl` | 4, connection survival through LB failover | `experiments.sh exp4` (conncheck) |
| `exp5_hash_quality.json` | 5, hash quality (one JSON document) | `tools/hashquality` |
| `exp1_mitigations.jsonl` | 1, native-XDP loss accounting and lab mitigations (`variant`) | `lab/measure.sh` with `MEASURE_VARIANT` |
| `exp6_conntrack.jsonl` | 6, cost of the connection table | `lab/measure.sh` via `experiments.sh exp6` |
| `restart.jsonl` | daemon restart keeps flows | `experiments.sh restart` (conncheck) |
| `rejected.jsonl` | packet-rate windows that failed the host contention guard | `lab/measure.sh` |

## What each `lb` label means

| `lb` | Forwarding plane in lb1 (and lb2 for Exp 4) | Packet to the real |
|---|---|---|
| `packetbalance` | the XDP program, `xdp_mode` says native or generic; `hash` maglev or modulo; `conntrack` on or off | IPIP, outer source from 10.99.0.0/24 by flow hash |
| `ipvs-mh` | IPVS, `mh` scheduler (Maglev inside IPVS) with `-b mh-port`, direct routing `-g`. The spec's fair baseline | original packet, destination MAC rewritten |
| `ipvs-rr` | IPVS, round robin, direct routing `-g` | original packet, destination MAC rewritten |
| `ipvs-mh-tun` | IPVS, `mh` with `-b mh-port`, tunneling `-i` | IPIP, outer source = the director's address. Like for like with PacketBalance: same 20 extra bytes, same decap on the real |
| `none` | nothing: the client sends straight to real1 (Exp 1: pktgen to 10.0.0.21; Exp 2: wrk to http://10.0.0.21/) | original packet. The ceiling of the VM's virtual network |

DR only rewrites the MAC, so the `-g` rows do slightly less work per packet
than PacketBalance, which encapsulates; the `ipvs-mh-tun` row removes that
difference. The `-g` rows stay because they are the spec's baseline and how
IPVS is usually deployed.

## Fields on every row

| Field | Meaning |
|---|---|
| `experiment` | `exp1`, `exp1_sweep`, `exp2`, `exp3`, `exp4`, `exp6` |
| `host` | from `lab/host.env`, e.g. `Apple M3 Pro via Lima vz`. A VM, always |
| `cpus`, `kernel`, `arch` | `os.cpu_count()`, `uname -r`, `uname -m` inside the VM |
| `lb` | which forwarding plane, see the label table below |
| `config` | the harness's label for the configuration (Exp 2 to 4) |
| `xdp_mode` | `native` or `generic` for PacketBalance, `null` otherwise |
| `hash` | `maglev` or `modulo` for PacketBalance, `mh`/`rr` for IPVS, `null` for none |
| `conntrack` | PacketBalance connection table on or off. IPVS rows say `true` (IPVS always keeps its connection table) |
| `date` | ISO 8601, UTC |
| `repeat` | 1..3 |
| `notes` | free text: failures, caveats, the body of a curl through the VIP |

## Packet-rate rows (Exp 1, Exp 1 sweep, Exp 6), `lab/measure.sh`

| Field | Meaning |
|---|---|
| `generator` | `pktgen` (kernel module, in the client namespace) or `udpblast` (userspace fallback) |
| `generator_threads` | pktgen kernel threads (`PKTGEN_THREADS`, default 1) |
| `pkt_size` | Ethernet frame size **including** the 4-byte FCS a physical NIC would add, 64 by default |
| `pkt_size_skb` | pktgen's `pkt_size`, the frame without FCS, which is what a veth carries: 60 |
| `flows` | distinct UDP 5-tuples offered: source port walks 1024..1024+flows-1 |
| `dst` | the VIP, or real1's address for `lb=none` |
| `target_rate_pps` | pktgen `ratep`; 0 = unthrottled |
| `duration_s` | measured length of the counter window (warmup excluded) |
| `offered_pps` | pktgen `pkts-sofar` delta over the window |
| `forwarded_pps` | PacketBalance: delta of the sum of the per-VIP `tx` counters from `pbctl --json stats`. IPVS: delta of the sum of `InPkts` over services from `ipvsadm -Ln --stats --exact` (in DR mode every accepted packet is forwarded). `null` for `none` |
| `received_pps` | delta of `rx_packets` on `veth0` summed over every real namespace (`/proc/net/dev`), i.e. what actually arrived |
| `received_tunnel_pps` | the same on the reals' IPIP device (`tunl0`), nonzero only for PacketBalance |
| `lb_cpu_util` | `100 - %idle` of the `all` row of `mpstat -P ALL 1 <duration>` during the window |
| `softirq_util` | `%soft` of the `all` row (where XDP, the bridge and IPVS run) |
| `per_cpu_util` | `100 - %idle` per CPU |
| `pps_per_core` | estimate, see below, from `forwarded_pps` |
| `pps_per_core_basis` | `forwarded_pps`, or `received_pps` for `lb=none` |
| `pps_per_core_received` | the same estimate from `received_pps`. **The tables use this one**: it counts only frames that arrived |
| `reals_rx_dropped_pps` | frames that reached a real's veth0 but were dropped by its full receive backlog (`rx_dropped`, `net.core.netdev_max_backlog`); not in `received_pps` |
| `pb_counters_delta`, `pb_drops_delta` | PacketBalance only: every `pbctl --json stats` counter summed over global and VIPs, as a delta over the window; the nonzero `drop_*` reasons |
| `lb_veth_xdp_delta` | the LB veth0's per-queue XDP stats from `ethtool -S`, summed, delta over the window. `xdp_tx_errors` = frames the XDP program XDP_TX'd that the peer's ring refused (native mode); PacketBalance counts them in `tx` |
| `lb_veth_tx_dropped_pps` | the LB veth0's `tx_dropped` rate (generic-mode XDP_TX and the IPVS transmit path lose frames here when the peer is full) |
| `lb_veth_xdp_tx_errors`, `lb_veth_xdp_drops` | the two XDP driver counters from `lb_veth_xdp_delta`, as window counts |
| `peer_rx_dropped` | `rx_dropped` of the LB veth's bridge-side peer (`pb-lb1`) over the window: the same event as `lb_veth_tx_dropped`, seen from the other end |
| `peer_xdp_delta` | `ethtool -S pb-lb1` XDP stats (the do-nothing XDP_PASS program there, which drains native XDP_TX frames) |
| `netdev_delta` | `rx/tx_packets`, `rx/tx_dropped` over the window of every device on the path: `client:veth0`, `root:pb-client`, `root:pb-lb1`, `root:br0`, `lb1:veth0`, `root:pb-realN`, `realN:veth0`, plus `softnet_dropped` (/proc/net/softnet_stat, backlog-full drops, whole VM) |
| `packet_accounting` | PacketBalance rows: where every frame went, as window counts. See "Packet accounting" below |
| `variant` | `standard`, or the name of a lab mitigation (`exp1_mitigations.jsonl`) |
| `mpstat_samples` | one-second `mpstat` samples in the window (must equal the duration) |
| `cpu_canary`, `cpu_canary_in_window`, `cpu_canary_min_required`, `quiet_wait_s`, `vm_build_seconds_in_window`, `host_gate` | host contention guard, see below |

### pps per core, and why it is an estimate

```
pps_per_core = forwarded_pps / ((lb_cpu_util / 100) * cpus)
```

All namespaces share the VM's CPUs. `lb_cpu_util` therefore counts the
generator (pktgen's kernel thread), the bridge, the LB, and the reals' receive
path, not only the load balancer. The estimate charges all of that to the LB, so
it is a **lower bound** on the LB's own packets per second per core. The bias is
the same for every configuration on the same host, which keeps comparisons
between rows fair, but the absolute number is not a per-core figure for a
dedicated LB host. `docs/CAPACITY.md` says so where it uses it.

With native XDP on a veth, the XDP program runs in the veth's NAPI poll on the
CPU that transmitted into the veth, so at one pktgen thread most of the path
runs on one CPU; `per_cpu_util` shows where the work landed.

### Packet accounting (PacketBalance rows)

```
offered_by_client            client veth0 tx_packets
 - dropped_before_lb_program   pb-lb1 tx_dropped: the LB veth's receive ring was full, the XDP program never saw the frame
 = frames the program ran on ~ pb_tx (every VIP frame ends in XDP_TX; pb_drops_delta is 0 in every row)
pb_tx                        the program's tx counter (XDP_TX verdicts)
 = received_at_reals          reals' veth0 rx_packets
 + lb_veth_tx_dropped         the LB veth could not hand the frame to its peer pb-lb1 (peer ring full).
                              Native: also counted as xdp_tx_errors on lb1 veth0 and rx_dropped on pb-lb1
                              (same_event_* fields, one event, three counters, subtracted once)
 + peer_xdp_drops             pb-lb1's XDP_PASS program dropped it (always 0)
 + reals_rx_dropped           the real's receive backlog was full (same event as the bridge port's tx_dropped)
 + unaccounted                timing skew between the stats and netdev snapshots, frames in flight
```

A 30 s native window at saturation (4 pktgen threads, 10,000 flows), from
`exp1_mitigations.jsonl` / `exp1_pps.jsonl`: the client offered about 150 M frames,
108 M were dropped before the program because lb1's veth receive ring was full,
the program forwarded about 42 M (its `tx`), 23 M of those were lost handing
them to pb-lb1 (`xdp_tx_errors`), and 19 M arrived at the reals. The loss is
in the veth driver on either side of the program, not in the program: its own
drop counters are 0 and the conntrack hit rate is above 99.9%.

### Exp 1 in this VM: native XDP forwards less than IPVS, and why

Published as measured. On veth, native XDP is not "the driver's RX ring before
the skb": the frame arrives through the bridge-side peer's transmit into lb1's
veth ptr_ring, the program runs in that veth's NAPI poll, and XDP_TX puts the
frame on the peer's (pb-lb1's) ptr_ring, where a second XDP program
(XDP_PASS) has to build the skb that the bridge forwards. Both rings hold 256
frames, and all of it runs in softirq on the CPU of the pktgen thread that sent
the frame (CPUs 0 to 3; CPUs 4 and 5 stay idle). Unthrottled, the generator
fills lb1's ring faster than the NAPI drains it (about 70% of offered frames
are dropped before the program), and the program's XDP_TX outruns pb-lb1's
drain (about half of what it forwards is lost there). IPVS and generic XDP
take the stack's backlog path, which paces the generator (offered 2.2 to
2.5 M instead of 5 M) and loses less. The throttled sweep shows the same: every
plane is lossless up to 1 M pps; at 2 M offered, native delivers 1.64 M, generic
1.72 M, IPVS `mh` 1.82 M.

`exp1_mitigations.jsonl` tries lab changes one at a time. Steering the skb work
after pb-lb1 to CPUs 4 and 5 with RPS lifts native from 0.64 M to 0.97 M and
generic from 1.39 M to 1.75 M received, but lowers IPVS `mh` from 2.21 M to
1.55 M, so it is not a neutral lab setting and the published table does not
use it. Halving the generator to 2 threads makes generic and IPVS generator
bound (0.95 M and 0.91 M, offered = received) and native still loses 60% of
its offered load before the program (0.77 M received). Enabling NAPI on pb-lb1
with GRO instead of the XDP_PASS program does not deliver native XDP_TX frames
at all on this kernel (every curl timed out), as `docs/bugs/lab.md` records.
None of this says what native XDP does on a physical NIC, where the program runs
on the driver's RX ring and XDP_TX goes to the NIC's own TX ring.

### Host contention guard

The VM's vCPUs are threads on a shared Mac. Before this guard existed, three
30 s windows of the same IPVS configuration measured 121 k, 1.04 M and 2.06 M
pps (moved to `superseded/`, see `docs/bugs/lab.md`). Now every packet-rate
window (and every Exp 2 wrk run) starts only when no compiler or build runs in
the VM and a CPU canary (a fixed single-threaded Python loop pinned to the last
vCPU, which the generator does not use; iterations per second) is at least
`CANARY_MIN` (12,000,000 in this lab; about 17 M on a quiet host, 7 to 12 M with
a video call running on the Mac). The canary is taken again when the window
ends. A window whose canary fell below the minimum, or that had a build running,
is written to `rejected.jsonl` instead and retried (`host_gate: "fail"`).
Rejected windows are kept there, not deleted.

## `superseded/`

Rows measured before a harness change that makes them incomparable with the
current rows. They are kept for provenance and are not rendered.
`*.pre-host-gate.jsonl`: Exp 1, Exp 1 sweep and Exp 2 baseline rows measured
without the host contention guard; every configuration was re-measured with it.

## Daemon restart rows (`restart.jsonl`)

`lab/experiments.sh restart`: conncheck holds 1,000 connections through lb1
(native); the daemon, started without `--detach-on-exit`, gets SIGTERM at t=10 s
and is started again at t=15 s. `flows_tracked_before` /
`flows_tracked_after_restart` are distinct 5-tuples in the pinned connection
table (`pbctl flows`); `xdp_while_daemon_down` is the program still attached
while no daemon ran.

### Exp 3 `add`: why PacketBalance with its connection table still broke 0.2 to 3.5%

The connection table is an LRU_PERCPU_HASH: a flow's entry lives only on the
CPUs that have seen its packets. On a veth the XDP program runs on the CPU that
transmitted the frame, i.e. wherever the conncheck thread sending that
heartbeat was scheduled. When a heartbeat of an established flow arrives on a
CPU with no entry for it, the program falls back to the hash, and while real5
is in the ring about 1/5 of those flows hash to real5, which answers the
unknown ACK with a RST (`broken_by_cause.rst`; `broken_by_backend` shows them
spread over real1..real4, the reals they were on). The number of broken flows
therefore depends on how many flows changed CPU during the 10 s real5 was in the
ring (20, 315 and 350 in the three repeats). IPVS keeps one shared connection
table and broke 0. Without the table (no conntrack) Maglev broke 20 to 35%,
the 1/(N+1) = 20% the hash must move plus flows caught by the second ring change
at t=20 s. This is the per-CPU LRU trade-off that docs/DESIGN.md describes,
measured.

## conncheck rows (Exp 3, Exp 4)

The row is `conncheck --json` output merged with the configuration fields.

| Field | Meaning |
|---|---|
| `total` | connections attempted (10,000) |
| `established` | handshakes completed |
| `connect_failed`, `connect_fail_reasons` | never established, by errno text |
| `broken` | established connections that broke, any cause |
| `broken_by_cause` | `rst` (ECONNRESET/EPIPE), `eof` (peer FIN), `timeout` (a heartbeat unanswered for 2 s, or kernel ETIMEDOUT), `wrong_backend` (a heartbeat answered by a different real id than the first) |
| `survived` | connections still healthy at the end |
| `backends_at_start` | established connections per real id once the ramp finished (`"3"` = real3) |
| `backends_at_end` | healthy connections per real id at the end |
| `broken_by_backend` | broken connections per the real id they were on when they broke |
| `timeline` | per second since start: broken total and by cause (seconds with none omitted at the tail) |
| `ramp_complete_s` | when every connect had resolved and every connection had its first id |
| `heartbeat_ms`, `timeout_ms`, `duration_s` | instrument settings |
| Exp 3: `scenario` | `remove`: real3 removed at t=10 s and added back at t=20 s. `add`: real5 (not in the configuration) added at t=10 s and removed at t=20 s, on live traffic to real1..real4. The `remove` rows measured before the field existed got `scenario: "remove"` added by a one-off script (their `events` field says the same) |
| Exp 3 remove: `removed_real`, `removed_backend_id`, `events` | real3 removed at t=10 s, added at t=20 s |
| Exp 3 add: `added_real`, `added_backend_id`, `min_fraction_moved`, `events` | real5 added at t=10 s, removed at t=20 s; 0.2 = 1/(N+1), what a consistent hash alone must move |
| Exp 4: `drift`, `reals_lb1`, `reals_lb2`, `min_fraction_moved`, `events` | route flip at t=10 s; drift adds real5 on lb2 only; `min_fraction_moved` = 1/(N+1) = 0.2 is the least any hash can move when a fifth real appears |
| Exp 4 IPVS: `ipvs_sloppy_tcp` | `net.ipv4.vs.sloppy_tcp` on both directors. With 0, a director that never saw the SYN does not schedule a flow at all (see `lab/ipvs.sh`) |

## Exp 2 rows (wrk)

`req_per_s`, `requests`, `latency_avg_us`, `latency_stdev_us`, `latency_max_us`,
`p50_us`, `p75_us`, `p90_us`, `p99_us` (from `wrk --latency`), `socket_errors`,
`non_2xx`, `url`, `wrk_threads`, `wrk_connections`, `duration_s`. The `none` row
is direct to real1, one nginx instead of four; the note says so.

## Lab settings that shape the numbers

- veths are MTU 1500. The client's VIP route has `mtu 1480`, so TCP's MSS is
  1440 and an IPIP-encapsulated full segment still fits. Both LBs get the same
  route.
- IPVS: direct routing (`-g`), `mh` with `-b mh-port` (without it `mh` hashes
  only the source address and every lab flow lands on one real),
  `expire_nodest_conn=1`, `conntrack=0`, `sloppy_tcp=1` unless a row says 0.
- The reals' UDP echo servers are paused during packet-rate windows; received
  packets are counted at the interface.
- The root-side peers of the LB veths (pb-lb1, pb-lb2) carry a do-nothing
  XDP_PASS program, required for native XDP_TX on veth; it is there for IPVS
  runs as well. The client's veth has TX checksum offload off, so it sends
  complete checksums as a physical NIC would. Both are in `docs/bugs/lab.md`.
- pktgen runs 4 kernel threads by default (`PKTGEN_THREADS`); one thread is
  generator bound at about 0.7 Mpps through IPVS in this VM.
