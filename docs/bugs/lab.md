# Lab bugs

Real defects hit while building the lab and the IPVS baseline (`lab/`,
`tools/conncheck`). Each entry: symptom, how it was found, cause, fix. Lab VM:
Ubuntu 24.04 aarch64, kernel 6.8.0-139-generic, Lima vz on an Apple M3 Pro.

## `ipvsadm --stats` reads zero right after traffic, then lags by up to 2 s

**Symptom.** Right after 20 HTTP requests through IPVS in lb1, `ipvsadm -Ln
--stats` showed `Conns 0 InPkts 0` on every service, while `ipvsadm -Lnc`
listed the connections and `tcpdump` on real1 showed the forwarded packets. A
few seconds later the same command showed `Conns 20 InPkts 120`.

**Found with.** `ipvsadm -Ln --stats --exact` against `ipvsadm -Lnc` and
`tcpdump -ni veth0` on real1; `ps` showed an `ipvs-e:10:0` kernel thread.

**Cause.** Since kernel 6.2 IPVS keeps per-CPU packet counters and folds them
into the totals that `ipvsadm` reports only when the estimator kthread
(`ipvs-e:*`) visits the service, about every 2 s. A read therefore returns
the value at the last estimator pass. For `lab/measure.sh`, which computes
forwarded pps as a counter delta over a 30 s window, both ends could be stale by
different amounts, an error of up to about 7% per window.

**Fix.** `lab/measure.sh` samples the IPVS counter by polling until the reported
value changes, and timestamps that moment, at both ends of the window. The
forwarded rate is the delta over the time between those two estimator ticks
(`forwarded_window_s` on the row), not over the pktgen window. On a check run
the IPVS forwarded rate then matched packets received at the reals within 3%.

## IPVS `mh` put 91% of the connections on one real

**Symptom.** With `ipvsadm -A -t 198.51.100.1:7000 -s mh`, 500 conncheck
connections landed 453 on real3 and 18, 13 and 16 on the others.

**Found with.** conncheck's `backends_at_start` (each connection records the
id of the real that answers it).

**Cause.** The `mh` scheduler hashes only the source address unless the service
has the `mh-port` flag. Every lab connection comes from the client's single
address, 10.0.0.10, so they all map to one real (the few on the others reused
source ports that still had IPVS connection entries from an earlier run). A
Maglev baseline configured like that would have looked like a broken hash in
Experiments 1, 3 and 4.

**Fix.** `lab/ipvs.sh` always creates `mh` services with `-b mh-port`. With it,
10,000 connections split 2443 / 2574 / 2473 / 2510.

## Every connection reset on IPVS failover, even with Maglev (`mh`)

**Symptom.** Experiment 4 through IPVS `mh`: after the client's route moved
from lb1 to lb2 (same services, same reals, same scheduler), 10,000 of 10,000
connections broke, all with `rst`. Maglev should have sent each flow to the
same real.

**Found with.** conncheck's `broken_by_cause`, then `tcpdump -ni veth0
'tcp[tcpflags] & tcp-rst != 0'` in lb2, which showed the resets leaving lb2
itself with source 198.51.100.1:7000.

**Cause.** IPVS only schedules a new connection from a SYN. lb2 had never seen
these flows, so their ACKs matched no IPVS connection entry and IPVS passed them
to lb2's own stack. The director owns the VIP on `lo` (required for DR), nothing
listens there on port 7000, and the stack answered each packet with a RST.

**Fix.** `net.ipv4.vs.sloppy_tcp=1` on the directors (the default in
`lab/ipvs.sh`, set the same for `rr` and `mh`), which lets IPVS create a
connection entry from a mid-stream packet. With it, `mh` failover broke 0 of
10,000 connections. Experiment 4 records rows with both settings
(`ipvs_sloppy_tcp`), because the 0 rows show that a stateless LB tier needs
more than consistent hashing.

## nginx on the reals was capped at 1024 file descriptors

**Symptom.** `lab/up.sh` printed `16384 worker_connections exceed open file
resource limit: 1024` for every real.

**Found with.** up.sh's own output.

**Cause.** Processes started through `sudo` and `ip netns exec` inherit a soft
`RLIMIT_NOFILE` of 1024. With `wrk -c256` and 10,000 conncheck connections, an
fd cap on the reals would have shown up as refused or reset connections and been
blamed on the load balancer.

**Fix.** up.sh raises `ulimit -n` before it starts anything, and each nginx
config sets `worker_rlimit_nofile 65535`. conncheck and conncheck-server raise
their own limit.

## The daemon could not pin its maps: `ip netns exec` hides /sys/fs/bpf (found by CI)

**Symptom.** In the CI e2e job, `packetbalance` started with `ip netns exec lb1`
failed at load with libbpf's `mkdir /sys/fs/bpf/packetbalance/lb1/config: No
such file or directory`, although bpffs was mounted at /sys/fs/bpf on the host.

**Found with.** The CI log, then in the VM: `ip netns exec lb1 stat -f -c %T
/sys/fs/bpf` prints `sysfs`, not `bpf_fs`.

**Cause.** `ip netns exec` runs the command in a private mount namespace and
mounts a fresh sysfs on /sys so /sys/class/net shows the namespace's devices.
That new sysfs hides everything mounted below the host's /sys, including the
bpffs at /sys/fs/bpf. The pins the daemon wanted to create (and the ones a
restarted daemon must find again) were on a filesystem the daemon could not see.

**Fix.** The lab mounts its own bpffs at /run/pblab/bpf (`lab/up.sh`; `down.sh`
removes the pins and unmounts it). /run is not remounted by `ip netns exec`, so
the mount, and every pin in it, is visible to each daemon and survives daemon
restarts. The daemons get `--pin-path /run/pblab/bpf/lb1` (and lb2), and the
daemon now checks that its pin path is on bpffs with a clear error.

## Native XDP_TX on a veth: frames "sent" and never seen again

**Symptom.** With PacketBalance attached in native mode to veth0 in lb1, every
`curl http://198.51.100.1/` timed out. `pbctl stats` counted the SYNs as `tx`
and `ethtool -S veth0` in lb1 showed `rx_queue_N_xdp_tx` rising, but `tcpdump`
on the peer, pb-lb1, and on real1 saw no IPIP frame at all.

**Found with.** `bpftrace` on `tracepoint:xdp:xdp_bulk_tx` (each XDP_TX flush
reported `sent=1 drops=0 err=0`), `tcpdump -eni pb-lb1 'ip proto 4'` (nothing),
and `tracepoint:skb:kfree_skb` (no drop: the frames never became skbs).

**Cause.** XDP_TX on a veth does not go through a transmit queue: the frame is
put on the peer's XDP ring, which is drained by the peer's NAPI poll. The peer
(pb-lb1, on the bridge) had no XDP program, and on this 6.8 kernel enabling GRO
on it (the documented alternative that switches on veth NAPI) did not make the
frames appear either, including after toggling GRO off and on.

**Fix.** `lab/up.sh` attaches a do-nothing XDP_PASS program
(`lab/xdp_pass.bpf.c`) to pb-lb1 and pb-lb2. With it the IPIP frames appear on
pb-lb1 and reach the reals. It stays attached for IPVS runs too, so both
forwarding planes see the same peer path.

## Encapsulated SYNs dropped by the real as bad checksums

**Symptom.** After the fix above, the IPIP frames reached real1 and `tunl0`'s
RX counter rose, but there was still no SYN-ACK.

**Found with.** `nstat` in real1: `TcpInCsumErrors` rose with every attempt.

**Cause.** The client's veth has TX checksum offload on, so its TCP stack hands
the veth a CHECKSUM_PARTIAL skb with the checksum field not yet computed. On a
veth-to-veth path the receiving stack would trust the skb's checksum state,
but native XDP sees only the raw bytes, encapsulates them, and the metadata that
said "checksum still to do" is lost. The real decapsulates and verifies a
checksum nobody ever computed. A physical NIC always puts a finished checksum on
the wire, so this is a lab artifact, not a data-plane bug.

**Fix.** `lab/up.sh` turns off TX checksum offload on the client's veth0
(`ethtool -K veth0 tx off`), so the client sends what a real NIC would. It is set
for every run, IPVS included.

## UDP echo answered from the wrong address under DSR

**Symptom.** `echo hi | socat - UDP:198.51.100.1:5000` from the client printed
nothing, through PacketBalance and through IPVS, while the request reached the
real.

**Found with.** socat in the client namespace against the reals' echo server.

**Cause.** The echo server's socket is bound to 0.0.0.0 and unconnected, so the
kernel chose the reply's source address by routing: the real's 10.0.0.2x, not
the VIP. The client's connected socket drops a reply that does not come from the
address it sent to. This is the UDP half of DSR that TCP gets for free (an
accepted TCP socket is bound to the address the SYN was sent to).

**Fix.** `lab/udp_echo.py` reads the destination address with `IP_PKTINFO` and
replies from it.

## The committed IPVS baseline measured 121 k, 1.04 M and 2.06 M pps for one configuration

**Symptom.** The three Exp 1 repeats of IPVS `mh` (DR) in the first baseline
run gave 1.04 M, 2.11 M and 124 k forwarded pps; `rr` 1.93 M, 2.38 M, 132 k;
the no-LB ceiling 5.94 M, 1.12 M, 5.94 M. A 20x spread inside one configuration.

**Found with.** The per-row `offered_pps` (pktgen itself slowed down, so the
generator, not the LB, lost speed), then a CPU canary: a fixed single-threaded
Python loop pinned to one vCPU ran 7 to 12 M iterations/s while the Mac had a
video call and other apps running, and 15 to 18 M/s after a host reboot. The
VM's vCPUs are host threads; on Apple silicon they may also land on efficiency
cores. Nothing inside the VM shows that.

**Cause.** Host contention outside the VM. The harness had no way to see it.

**Fix.** `lab/common.sh host_gate`: before every packet-rate window (and every
Exp 2 wrk run) wait until no compiler or build runs in the VM and the canary is
at least `CANARY_MIN` (12 M/s); take the canary again after the window; a
window that fails goes to `results/rejected.jsonl` and is retried
(`MEASURE_TRIES`). The canary and the gate result are on every row. The
pre-guard Exp 1, sweep and Exp 2 baseline rows moved to `results/superseded/`
and every configuration was measured again with the guard. The spread of the
new IPVS `mh` rows is 2.10 to 2.28 M.

## PacketBalance's `tx` counter said 1.4 M pps forwarded; 0.6 M arrived

**Symptom.** Exp 1, native XDP: `pbctl stats` `tx` rose by 1.39 to 1.66 M/s, the
reals received 0.55 to 0.60 M/s. Generic: 2.5 M/s counted, 1.4 M/s received.
IPVS's InPkts and the reals' receive count agreed within 3%.

**Found with.** A packet accounting added to `lab/measure.sh` (the
`netdev_delta` and `packet_accounting` row fields): rx/tx packets and dropped
of every device on the path, `ethtool -S` on lb1's veth0 and on its peer
pb-lb1, and /proc/net/softnet_stat, all as deltas over the window.

**Cause.** The program's `tx` counts XDP_TX verdicts. On a veth, native XDP_TX
puts the frame on the peer's (pb-lb1's) 256-entry ptr_ring, and when the ring
is full `veth_xdp_xmit` drops it; the drop is visible only as lb1 veth0
`rx_queue_N_xdp_tx_errors` (and `tx_dropped`, and pb-lb1 `rx_dropped`, the same
event three times). In generic mode XDP_TX goes through the veth's normal
transmit and shows as lb1 veth0 `tx_dropped`. At saturation, per 30 s window
(the two `standard` rows of `results/exp1_mitigations.jsonl`): about 141 M offered,
about 100 M dropped before the program (lb1's own receive ring full, pb-lb1
`tx_dropped`), about 40.5 M XDP_TX'd, about 21 M lost to the full pb-lb1 ring, about
19.5 M received, 9 to 15 k dropped by the reals' backlog, under 0.1% of offered
unaccounted for (timing skew). The program's
own drop counters were 0.

**Fix.** The harness, not the program: every packet-rate row carries
`received_pps` (what arrived) next to `forwarded_pps` (the LB's own count), the
driver counters and the accounting, and `lab/render_tables.py` reports received
pps, and pps per core computed from it, as the headline. The `tx` counter's
meaning ("XDP_TX verdicts, not frames delivered") is a documentation item for
docs/API.md (routed to the owner).

## `down.sh` stopped half way: "Cannot find device pb-real4"

**Symptom.** `sudo lab/down.sh` printed `Cannot find device "pb-real4"` and
exited, leaving br0, the bpffs mount and /run/pblab behind. up.sh's own
down.sh call then cleaned up, so it went unnoticed.

**Found with.** down.sh's output.

**Cause.** The list of pb-* devices is taken after `ip netns del`, which removes
the in-namespace ends of the veth pairs asynchronously; the root-side peer can
vanish between the listing and the `ip link del`, and `set -e` aborted the script.

**Fix.** `ip link del` of a pb-* veth tolerates a device that is already gone.

## The MTU check could not fail: DSR sends the big direction around the LB

**Symptom.** A `curl` of a 100 KB file through the VIP completed with and
without the client route's `mtu 1480`.

**Found with.** tcpdump on the reals: the encapsulated packets were all
small (SYN, ACKs, the GET). The 100 KB response goes real -> client directly.

**Cause.** Under DSR only the client-to-real direction is encapsulated, so a
download never puts a full-size segment through the LB.

**Fix.** The reals' nginx accepts `PUT /upload/` (dav_methods), and
`lab/mtucheck.sh` uploads 1 MB through the VIP. With the route mtu the largest
encapsulated TCP payload is 1428 bytes (1500-byte outer packet) and all
uploads complete in both XDP modes; with `--no-route-mtu` every upload stalls
at 64 KB and times out (the PMTU black hole in docs/DESIGN.md), with
`drop_mtu` 0 because the 1520-byte frames are dropped by the veth, not the program.
