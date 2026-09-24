# Bug log

Every entry below is a bug actually hit while building PacketBalance, with the tool or evidence that found it; nothing here is hypothetical.
Unless an entry says otherwise, the environment was the Lima lab VM (Ubuntu 24.04 aarch64 on an Apple M3 Pro, kernel 6.8.0-139, clang 18, libbpf 1.3.0, bpftrace 0.20.2); the per-area sources are in `docs/bugs/`.

## Index

| # | Bug | Area | Found with |
|---|-----|------|------------|
| 1 | Verifier rejects packet math after `bpf_ntohs(iph->tot_len)` | Data plane | libbpf verifier log |
| 2 | A per-CPU conntrack value zero-filled by another CPU reads as a hit on real 0 | Data plane | Reading `kernel/bpf/hashtab.c`, then a CPU-pinned `BPF_PROG_TEST_RUN` test |
| 3 | Conntrack tests flaked because the test thread migrated between CPUs | Data plane | Test binary in a loop, failures counted with `uniq -c` |
| 4 | bpftrace filter on the program name never matched | Data plane | bpftrace against `fexit:vmlinux:bpf_prog_test_run_xdp` |
| 5 | bpftrace 0.20 generated a program the verifier rejects | Data plane | `bpftrace -v` verifier log |
| 6 | bpftrace printed the `adjust_head` delta as 4294967276 | Data plane | First run of `xdp_adjust_head.bt` |
| 7 | `hash.h` comment claimed the flow hash is the kernel's `jhash_3words` | Data plane | Reading it against `include/linux/jhash.h` |
| 8 | A re-added real got no ring slots for up to a second | Control plane | Ring dump with `bpftool map dump`, then the daemon log |
| 9 | Restart could briefly publish an empty ring for a live VIP | Control plane | Code review, confirmed by the restart test log |
| 10 | Restart logs claimed things that were no longer true | Control plane | `bpftool map show pinned` next to the daemon log |
| 11 | `VipSpec::parse` accepted trailing junk and signs in the port | Control plane | Probing the parser while writing `vipspec_test.cpp` |
| 12 | `ipvsadm --stats` reads zero right after traffic, then lags by up to 2 s | Lab and baselines | `ipvsadm -Ln --stats` against `ipvsadm -Lnc` and `tcpdump` |
| 13 | IPVS `mh` put 91% of the connections on one real | Lab and baselines | conncheck `backends_at_start` |
| 14 | Every connection reset on IPVS failover, even with `mh` | Lab and baselines | conncheck `broken_by_cause`, then `tcpdump` for RSTs in lb2 |
| 15 | nginx on the reals was capped at 1024 file descriptors | Lab and baselines | `lab/up.sh` output |
| 16 | `__array(values, ...)` not last in the `rings` map definition | Build and CI | clang `-Werror` |
| 17 | `clang -target bpf` could not find `<asm/types.h>` | Build and CI | First build of `pb_bpf_obj` |
| 18 | Daemon could not create its pin path under `ip netns exec` in CI | Build and CI | GitHub Actions log, then `stat -f -c %T` |
| 19 | The data plane could transmit a frame to MAC 00:00:00:00:00:00 | Data plane | End-to-end code review, then a `BPF_PROG_TEST_RUN` test |
| 20 | Freeing a real cleared its MAC before its address | Control plane | Code review of `real_table.cpp` write order |
| 21 | Running out of file descriptors marked healthy reals DOWN | Control plane | Code review of the health checker's per-round socket count |
| 22 | Native XDP_TX on a veth: frames "sent" and never seen again | Lab and baselines | `bpftrace` on `xdp:xdp_bulk_tx`, `tcpdump` on the peer |
| 23 | Encapsulated SYNs dropped by the real as bad checksums | Lab and baselines | `nstat` `TcpInCsumErrors` in the real |
| 24 | UDP echo answered from the wrong address under DSR | Lab and baselines | socat in the client namespace |
| 25 | The IPVS baseline measured 121 k, 1.04 M and 2.06 M pps for one configuration | Lab and baselines | Per-row `offered_pps`, then a CPU canary |
| 26 | PacketBalance's `tx` counter said 1.5 M pps forwarded; 0.58 M arrived | Lab and baselines | Packet accounting over every device, `ethtool -S` |
| 27 | `down.sh` stopped half way: "Cannot find device pb-real4" | Lab and baselines | `down.sh` output |
| 28 | The MTU check could not fail: DSR sends the big direction around the LB | Lab and baselines | `tcpdump` on the reals |

## Data plane

These came from the XDP program (`bpf/packetbalance.bpf.c`), its `BPF_PROG_TEST_RUN` tests (`tests/dataplane/`), the bpftrace scripts (`tools/bpftrace/`) and the shared flow hash.

### 1. Verifier rejects packet math after `bpf_ntohs(iph->tot_len)`

**Symptom.** The skeleton failed to load with `-EINVAL`, and every data-plane test failed in `SetUp`.

**Found with.** The libbpf verifier log that the test printed:

```
103: (69) r7 = *(u16 *)(r6 +16)
104: (dc) r7 = be16 r7                ; R7_w=scalar()
106: (2d) if r3 > r7 goto pc+2        ; R7_w=scalar(umin=20)
107: (0f) r2 += r7
math between pkt pointer and register with unbounded min value is not allowed
```

**Cause.** The 16-bit load is bounded to `0xffff`, but the 6.8 verifier drops all bounds across the `be16` byte-swap instruction, so `tot_len` becomes an unbounded scalar. The truncation check `(void *)iph + tot_len > data_end` then adds an unbounded register to a packet pointer, which the verifier refuses. clang knows that a byte-swapped `__u16` fits in 16 bits, so a plain `if (tot_len > 0xffff)` is deleted as dead code and does not help.

**Fix.** `asm volatile("" : "+r"(tot_len));` hides the value from the optimizer, and an explicit `if (tot_len > 0xffff) drop` after it gives the verifier the bound. The program loads with 2,763 verified instructions.

### 2. A per-CPU conntrack value zero-filled by another CPU reads as a hit on real 0

**Symptom.** A flow whose SYN was handled on CPU A and whose next packet was handled on CPU B was sent to `real_id 0` (10.0.0.20 in the test), a real the ring never chose.

**Found with.** Reading `pcpu_init_value()` in `kernel/bpf/hashtab.c`: when a BPF program inserts into a `PERCPU` hash, the kernel zero-fills the value on every other CPU, so the key exists for every CPU. The guard went in with the first version of the program. The failure was then demonstrated by the test `OtherCpuZeroValueIsAMissNotRealZero`, which pins the thread to CPU 0 for the SYN and CPU 1 for the ACK. With the guard removed the test fails: the outer `daddr` is 10.0.0.20 (real 0) instead of 10.0.0.21, and the table records a hit, not a miss.

**Cause.** `LRU_PERCPU_HASH` shares keys across CPUs and keeps values per CPU. A lookup on CPU B returns B's all-zero value, and `real_id 0` is a valid id.

**Fix.** A value with `last_seen_ns == 0` counts as a miss (`bpf_ktime_get_ns()` is never 0 after boot). The flow falls through to the hash, which picks the same real CPU A picked unless the ring changed. Anything that walks `conntrack` from userspace (`pbctl flows`) must also skip per-CPU values with `last_seen_ns == 0`.

### 3. Conntrack tests flaked because the test thread migrated between CPUs

**Symptom.** Out of 6 back-to-back runs of `dataplane_test`, 3 had failures, each time in a different test: `ConntrackPinsFlowAcrossRingSwap`, `IcmpFragNeededForwardedToFlowOwner` or `RstForUnknownFlowForwardedWithoutState`. The failing packet went to the ring's current real instead of the pinned one, and `PB_CNT_CT_HIT` was 0.

**Found with.** Running the test binary in a loop and counting failures by test name with `uniq -c`.

**Cause.** `BPF_PROG_TEST_RUN` runs the program on the calling CPU. When the scheduler moved the test thread between two packets of the same flow, the second packet saw the zero-filled value from bug 2. The program was correct (a miss, then a hash) and the test's assumption was wrong. On a real NIC one flow stays on one RX queue.

**Fix.** The test fixture pins itself to one CPU in `SetUp` and restores its affinity in `TearDown`. The cross-CPU case has its own test, which pins explicitly. After the fix, 20 of 20 runs passed.

### 4. bpftrace filter on the program name never matched

**Symptom.** `xdp_action_histogram.bt` filtered with `args->xdp_prog->aux->name == "xdp_packetbal"`. It would have printed nothing, forever.

**Found with.** Checking the filter, before trusting it, against a probe that does fire under test runs, `fexit:vmlinux:bpf_prog_test_run_xdp`: `@eq[0]: 2811` for `==` against `@ncmp[1]: 2811` for `strncmp(..., 13)`.

**Cause.** `aux->name` is `char[16]` and holds the name truncated to 15 characters, `xdp_packetbalan` (`bpftool prog show` prints the same). `==` compares whole strings, so a prefix never matches.

**Fix.** The filter is `strncmp(args->xdp_prog->aux->name, "xdp_packetbal", 13) == 0`.

### 5. bpftrace 0.20 generated a program the verifier rejects

**Symptom.** `ERROR: Error loading program: tracepoint:xdp:xdp_exception`.

**Found with.** `bpftrace -v`, which printed the verifier log. It ended at `4: (7b) *(u64 *)(r10 -53) = r1`, a misaligned 8-byte stack store.

**Cause.** A bpftrace 0.20 code generation problem: the map key was a tuple that mixed integers with a string (`@name[act]`), and bpftrace laid it out unaligned on the stack.

**Fix.** The key is now all numeric, `(prog_id, act, ifindex)`, and the action numbers are listed in a comment.

### 6. bpftrace printed the `adjust_head` delta as 4294967276

**Symptom.** `@delta[4294967276]: 4321` where `-20` was expected.

**Found with.** The first run of `xdp_adjust_head.bt` against test-run traffic.

**Cause.** The helper's `int delta` arrives zero-extended in a 64-bit register, and casting it to `int64` keeps it positive.

**Fix.** Cast to `int32` for the argument and for the return value. The script now prints `@delta[-20]`.

### 7. `hash.h` comment claimed the flow hash is the kernel's `jhash_3words`

**Symptom.** The header described the flow hash as "Bob Jenkins' lookup3 jhash as the Linux kernel uses it", but its values differ from the kernel's. The kernel's `jhash_3words(a, b, c, initval)` adds `initval + JHASH_INITVAL + (3 << 2)` to all three words before the final mix; `pb_jhash_3words` adds `JHASH_INITVAL` to `a` and `b` and `initval` to `c`. The final mix is the same and the pre-mix is not.

**Found with.** Reading `include/packetbalance/hash.h` against the kernel's `include/linux/jhash.h`. `tests/core/hash_test.cpp` pins the current values.

**Cause.** A documentation error, harmless for correctness because the XDP program and the control plane both include `hash.h` and never call the kernel's jhash. It was still a trap: anyone who swapped in the kernel function "because the comment says it is the same" would have broken lb1 and lb2 agreement.

**Fix.** Fixed. The comment was corrected to say the function is not bit-identical to the kernel's `jhash_3words` and why. The function itself was left alone, because changing it would reshuffle every ring.

### 19. The data plane could transmit a frame to MAC 00:00:00:00:00:00

**Symptom.** Not seen on the wire in the lab. Found by reading `bpf/packetbalance.bpf.c`: after a conntrack hit the program looked up `neigh[real_id]` and tested the pointer for NULL. `neigh` is a `BPF_MAP_TYPE_ARRAY`, so the lookup never returns NULL for an in-range index, and a slot whose MAC had not been written (a reused `real_id` whose new MAC was not resolved yet, or a slot in the middle of being freed) produced a frame with an all-zero destination MAC and no counter recorded it. `docs/DESIGN.md` already claimed this case was dropped.

**Found with.** The end-to-end review, then a new test `Dataplane.UnresolvedMacDropsNoReal` that populates a real with no MAC and asserts `XDP_DROP` and the `no_real` counter.

**Cause.** An array lookup was treated like a hash lookup. The absence check has to be on the value, not the pointer.

**Fix.** Fixed. The program drops and counts `no_real` when the six MAC bytes are all zero. The verifier still accepts the program, at 2841 instructions.

## Control plane

### 20. Freeing a real cleared its MAC before its address

**Symptom.** Not seen at runtime. `RealTable::write_slot` cleared `neigh[id]` first and `reals[id]` second when a real was deleted. Between the two writes a conntrack entry naming that `real_id` would have read a non-zero address and an all-zero MAC, which is exactly the window bug 19 makes visible.

**Found with.** Reading `src/daemon/real_table.cpp` while checking bug 19's reachable paths.

**Cause.** Both writes are individually correct; only the order between them was wrong for a reader that checks the address first.

**Fix.** Fixed. A freed slot now clears `reals` first so a stale conntrack hit becomes a miss (and rehashes) before the MAC disappears. A new slot still writes `neigh` before `reals`.

### 21. Running out of file descriptors marked healthy reals DOWN

**Symptom.** Not seen in the four-real lab. One health-check round opens a socket for every (TCP VIP, real) pair at once, up to 64 VIPs by 511 reals. Past the default soft limit of 1024 open files, `socket()` fails with `EMFILE`, and the checker counted each failure against the real it was about to probe. With enough VIPs the daemon would have marked healthy reals down and pulled them out of the ring.

**Found with.** Reading `src/daemon/health_checker.cpp` for how a probe failure is classified.

**Cause.** The checker did not distinguish "the real refused" from "the load balancer could not open a socket".

**Fix.** Fixed. Errors that describe the load balancer itself (`EMFILE`, `ENFILE`, `ENOBUFS`, `ENOMEM`, `EADDRNOTAVAIL`, `EAGAIN`) set `ProbeResult::local_error`; `LbState::report_health` ignores those results and logs one warning per round; `main.cpp` raises the file-descriptor soft limit to the hard limit at startup. Test: `ControlPlane.LocalProbeErrorsDoNotCount`.


These came from `packetbalance` (the daemon) and the shared VIP parser, smoke-tested in the lab VM.

### 8. A re-added real got no ring slots for up to a second

**Symptom.** A script that measures ring disruption read the live inner ring map with `bpftool map lookup pinned .../rings key 0 0 0 0` (for the `inner_map_id`) and `bpftool map dump id N`, removed a real, and dumped again. For the second hash mode it reported that the removed real held 0 slots, although it had held a quarter of them a moment earlier.

**Found with.** That script's output (`removed real_id 2 held 0`), then the daemon log: after `reload` put the real back, the ring was built with `reals_in_ring=2/3`, and `neigh: real 10.77.0.3 -> ...` plus a second rebuild came about 100 ms later.

**Cause.** The ring leaves out any real whose destination MAC is unknown, since `XDP_TX` to a zero MAC would drop that real's share of traffic. The MAC lives in the real's slot in `RealTable`. Deleting the real's last VIP freed the slot and its MAC. Adding it back allocated a new slot with no MAC, and the ARP resolver only filled it on its next pass (up to 1 s), even though the kernel's ARP table still had the entry.

**Fix.** When a real is added, `LbState` looks the address (or the next hop) up in `/proc/net/arp` right away. That read is cheap and never probes. The resolver thread only handles addresses the kernel does not know yet. A re-added real now goes into the ring in the same swap that adds it.

### 9. Restart could briefly publish an empty ring for a live VIP

**Symptom.** In the first draft, VIPs came up in this order: allocate the vip_id, build and install a ring with no reals, write `vip_map`, then add the reals and rebuild. On a fresh start that is harmless. On a restart that reuses pinned maps, the VIP is already in `vip_map`, so for a few milliseconds new connections to it (SYNs, and conntrack misses) would hash into a ring of `PB_REAL_NONE` and be dropped as `drop_no_real`.

**Found with.** Code review of `create_vip_locked` while writing the adoption path, before the first restart test. After the fix, the restart test showed one ring build per VIP (`reason=startup (adopted)`, `reals_in_ring=3/3`) and the synthetic conntrack entry survived.

**Cause.** The ring was published before it held the VIP's reals. That is only safe while nothing can see the VIP yet.

**Fix.** `create_vip_locked` takes the initial reals and publishes one complete ring, then writes `vip_map`. Deletion runs in the reverse order (`vip_map` first, then the ring, then the real_ids). Removing a real works the same way: rebuild the ring without it, then free its real_id, so a packet can never hash to an id whose `reals` slot is already empty.

### 10. Restart logs claimed things that were no longer true

**Symptom.** A run with `--recreate-maps --xdp-mode generic`, against a daemon that had been running native with a 65536-entry table, logged two false lines: `attached ... in generic mode, replacing the previous program` (the native program had already been detached, so nothing was replaced) and `reusing pinned maps ...: tracked flows are preserved` (conntrack had just been unpinned and recreated empty).

**Found with.** Reading the smoke-test log next to `bpftool map show pinned /sys/fs/bpf/pbdev/conntrack`, which showed a new map id with `max_entries 1024`.

**Cause.** The "was attached" flag was captured before the mode-switch detach, and "reused pins" was set for any pinned file found, including the one the daemon then removed.

**Fix.** The flag is cleared after the detach, and a map counts as reused only when its definition matched. A new `reused_conntrack()` drives the "flows are preserved" message. An operator reads these lines during an incident, so they have to be exact.

### 11. `VipSpec::parse` accepted trailing junk and signs in the port

**Symptom.** `198.51.100.1:80x/tcp`, `198.51.100.1: 80/tcp` and `198.51.100.1:+80/tcp` all parsed as `198.51.100.1:80/tcp`, and the address parser accepted `1.2.3.-0`. A typo in a config file or a `pbctl` argument was silently accepted instead of rejected.

**Found with.** Probing the parser with malformed strings while writing `tests/core/vipspec_test.cpp`.

**Cause.** `include/packetbalance/vipspec.h` parsed the port with `std::stoi`, which skips leading whitespace, accepts a sign and stops at the first non-digit. `parse_ipv4` used `sscanf` with `%u`, which has the same leniency.

**Fix.** Fixed. The parser is now strict: exactly four decimal octets of digits only (no sign, no whitespace), and a port of ASCII digits only in the range 1 to 65535.

## Lab and baselines

These came from the lab (`lab/`) and the IPVS baseline, and from `tools/conncheck`. Most of them would have made a baseline look worse or better than it is.

### 12. `ipvsadm --stats` reads zero right after traffic, then lags by up to 2 s

**Symptom.** Right after 20 HTTP requests through IPVS in lb1, `ipvsadm -Ln --stats` showed `Conns 0 InPkts 0` on every service, while `ipvsadm -Lnc` listed the connections and `tcpdump` on real1 showed the forwarded packets. A few seconds later the same command showed `Conns 20 InPkts 120`.

**Found with.** `ipvsadm -Ln --stats --exact` against `ipvsadm -Lnc` and `tcpdump -ni veth0` on real1. `ps` showed an `ipvs-e:10:0` kernel thread.

**Cause.** Since kernel 6.2, IPVS keeps per-CPU packet counters and folds them into the totals that `ipvsadm` reports only when the estimator kthread (`ipvs-e:*`) visits the service, about every 2 s. A read therefore returns the value at the last estimator pass. `lab/measure.sh` computes forwarded pps as a counter delta over a 30 s window, and both ends could be stale by different amounts, an error of up to about 7% per window.

**Fix.** `lab/measure.sh` samples the IPVS counter by polling until the reported value changes, and timestamps that moment, at both ends of the window. The forwarded rate is the delta over the time between those two estimator ticks (`forwarded_window_s` on the row), not over the pktgen window. On a check run, the IPVS forwarded rate then matched the packets received at the reals within 3%.

### 13. IPVS `mh` put 91% of the connections on one real

**Symptom.** With `ipvsadm -A -t 198.51.100.1:7000 -s mh`, 500 conncheck connections landed 453 on real3 and 18, 13 and 16 on the others.

**Found with.** conncheck's `backends_at_start` field (each connection records the id of the real that answers it).

**Cause.** The `mh` scheduler hashes only the source address unless the service has the `mh-port` flag. Every lab connection comes from the client's single address, 10.0.0.10, so they all mapped to one real (the few on the others reused source ports that still had IPVS connection entries from an earlier run). A Maglev baseline configured like that would have looked like a broken hash in Experiments 1, 3 and 4.

**Fix.** `lab/ipvs.sh` always creates `mh` services with `-b mh-port`. With it, 10,000 connections split 2443 / 2574 / 2473 / 2510.

### 14. Every connection reset on IPVS failover, even with `mh`

**Symptom.** In Experiment 4 through IPVS `mh`, after the client's route moved from lb1 to lb2 (same services, same reals, same scheduler), 10,000 of 10,000 connections broke, all with `rst`. Maglev should have sent each flow to the same real.

**Found with.** conncheck's `broken_by_cause`, then `tcpdump -ni veth0 'tcp[tcpflags] & tcp-rst != 0'` in lb2, which showed the resets leaving lb2 itself with source 198.51.100.1:7000.

**Cause.** IPVS schedules a new connection only from a SYN. lb2 had never seen these flows, so their ACKs matched no IPVS connection entry and IPVS passed them to lb2's own stack. The director owns the VIP on `lo` (required for DR), nothing listens there on port 7000, and the stack answered each packet with a RST.

**Fix.** `net.ipv4.vs.sloppy_tcp=1` on the directors (the default in `lab/ipvs.sh`, set the same for `rr` and `mh`), which lets IPVS create a connection entry from a mid-stream packet. With it, `mh` failover broke 0 of 10,000 connections. Experiment 4 records rows with both settings (`ipvs_sloppy_tcp`), because the `ipvs_sloppy_tcp=0` rows show that a stateless LB tier needs more than consistent hashing.

### 15. nginx on the reals was capped at 1024 file descriptors

**Symptom.** `lab/up.sh` printed `16384 worker_connections exceed open file resource limit: 1024` for every real.

**Found with.** `lab/up.sh`'s own output.

**Cause.** Processes started through `sudo` and `ip netns exec` inherit a soft `RLIMIT_NOFILE` of 1024. With `wrk -c256` and 10,000 conncheck connections, an fd cap on the reals would have shown up as refused or reset connections and been blamed on the load balancer.

**Fix.** `lab/up.sh` raises `ulimit -n` before it starts anything, and each nginx config sets `worker_rlimit_nofile 65535`. conncheck and conncheck-server raise their own limit.

### 22. Native XDP_TX on a veth: frames "sent" and never seen again

**Symptom.** With PacketBalance attached in native mode to veth0 in lb1, every `curl http://198.51.100.1/` timed out. `pbctl stats` counted the SYNs as `tx` and `ethtool -S veth0` in lb1 showed `rx_queue_N_xdp_tx` rising, but `tcpdump` on the peer, pb-lb1, and on real1 saw no IPIP frame at all.

**Found with.** `bpftrace` on `tracepoint:xdp:xdp_bulk_tx` (each XDP_TX flush reported `sent=1 drops=0 err=0`), `tcpdump -eni pb-lb1 'ip proto 4'` (nothing), and `tracepoint:skb:kfree_skb` (no drop: the frames never became skbs).

**Cause.** XDP_TX on a veth does not go through a transmit queue: the frame is put on the peer's XDP ring, which is drained by the peer's NAPI poll. The peer (pb-lb1, on the bridge) had no XDP program, and on this 6.8 kernel enabling GRO on it (the documented alternative that switches on veth NAPI) did not make the frames appear either, including after toggling GRO off and on.

**Fix.** `lab/up.sh` attaches a do-nothing XDP_PASS program (`lab/xdp_pass.bpf.c`) to pb-lb1 and pb-lb2. With it the IPIP frames appear on pb-lb1 and reach the reals. It stays attached for IPVS runs too, so both forwarding planes see the same peer path.

### 23. Encapsulated SYNs dropped by the real as bad checksums

**Symptom.** After #22 was fixed, the IPIP frames reached real1 and `tunl0`'s receive counter rose, but there was still no SYN-ACK.

**Found with.** `nstat` in real1: `TcpInCsumErrors` rose with every attempt.

**Cause.** The client's veth had TX checksum offload on, so its TCP stack handed the veth a `CHECKSUM_PARTIAL` skb with the TCP checksum not yet computed. On a veth-to-veth path the receiving stack would trust the skb's checksum state, but native XDP sees only the raw bytes, encapsulates them, and the metadata that said "checksum still to do" is lost. The real decapsulates and verifies a checksum nobody computed. A physical NIC always puts a finished checksum on the wire, so this is a lab artifact, not a data-plane bug.

**Fix.** `lab/up.sh` turns off TX checksum offload on the client's veth0 (`ethtool -K veth0 tx off`), so the client sends what a real NIC would. It is set for every run, IPVS included.

### 24. UDP echo answered from the wrong address under DSR

**Symptom.** `echo hi | socat - UDP:198.51.100.1:5000` from the client printed nothing, through PacketBalance and through IPVS, while the request reached the real.

**Found with.** socat in the client namespace against the reals' echo server.

**Cause.** The echo server's socket was bound to 0.0.0.0 and unconnected, so the kernel chose the reply's source address by routing: the real's 10.0.0.2x, not the VIP. The client's connected socket drops a reply that does not come from the address it sent to. This is the UDP half of DSR that TCP gets for free, because an accepted TCP socket is bound to the address the SYN was sent to.

**Fix.** `lab/udp_echo.py` reads the destination address with `IP_PKTINFO` and replies from it.

### 25. The IPVS baseline measured 121 k, 1.04 M and 2.06 M pps for one configuration

**Symptom.** The three Experiment 1 repeats of IPVS `mh` (DR) in the first baseline run gave 1.04 M, 2.11 M and 124 k forwarded pps; `rr` 1.93 M, 2.38 M and 132 k; the no-LB ceiling 5.94 M, 1.12 M and 5.94 M. A spread of 20 times inside one configuration.

**Found with.** The per-row `offered_pps`, which showed that pktgen itself had slowed down (the generator lost speed, not the LB), then a CPU canary: a fixed single-threaded Python loop pinned to one vCPU ran 7 to 12 M iterations/s while the Mac had a video call and other apps running, and 15 to 18 M/s after a host reboot. The VM's vCPUs are host threads and may also land on efficiency cores. Nothing inside the VM shows that.

**Cause.** Host contention outside the VM, which the harness had no way to see.

**Fix.** `lab/common.sh` `host_gate`: before every packet-rate window and every Experiment 2 wrk run, wait until no compiler or build runs in the VM and the canary reaches `CANARY_MIN` (12 M/s); take the canary again after the window; a window that fails goes to `results/rejected.jsonl` and is retried. The canary and the gate result are on every row. The pre-guard Experiment 1, sweep and Experiment 2 rows moved to `results/superseded/` and every configuration was measured again. The new IPVS `mh` rows spread 2.10 to 2.28 Mpps received. 15 windows were rejected and retried in the final run.

### 26. PacketBalance's `tx` counter said 1.5 M pps forwarded; 0.58 M arrived

**Symptom.** Experiment 1, native XDP: the `tx` counter rose by 1.39 to 1.66 M/s while the reals received 0.55 to 0.60 M/s. Generic: 2.5 M/s counted, 1.4 M/s received. IPVS's `InPkts` and the reals' receive count agreed within 3%.

**Found with.** A packet accounting added to `lab/measure.sh` (the `netdev_delta` and `packet_accounting` row fields): receive and transmit packets and drops of every device on the path, `ethtool -S` on lb1's veth0 and on its peer pb-lb1, and `/proc/net/softnet_stat`, all as deltas over the window.

**Cause.** `tx` counts XDP_TX verdicts. On a veth, native XDP_TX puts the frame on the peer's (pb-lb1's) 256-entry ptr_ring, and when that ring is full the veth driver drops it after the program has returned. The drop shows only as lb1 veth0 `rx_queue_N_xdp_tx_errors` (and `tx_dropped`, and pb-lb1 `rx_dropped`: one event, three counters). In generic mode XDP_TX goes through the veth's normal transmit and shows as lb1 veth0 `tx_dropped`. In a 30 s native window at saturation (`results/exp1_mitigations.jsonl`, variant `standard`): about 141 M frames offered, about 100 M dropped before the program because lb1's own receive ring was full, about 40.5 M XDP_TX verdicts, about 21 M lost to the full pb-lb1 ring, about 19.5 M received, 9 to 15 k dropped by the reals' backlog, and under 0.1% of offered unaccounted for (timing skew between snapshots). The program's own drop counters were 0.

**Fix.** In the harness, not the program: every packet-rate row carries `received_pps` (what arrived) next to `forwarded_pps` (the LB's own count), plus the driver counters and the accounting, and `lab/render_tables.py` reports received pps, and pps per core computed from it, as the headline. `docs/API.md` now says that `tx` is XDP_TX verdicts, not delivered frames, and `docs/RUNBOOK.md` has the check against `xdp_tx_errors`.

### 27. `down.sh` stopped half way: "Cannot find device pb-real4"

**Symptom.** `sudo lab/down.sh` printed `Cannot find device "pb-real4"` and exited, leaving br0, the bpffs mount and `/run/pblab` behind. `up.sh` calls `down.sh` first and cleaned up on its second pass, so it went unnoticed.

**Found with.** `down.sh`'s own output.

**Cause.** The list of `pb-*` devices was taken after `ip netns del`, which removes the in-namespace ends of the veth pairs asynchronously. The root-side peer can vanish between the listing and the `ip link del`, and `set -e` aborted the script.

**Fix.** `ip link del` of a `pb-*` veth tolerates a device that is already gone.

### 28. The MTU check could not fail: DSR sends the big direction around the LB

**Symptom.** A `curl` of a 100 KB file through the VIP completed with and without the client route's `mtu 1480`.

**Found with.** `tcpdump` on the reals: the encapsulated packets were all small (SYN, ACKs, the GET). The 100 KB response goes from the real to the client directly.

**Cause.** Under DSR only the client-to-real direction is encapsulated, so a download never puts a full-size segment through the LB.

**Fix.** The reals' nginx accepts `PUT /upload/` (`dav_methods`), and `lab/mtucheck.sh` uploads 1 MB through the VIP. With the route MTU the largest encapsulated TCP payload is 1428 bytes (a 1500-byte outer packet) and every upload completes in both XDP modes. With `--no-route-mtu` every upload stalls at 64 KB and times out (the PMTU black hole in docs/DESIGN.md), with `drop_mtu` at 0 because the 1520-byte frames are dropped by the veth, not the program.

## Build and CI

### 16. `__array(values, ...)` not last in the `rings` map definition

**Symptom.** The first build of the XDP program failed with `error: flexible array member 'values' with type 'struct ring_map *[]' is not at the end of struct`.

**Found with.** clang with `-Werror`, on the first build in the VM.

**Cause.** libbpf's `__array()` expands to a flexible array member. The stub declared `__uint(pinning, ...)` after it, which C does not allow.

**Fix.** `pinning` now comes before `__array`. The map name, type and pinning are unchanged.

### 17. `clang -target bpf` could not find `<asm/types.h>`

**Symptom.** `/usr/include/linux/types.h:5:10: fatal error: 'asm/types.h' file not found`.

**Found with.** The first build of `pb_bpf_obj` in the VM.

**Cause.** Debian and Ubuntu keep the arch-specific `asm/` headers in `/usr/include/<triplet>/`. The host compiler searches that directory by default. `clang -target bpf` does not, because BPF has no multiarch triplet.

**Fix.** `bpf/CMakeLists.txt` adds `-idirafter /usr/include/${CMAKE_LIBRARY_ARCHITECTURE}` and falls back to `cc -print-multiarch`, so the same build works on x86_64 CI runners.

### 18. Daemon could not create its pin path under `ip netns exec` in CI

**Symptom.** In the GitHub Actions e2e job, the daemon failed with `mkdir /sys/fs/bpf/packetbalance/lb1/config: No such file or directory` when started with `ip netns exec lb1`, while the same command worked from the root namespace.

**Found with.** The CI log, then, reproduced in the VM, `ip netns exec X stat -f -c %T /sys/fs/bpf`, which printed `sysfs` inside the namespace against `bpf_fs` outside it.

**Cause.** `ip netns exec` unshares the mount namespace and mounts a fresh sysfs at `/sys`, so any bpffs mounted under `/sys/fs/bpf` is not visible to the command, and libbpf's auto-pin `mkdir` lands on plain sysfs.

**Fix.** The daemon now creates the pin path itself and refuses to start unless it is on a bpffs mount, with an error message that names this trap. The lab mounts its own bpffs at `/run/pblab/bpf`, which lives in the root mount namespace and is inherited by every `ip netns exec`, so pins also survive daemon restarts.
