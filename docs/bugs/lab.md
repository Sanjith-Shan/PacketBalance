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
