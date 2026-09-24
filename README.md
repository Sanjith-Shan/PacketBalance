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

Every number names the VM, kernel, XDP mode, packet size and flow count. The lab is one
VM, not a datacenter. The load balancer, the traffic generator and the backends share the
same virtual CPUs, and the load balancer is attached to a veth, not a physical NIC. The
numbers are comparable to each other and to the IPVS baseline on the same VM, and to
nothing else. They are not comparable to Katran's production figures. Each table reports
the mean over three repeats with the range. [docs/CAPACITY.md](docs/CAPACITY.md) turns
Experiment 1 into a capacity model and explains why the VM number is a lower bound.

### Experiment 1: packet rate at saturation

`pktgen` sends 64-byte UDP packets to the VIP, unthrottled, over 10,000 flows for 30 s.
The configurations are PacketBalance in native and generic XDP mode, IPVS with the `mh`
(Maglev) and `rr` schedulers in direct-routing mode, and no load balancer at all, which is
the ceiling of the VM's virtual network. The number that matters is forwarded packets per
second per core of load balancer CPU. XDP should forward more per core than IPVS in native
mode because it never builds an skb. Generic mode runs after the skb exists, so it pays
most of what IPVS pays, and it may lose to IPVS. If it does, that is the honest cost of
running XDP without driver support. IPVS in direct-routing mode rewrites only the MAC,
while PacketBalance adds and checksums a 20-byte header, so the comparison slightly
favors IPVS.

<!-- results:exp1 -->
<!-- /results:exp1 -->

### Experiment 2: latency and throughput through the load balancer

`wrk -t4 -c256 -d30s` against nginx on the backends, through PacketBalance, through IPVS,
and directly to one backend. The table gives requests per second and p50 and p99 latency.
A load balancer should add microseconds. In this lab the client, load balancer and
backends share CPUs, so a difference in requests per second can come from CPU contention
rather than forwarding cost, and the direct row is the reference for both.

<!-- results:exp2 -->
<!-- /results:exp2 -->

### Experiment 3: connection survival through backend churn

`conncheck` holds 10,000 long-lived TCP connections through the VIP, with a heartbeat
every 100 ms that the backend answers with its id. The harness removes real3, waits 10 s,
adds it back, and waits 10 s. Broken connections are counted by cause (reset, timeout,
answered by a different backend). With Maglev and the connection table, exactly the
connections on real3 should break and no others. Removing the connection table
(`--no-conntrack`) adds the flows whose ring slots moved, and re-adding real3 breaks more.
Replacing Maglev with a modulo ring (`--hash modulo`) makes the ring change touch almost
every flow, and the connection table is then the only protection. IPVS keeps a connection
table too and should also lose only real3's connections.

<!-- results:exp3 -->
<!-- /results:exp3 -->

### Experiment 4: connection survival through load balancer failover

`conncheck` holds 10,000 connections through lb1, and the harness moves the client's route
to lb2, which has the same configuration and an empty connection table. PacketBalance on
lb2 computes the same Maglev ring from the same backends, so every flow should hash to its
original backend and survive. IPVS `rr` has never seen these connections and assigns each
to the next backend, so about three quarters should break with four backends. IPVS `mh` is
Maglev inside the kernel and should survive as well as PacketBalance. If it does, that is
the expected result, because consistent hashing is what makes a stateless load balancer
tier possible, whichever program implements it. The drift case adds real5 on lb2 only
before the flip. Maglev predicts that about 1/N of connections break. A modulo ring
predicts almost all.

<!-- results:exp4 -->
<!-- /results:exp4 -->

### Experiment 5: hash quality

One million synthetic 5-tuples, hashed with the data plane's own flow hash, go through a
Maglev ring and a modulo ring with backend weights 1:1:2:4. The table compares each
backend's share with its weight, and the fraction of flows that change backend when one
backend is removed and when one is added, against the minimum possible. Both rings should
match the weights closely. Maglev's disruption should be close to the minimum. Modulo's
should be close to all flows. This experiment runs in the control plane's test tool and
is the same on any machine.

<!-- results:exp5 -->
<!-- /results:exp5 -->

### Experiment 6: what the connection table costs

Experiment 1 again, with the connection table off and at 64K, 1M and 8M entries. Katran
found that computing the hash can be cheaper than the table lookup. If PacketBalance
forwards faster with the table off, the same holds here, and the table is paying only for
correctness during backend changes. If the rate falls as the table grows, the cost is
cache misses in a larger table. If nothing changes, 10,000 flows fit in cache at every
size and the experiment cannot see the effect, which the table would say.

<!-- results:exp6 -->
<!-- /results:exp6 -->

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
created.

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
