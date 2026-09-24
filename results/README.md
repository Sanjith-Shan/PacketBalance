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
| `exp6_conntrack.jsonl` | 6, cost of the connection table | `lab/measure.sh` via `experiments.sh exp6` |

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
| `pps_per_core` | estimate, see below |
| `pps_per_core_basis` | `forwarded_pps`, or `received_pps` for `lb=none` |

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
| Exp 3: `removed_real`, `removed_backend_id`, `events` | real3 removed at t=10 s, added at t=20 s |
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
