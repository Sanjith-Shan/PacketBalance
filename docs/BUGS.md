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

## Control plane

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

These came from the lab (`lab/`) and the IPVS baseline, and from `tools/conncheck`. Every one of them would have made a baseline look worse or better than it is.

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
