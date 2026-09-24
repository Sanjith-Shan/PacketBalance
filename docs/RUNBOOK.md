# PacketBalance runbook

Procedures for operating a PacketBalance load balancer. Every `pbctl` command is the one
documented in [API.md](API.md). The reasoning behind each behavior is in
[DESIGN.md](DESIGN.md).

## Conventions

Commands are written for a single host installed from `deploy/`, with the defaults.

| Thing | Default | In the lab (`lab/common.sh`) |
|---|---|---|
| Config | `/etc/packetbalance/packetbalance.yaml` | `lab/gen/lb1.yaml`, `lab/gen/lb2.yaml` |
| Control socket | `/run/packetbalance.sock` | `/run/pblab/pb-lb1.sock`, `/run/pblab/pb-lb2.sock` |
| Pinned maps | `/sys/fs/bpf/packetbalance/` | `/sys/fs/bpf/packetbalance/lb1/`, `.../lb2/` |
| Interface | from the config (`interface:`) | `veth0` inside netns `lb1` or `lb2` |
| Metrics | `127.0.0.1:9101/metrics` | the same, inside each LB's netns |
| Daemon log | `journalctl -u packetbalance` | `/run/pblab/pb-lb1.log` |

In the lab, prefix interface and metrics commands with `ip netns exec lb1`, and give
`pbctl` the socket, for example `pbctl --socket /run/pblab/pb-lb1.sock stats`. Unix
sockets are not bound to a network namespace, so `pbctl` itself can run from anywhere.
`bpftool` also runs from the root namespace, because bpffs pins are global.

The examples use the lab's VIP and reals, `198.51.100.1:80/tcp` and `10.0.0.21` to
`10.0.0.24`.

**Every LB in a tier must be given the same change.** Two load balancers keep
connections alive across a failover only if they build the same ring, and they build the
same ring only from the same set of reals and weights. A change applied to one LB and
not the others is a latent outage that shows up at the next failover.

**Runtime changes are not persistent.** `pbctl vip add`, `real add` and the rest change
the running daemon. `pbctl reload` and a daemon restart re-read the YAML and remove
anything not in it. Make the change in the YAML and run `pbctl reload`, or make it with
`pbctl` and update the YAML in the same change.

## Install and start

On Linux with clang, libbpf, bpftool and the packages in `lab/lima.yaml`:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

sudo install -m 0755 build/src/daemon/packetbalance /usr/local/sbin/packetbalance
sudo install -m 0755 build/src/pbctl/pbctl /usr/local/bin/pbctl
sudo install -D -m 0644 deploy/packetbalance.yaml /etc/packetbalance/packetbalance.yaml
sudo install -m 0644 deploy/packetbalance.service /etc/systemd/system/packetbalance.service
```

Check the `ExecStart=` path in the unit (`systemctl cat packetbalance`) against where the
binary was installed. The data-plane object is compiled into the daemon as a libbpf
skeleton, so there is no separate `.bpf.o` to install.

Edit `/etc/packetbalance/packetbalance.yaml`. At minimum set `interface`, and set
`encap_src_prefix` to a prefix nothing else on the network uses. Make sure bpffs is
mounted (systemd mounts it on most distributions):

```sh
mount | grep -q ' /sys/fs/bpf ' || sudo mount -t bpf bpf /sys/fs/bpf
```

Start it:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now packetbalance
```

Verify, in this order:

```sh
systemctl status packetbalance              # active (running)
journalctl -u packetbalance -n 50           # "attached xdp_packetbalance to <if> in native mode"
ip -d link show dev <if>                    # "prog/xdp id N" (native) or "prog/xdpgeneric id N"
pbctl ping                                  # pong
pbctl vip list                              # every VIP from the config, reals up
pbctl ring show 198.51.100.1:80/tcp         # slots split by weight, "none": 0
curl -s 127.0.0.1:9101/metrics | grep pb_real_up
```

If the log says it fell back to generic mode, the driver does not support native XDP or
the attach failed. Generic mode forwards correctly at a much higher CPU cost. Decide
whether that is acceptable before putting the host into service.

The unit restarts the daemon on failure. A restart does not interrupt forwarding (see
[Hitless daemon restart](#hitless-daemon-restart)).

## Add a VIP

```sh
pbctl vip add 198.51.100.1:443/tcp
pbctl real add 198.51.100.1:443/tcp 10.0.0.21
pbctl real add 198.51.100.1:443/tcp 10.0.0.22
pbctl real add 198.51.100.1:443/tcp 10.0.0.23 --weight 2
pbctl vip list
pbctl ring show 198.51.100.1:443/tcp
```

`ring show` should give each real a slot count proportional to its weight, and
`"none": 0`. A non-zero `none` means some slots have no real, and packets hashing there
are dropped as `no_real`.

Before the VIP carries traffic, every real must be configured for DSR (VIP on `lo`,
`tunl0` up, `rp_filter` 0, `arp_ignore=1`, `arp_announce=2`, service listening on the VIP
or the wildcard). The table in DESIGN.md under "Why DSR with IPIP" lists them with the
reason for each. A real missing any of them passes its health check (which connects to
the real's own address) and black-holes its share of the VIP.

Then add the VIP to the YAML on every LB, and route or announce the VIP to the LBs.
Announcement is outside PacketBalance.

To remove a VIP, `pbctl vip del 198.51.100.1:443/tcp`, and remove it from the YAML.
Established connections to it break immediately, because the `vip_map` entry is gone
and their packets are passed to the LB host, which does not own the VIP.

## Add or remove a real

```sh
pbctl real add 198.51.100.1:80/tcp 10.0.0.25 --weight 1
pbctl ring show 198.51.100.1:80/tcp
```

Adding a real moves about 1/N of the ring's slots to it (N counted after the add).
Established connections are unaffected because the connection table pins them. New
connections start going to the new real within one ring swap.

Removing a real without draining it breaks every connection on it, immediately:

```sh
pbctl real del 198.51.100.1:80/tcp 10.0.0.25
```

Use it for a real that is already dead. For a live real, drain first.

## Drain a real for maintenance

Draining sets the real's weight to 0. It takes no ring slots, so no new flow hashes to
it, and existing flows keep reaching it through the connection table. Health checks
continue.

1. Drain it on every LB:

   ```sh
   pbctl real drain 198.51.100.1:80/tcp 10.0.0.23
   pbctl vip list                             # draining: true
   pbctl ring show 198.51.100.1:80/tcp        # 10.0.0.23 has 0 slots
   ```

2. Watch its flows drain. The connection table is not cleaned on FIN, so closed
   connections stay listed until the LRU evicts them. Count only recently active entries:

   ```sh
   pbctl --json flows --real 10.0.0.23 --limit 1000000 \
     | jq '[.[] | select(.age_ms < 30000)] | length'
   ```

   and watch the real's packet rate fall:

   ```sh
   watch -n5 "curl -s 127.0.0.1:9101/metrics | grep 'pb_real_packets_total{real=\"10.0.0.23\"}'"
   ```

   `pb_real_packets_total` is a counter, so watch its rate (in Prometheus,
   `rate(pb_real_packets_total{real="10.0.0.23"}[1m])`), not its value. The ground truth
   is on the real itself, `ss -Htn state established '( sport = :80 )' | wc -l`.

3. When the rate is at or near zero, or the maintenance deadline arrives, remove it:

   ```sh
   pbctl real del 198.51.100.1:80/tcp 10.0.0.23
   ```

Long-lived connections (websockets, database pools, UDP flows, which never end) will not
drain on their own. Decide how long to wait before starting. For UDP, `age_ms` is the
only signal that a flow is idle.

To return the real to service, `pbctl real undrain 198.51.100.1:80/tcp 10.0.0.23`, which
restores its configured weight, or `real add` if it was deleted.

What a drain does not protect. A flow that misses the connection table (evicted from a
full LRU, or processed on a different CPU than before) re-hashes, and the drained real is
no longer in the ring, so that connection moves and breaks. So does a connection moved to
an LB whose table has never seen it. RSS normally keeps a flow on one CPU, so the first
case is rare. Check `pb_conntrack_misses_total` before a large drain if the table is
close to full.

## Change a weight

```sh
pbctl real weight 198.51.100.1:80/tcp 10.0.0.24 4
pbctl ring show 198.51.100.1:80/tcp
```

The new share applies to new connections. Established connections stay where they are,
so the real's actual load converges over the connection lifetime, not instantly. The
ring change moves roughly the change in the real's share of slots. Going from 1:1:1:1 to
1:1:1:4 moves the fourth real from 25% to about 57% of slots. Apply it on every LB and
in the YAML.

## Hitless daemon restart

Stopping the daemon does not detach the XDP program or delete the maps, so forwarding
continues while the daemon is down. Health checks, the API and metrics stop.

Prove it with live connections. In one terminal, hold connections through the VIP (in the
lab, from the client namespace):

```sh
ip netns exec client build/tools/conncheck/conncheck \
  --vip 198.51.100.1:7000 --connections 1000 --heartbeat-ms 100 \
  --duration-s 60 --json /tmp/restart.json
```

In another, restart:

```sh
sudo systemctl restart packetbalance
journalctl -u packetbalance -n 100 | grep -E 'adopted|replacing|ring:'
```

Expect `adopted real_id ...` for every real, `startup (adopted)` for every VIP, one
`ring: ... generation=` line per VIP, and `replacing the previous program` on attach.
When conncheck finishes, its JSON must show zero broken connections of any cause.
Counters in `pbctl stats` continue from where they were, because the stats maps are
pinned too.

If the daemon fails to start after a restart with `pinned map ... does not match this
build`, see the next section.

Keep the down window short. While the daemon is down the rings are frozen. A real that
dies in that window keeps its slots and black-holes its share of new connections until
the daemon returns.

## Roll a new BPF object

The XDP program is embedded in the daemon binary, so rolling the data plane means
installing a new `packetbalance` binary and restarting. When the new build's map
definitions match the pinned maps, the restart replaces the program atomically and keeps
the connection table. There is no instant with no program attached.

1. Take the LB out of ECMP (withdraw the VIP announcement, or in the lab
   `sudo lab/route-via.sh lb2`). This is not required for a matching build, but it costs
   nothing and makes a bad build harmless.
2. Install the new binary and restart:

   ```sh
   sudo install -m 0755 build/src/daemon/packetbalance /usr/local/sbin/packetbalance
   sudo systemctl restart packetbalance
   journalctl -u packetbalance -n 50
   bpftool net show dev <if>        # new program id
   ```

3. If the log says `pinned map <path> does not match this build (...)`, the new build
   changed a map (the conntrack size, or a struct in `abi.h`). The daemon refused to
   start and the old program is still attached and forwarding. To proceed, restart with
   `--recreate-maps`, which unpins the mismatched maps and creates new ones. The new
   connection table starts empty, and established flows fall back to the ring. They
   survive as long as the ring does not change at the same moment, so do not combine a
   map-changing roll with a real or weight change.
4. Verify with `pbctl stats` (counters moving, no new drop reason), send test traffic,
   then put the LB back into ECMP.

Rollback is the same procedure with the previous binary.

Changing `xdp_mode` between native and generic is not hitless. The kernel will not hold a
native and a generic program on one interface at once, so the daemon detaches first and
logs `(not hitless)`. Do it with the LB out of ECMP.

## Diagnose: the VIP is black-holing

The symptom is that connections to the VIP time out, or connect and then hang. Walk the packet path
in order and stop at the first step that fails. `sudo lab/diagnose.sh lb1` runs this
checklist against the lab and prints PASS, FAIL or INFO per step.

**1. Does traffic reach the LB?** From a client, `ip route get 198.51.100.1` must point at
the LB. On the LB side, do not `tcpdump` the interface XDP is attached to. XDP runs
before the capture hook, so packets the program transmits or drops are never seen there,
in either mode. Capture on the far side of the link instead. In the lab that is the
LB's bridge-side peer in the root namespace:

```sh
tcpdump -ni pb-lb1 -c 20 'host 198.51.100.1 or ip proto 4'
```

Client packets to the VIP going in and IPIP packets (proto 4) coming back out means the
LB is forwarding. Packets going in and nothing coming out means go to step 2.

**2. Is the program attached, in the mode you expect?**

```sh
ip -d link show dev <if>          # prog/xdp id N, or prog/xdpgeneric id N
bpftool net show dev <if>
bpftool prog show id <N>
```

No program means the daemon never attached or someone ran `ip link set dev <if> xdp off`.
Check `journalctl -u packetbalance` for the attach line and any verifier error. Two
programs on one interface are not possible, but IPVS rules on the same host are. If
`ipvsadm -Ln` lists the VIP, two forwarding planes are fighting over it.

For per-packet cost while you are here, `sysctl -w kernel.bpf_stats_enabled=1` makes
`bpftool prog show` report `run_cnt` and `run_time_ns`. Turn it off afterwards, it costs
a little on every run.

**3. Are the maps what the daemon thinks they are?**

```sh
P=/sys/fs/bpf/packetbalance
bpftool map dump pinned $P/vip_map
bpftool map dump pinned $P/reals
bpftool map dump pinned $P/neigh
bpftool map dump pinned $P/config
```

Keys and values are in network byte order. 198.51.100.1 port 80 TCP is the `vip_map` key
`c6 33 64 01 00 50 06 00`. 10.0.0.21 in `reals` is `0a 00 00 15`. A `neigh` value of all
zeros for a real in use means its MAC was never resolved (see the next section). A
`config` value with a zero LB MAC means the daemon never finished writing its config.

**4. What do the counters say?**

```sh
pbctl stats --vip 198.51.100.1:80/tcp
```

If `packets` is not increasing for the VIP while traffic arrives, the packets are not
matching `vip_map`. Check the address, port and protocol, and whether the global `pass`
counter is climbing instead. If `packets` climbs but `tx` does not, one of the drop
counters is climbing.

| Drop reason | Meaning | Usual cause |
|---|---|---|
| `no_real` | The ring slot had no real, or the real's slot in `reals` or `neigh` was empty | Every real down, draining or unresolved. `pbctl health`, `pbctl ring show` (`none` > 0) |
| `mtu` | Inner packet plus 20 bytes exceeds 3,500 | Jumbo frames from clients. See [MTU](#diagnose-mtu-black-hole-for-large-requests) |
| `frag` | IPv4 fragment (MF set or non-zero offset). Counted globally | A client or middlebox fragmenting, usually large UDP. PacketBalance never forwards fragments |
| `opts` | IPv4 header with options. Counted globally | Rare. Some scanners and old stacks |
| `short` | Truncated Ethernet, IP, TCP, UDP or ICMP header, or `tot_len` past the frame | Malformed traffic, or a driver passing partial frames |
| `adj_head` | `bpf_xdp_adjust_head` could not add 20 bytes of headroom | A driver with no XDP headroom. Try generic mode to confirm |
| `other` | IP version not 4, or the `config` map missing | Daemon started without writing config, or bad traffic |

The same counters are `pb_drops_total{reason=...}` in the metrics.

**5. Are the reals up and in the ring?**

```sh
pbctl health
pbctl ring show 198.51.100.1:80/tcp
```

**6. Does the encapsulated packet reach the real, and does the real decapsulate it?** On
the real (in the lab, `ip netns exec real1 ...`) run

```sh
tcpdump -ni <nic> -c 10 'ip proto 4'      # outer packets arriving from the LB
tcpdump -ni tunl0 -c 10                   # the same packets after decapsulation
```

Outer packets on the NIC but nothing on `tunl0` means decapsulation is not happening.
Check `lsmod | grep ipip` and `ip link show tunl0` (must be UP), and that the outer
destination is an address the real owns.

**7. Does the real accept the decapsulated packet?** Packets on `tunl0` but no reply
leaving the real is almost always one of these three.

```sh
sysctl net.ipv4.conf.all.rp_filter net.ipv4.conf.tunl0.rp_filter   # both must be 0 (or 2)
nstat -az TcpExtIPReversePathFilter                                 # climbing = rp_filter drops
ip addr show dev lo | grep 198.51.100.1                             # VIP must be on lo
ss -ltn '( sport = :80 )'                                           # listening on 0.0.0.0 or the VIP
```

The effective `rp_filter` is the larger of `all` and the device's value, so setting only
`tunl0` to 0 is not enough if `all` is 1.

**8. Does the reply reach the client?** `tcpdump -ni <nic> 'src host 198.51.100.1'` on the
real, then on the client. A reply leaving the real and not reaching the client is a
routing or firewall problem on the return path, which PacketBalance is not on. In an L2
setup, also check `arp_ignore=1` and `arp_announce=2` on the reals. A real answering ARP
for the VIP pulls traffic away from the LB entirely, and the symptom is intermittent.

**9. Kernel messages.** `dmesg -T | tail -50` on the LB and the real. Look for driver XDP
errors, `ipip` messages, and martian-source logs (enable with
`sysctl -w net.ipv4.conf.all.log_martians=1`). On a physical NIC, `ethtool -S <if> | grep
-i xdp` shows driver-level XDP transmit errors, which PacketBalance's counters cannot see.
On a veth, native `XDP_TX` frames are dropped silently if the peer is not running NAPI.
The lab enables GRO on `pb-lb1` and `pb-lb2` for this reason. Check with
`ethtool -k pb-lb1 | grep generic-receive-offload`.

## Diagnose: one real gets no traffic

The symptom is that `pb_real_packets_total` for one real is flat, or the real's own logs show no
requests, while the others are busy.

1. **Is it down?** `pbctl health`. `up: false` with a rising `consecutive_fail` means
   the health check is failing. Try the same check by hand from the LB,
   `nc -vz -w1 10.0.0.23 80`. If that works and the daemon disagrees, look at the log for
   the check's error detail.
2. **Is its weight 0 or is it draining?** `pbctl vip list`. A real left draining after
   maintenance looks exactly like this.
3. **Is its MAC unresolved?** A real whose MAC the daemon never resolved gets no ring
   slots. `journalctl -u packetbalance | grep 'neigh: no ARP entry'`, and
   `bpftool map dump pinned /sys/fs/bpf/packetbalance/neigh` (all zeros for its
   `real_id`). Check `ip neigh show 10.0.0.23` on the LB and whether the real answers
   ARP at all.
4. **Does the ring agree?** `pbctl ring show 198.51.100.1:80/tcp`. Zero slots for the real
   confirms one of the three causes above. A normal slot count means the ring is fine and
   the problem is further along.
5. **Flow-level check.** `pbctl flows --vip 198.51.100.1:80/tcp --real 10.0.0.23` lists
   flows the table has assigned to it. Flows listed but the real sees nothing means the
   LB is sending and the real is not receiving. Go to steps 6 and 7 of the black-hole
   checklist, run on that real only. The symptom of a single broken real from the
   client's side is that about 1/N of new connections hang.
6. **Only some LBs?** Run the same checks on every LB. One LB with the real down (its own
   health view) and another with it up is the "rings disagree" case, which breaks flows
   that move between them.

## Diagnose: connections drop on every deploy

The symptom is that every time something is deployed (an LB, the reals, or a config change), a burst
of clients see resets or timeouts. First decide which deploy.

**A load balancer deploy.**

- **Is conntrack on?** `pbctl config` (`conntrack`) and `pbctl vip list` (per-VIP
  `no_conntrack` flag). Without it, every ring change breaks flows.
- **Did the deploy recreate the maps?** `journalctl -u packetbalance | grep -i recreate`.
  A roll that changed a map loses the connection table. Combined with any ring change,
  that breaks flows.
- **Did the XDP mode change?** Look for `(not hitless)` in the log.
- **Does the unit detach on exit?** `systemctl cat packetbalance | grep detach`.
  `--detach-on-exit` in `ExecStart` turns every restart into an outage.
- **Did the ring change during the deploy?** Compare `pb_ring_generation` before and
  after. A restart with unchanged config rebuilds the ring once, identically. Several
  increments mean something else changed.

**A backend deploy.**

- **Are reals removed with `real del` instead of drained?** That breaks every connection
  on them by design. Drain first.
- **Is health flapping?** A backend deploy that restarts the service makes health checks
  fail. Look at the transitions:

  ```sh
  journalctl -u packetbalance --since -1h | grep -E 'UP->DOWN|DOWN->UP|ring:'
  ```

  Each transition is a ring change. Transitions for many reals at once, or the same real
  going down and up repeatedly, point at the thresholds. With `fall: 3`,
  `interval_ms: 1000`, a service restart shorter than about 2.5 s should not take a real
  out at all. If it does, the service takes longer to start than the health check
  tolerates. Raising `fall` delays detection of real failures by the same amount, so
  drain before restarting a service rather than tuning the checker around it.
- **Did every real go down at once?** A rolling deploy that is not rolling empties the
  ring. `pb_drops_total{reason="no_real"}` spikes.

**Either.**

- **Hash mode.** `pbctl ring show` prints `"hash": "modulo"` if the baseline hash was
  left configured. Under modulo, any real change moves almost every flow.
- **Is the table big enough?** `pb_conntrack_entries` near the configured size, and
  `pb_conntrack_misses_total` rising for established traffic, means the LRU is evicting
  live flows. They then depend on the ring and break on every ring change. Resize (which
  recreates the map, so do it out of ECMP).
- **Do the LBs agree?** Run `pbctl ring show` on each LB and compare slot counts per real.
  Different counts mean different rings, and any ECMP reshuffle during the deploy moves
  flows onto the wrong real.

## Diagnose: MTU black hole for large requests

The symptom is that small requests work and large uploads or large TLS ClientHellos hang after the
handshake. Responses are not affected by the LB's encapsulation (they do not pass through
it), but see the ICMP note below.

1. **Where is the big packet lost?** A 1,500-byte client packet becomes a 1,520-byte IPIP
   packet. On the LB's link peer (`tcpdump -ni pb-lb1 -e 'ip proto 4 and greater 1500'`)
   look for encapsulated frames above the path MTU toward the reals, then check the real's
   receive drops (`ip -s link show dev <nic>`) or the switch's.
2. **Did the LB drop it?** `pbctl stats` `drop_mtu` counts only packets above 3,500 bytes.
   Packets between the path MTU and 3,500 are sent and dropped downstream, uncounted by
   PacketBalance.
3. **Fix the path or the MSS.** Either raise the MTU of every link between the LBs and the
   reals by at least 20 bytes, or make clients send smaller segments. On the reals,
   `ip route change <route> advmss 1440` lowers the MSS they advertise. The lab instead
   puts `mtu 1480` on the client's route to the VIP (`ip route get 198.51.100.1` from the
   client shows it). UDP services must keep datagrams at least 20 bytes under the path MTU
   themselves.
4. **Large responses stall instead.** That is PMTU discovery on the return path. The router
   that cannot forward the real's large reply sends ICMP "fragmentation needed" to the VIP,
   which arrives at the LB. With the PMTU feature flag on (`PB_CFG_F_ICMP_PMTU` in the
   `config` map, visible in `bpftool map dump pinned .../config`), the LB forwards it to
   the real that owns the flow and `icmp_pmtu_fwd` in `pbctl stats` counts it. With the
   flag off, the ICMP is passed to the LB host, which ignores it, and the real never
   lowers its MTU.

## Metrics

Served on `127.0.0.1:9101/metrics` in Prometheus text format, summed across CPUs.

| Metric | Meaning |
|---|---|
| `pb_packets_total{vip}` | Packets that matched the VIP |
| `pb_bytes_total{vip}` | Their bytes, before encapsulation |
| `pb_tx_total{vip}` | Packets encapsulated and transmitted. Should track `pb_packets_total` minus drops |
| `pb_syn_total{vip}` | Bare SYNs, i.e. new TCP connections |
| `pb_conntrack_hits_total{vip}` | Packets routed from the connection table |
| `pb_conntrack_misses_total{vip}` | Non-SYN packets that missed the table and fell through to the ring |
| `pb_hash_total{vip}` | Packets routed by the ring (SYNs, misses, and everything when conntrack is off) |
| `pb_pass_total` | Packets handed to the host stack (not for a VIP) |
| `pb_drops_total{vip,reason}` | Drops by reason, table above. `frag`, `opts`, `short` and `other` are counted before the VIP is known |
| `pb_real_packets_total{real}` | Packets sent to each real, across VIPs |
| `pb_real_up{vip,real}` | 1 if the health checker considers the real up |
| `pb_real_weight{vip,real}` | Configured weight, 0 while draining |
| `pb_ring_generation{vip}` | Increments on every ring swap |
| `pb_conntrack_entries` | Entries in the connection table |

Things worth knowing when reading them. `pb_conntrack_misses_total` is not an error
count. A second LB taking over flows, a flow changing CPU, and LRU eviction all show up
there, and all are handled by the ring. It becomes a problem only when it coincides with
ring changes. `pb_real_up` reflects this LB's health view only.

### Example alert rules

Thresholds are starting points. Tune them against the service's normal traffic.

```yaml
groups:
  - name: packetbalance
    rules:
      - alert: PBDaemonDown
        # Forwarding continues, but rings are frozen and health checks stop.
        expr: up{job="packetbalance"} == 0
        for: 1m
      - alert: PBNoRealDrops
        expr: sum by (instance, vip) (rate(pb_drops_total{reason="no_real"}[5m])) > 0
        for: 2m
      - alert: PBDropRatio
        expr: |
          sum by (instance) (rate(pb_drops_total[5m]))
            / clamp_min(sum by (instance) (rate(pb_packets_total[5m])), 1) > 0.001
        for: 10m
      - alert: PBRealDown
        expr: pb_real_up == 0 and pb_real_weight > 0
        for: 5m
      - alert: PBAllRealsDown
        expr: sum by (instance, vip) (pb_real_up) == 0
        for: 30s
      - alert: PBRingChurn
        # Health flapping or a config loop. Every change is a chance to break flows.
        expr: changes(pb_ring_generation[15m]) > 6
      - alert: PBNotForwarding
        expr: sum by (instance, vip) (rate(pb_packets_total[5m])) > 0
          and sum by (instance, vip) (rate(pb_tx_total[5m])) == 0
        for: 2m
      - alert: PBConntrackNearFull
        # Replace 1048576 with the configured conntrack size.
        expr: pb_conntrack_entries / 1048576 > 0.9
        for: 15m
      - alert: PBRingsDisagree
        # Same VIP, different ring contents across the tier.
        expr: count by (vip, real) (count_values by (vip, real) ("w", pb_real_weight * pb_real_up)) > 1
        for: 5m
```
