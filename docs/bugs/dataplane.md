# Data plane bugs

Real bugs hit while building the XDP program (`bpf/packetbalance.bpf.c`), its
BPF_PROG_TEST_RUN tests (`tests/dataplane/`) and the bpftrace scripts
(`tools/bpftrace/`). Environment: Lima VM, Ubuntu 24.04 aarch64, kernel
6.8.0-139, clang 18, libbpf 1.3.0, bpftrace 0.20.2.

## 1. Verifier rejects packet math after `bpf_ntohs(iph->tot_len)`

- **Symptom.** The skeleton failed to load with `-EINVAL`. Every data-plane test failed in SetUp.
- **How found.** The libbpf verifier log printed by the test:
  ```
  103: (69) r7 = *(u16 *)(r6 +16)
  104: (dc) r7 = be16 r7                ; R7_w=scalar()
  106: (2d) if r3 > r7 goto pc+2        ; R7_w=scalar(umin=20)
  107: (0f) r2 += r7
  math between pkt pointer and register with unbounded min value is not allowed
  ```
- **Root cause.** The 16-bit load is bounded to `0xffff`, but the 6.8 verifier drops all bounds across the `be16` byte-swap instruction, so `tot_len` becomes an unbounded scalar. The truncation check `(void *)iph + tot_len > data_end` then adds an unbounded register to a packet pointer, which the verifier refuses. clang knows a byte-swapped `__u16` fits in 16 bits, so a plain `if (tot_len > 0xffff)` is deleted as dead code and does not help.
- **Fix.** `asm volatile("" : "+r"(tot_len));` hides the value from the optimizer, then an explicit `if (tot_len > 0xffff) drop` gives the verifier the bound. Loads with 2,763 verified instructions.

## 2. A per-CPU conntrack value that another CPU zero-filled reads as a hit on real 0

- **Symptom.** A flow whose SYN was handled on CPU A and whose next packet was handled on CPU B was sent to `real_id 0` (10.0.0.20 in the test), a real the ring never chose.
- **How found.** From reading `pcpu_init_value()` in `kernel/bpf/hashtab.c`: when a BPF program inserts into a `PERCPU` hash, the kernel zero-fills the value on every other CPU, so the key exists for every CPU. The guard went in with the first version of the program. The failure was then shown by test `OtherCpuZeroValueIsAMissNotRealZero`, which pins the thread to CPU 0 for the SYN and CPU 1 for the ACK. With the guard removed the test fails: outer `daddr` is 10.0.0.20 (real 0) instead of 10.0.0.21, and the table records a hit, not a miss.
- **Root cause.** `LRU_PERCPU_HASH` shares keys across CPUs and keeps values per CPU. A lookup on CPU B returns B's all-zero value, and `real_id 0` is a valid id.
- **Fix.** A value with `last_seen_ns == 0` counts as a miss (`bpf_ktime_get_ns()` is never 0 after boot). The flow falls through to the hash, which gives the same real CPU A picked unless the ring changed. Anything that walks `conntrack` from userspace (`pbctl flows`) must also skip per-CPU values with `last_seen_ns == 0`.

## 3. Conntrack tests flaked: the test thread migrated between CPUs

- **Symptom.** Out of 6 back-to-back runs of `dataplane_test`, 3 had failures, each time in a different test: `ConntrackPinsFlowAcrossRingSwap`, `IcmpFragNeededForwardedToFlowOwner`, or `RstForUnknownFlowForwardedWithoutState`. The failing packet went to the ring's current real, not to the pinned one, and `PB_CNT_CT_HIT` was 0.
- **How found.** Running the binary in a loop and counting failures by name with `uniq -c`.
- **Root cause.** `BPF_PROG_TEST_RUN` runs the program on the calling CPU. When the scheduler moved the test thread between two packets of the same flow, the second packet saw the zero-filled value from bug 2. So the program was correct (a miss and a hash) and the test's assumption was wrong. On a real NIC one flow stays on one RX queue.
- **Fix.** The test fixture pins itself to one CPU in `SetUp` and restores its affinity in `TearDown`. The cross-CPU case has its own test, which pins explicitly. After the fix, 20 of 20 runs passed.

## 4. `__array(values, ...)` not last in the `rings` map definition

- **Symptom.** The first build of the program failed: `error: flexible array member 'values' with type 'struct ring_map *[]' is not at the end of struct`.
- **How found.** clang with `-Werror`, on the first build in the VM.
- **Root cause.** libbpf's `__array()` expands to a flexible array member. The stub declared `__uint(pinning, ...)` after it, which C does not allow.
- **Fix.** `pinning` now comes before `__array`. The map name, type and pinning are unchanged.

## 5. `clang -target bpf` could not find `<asm/types.h>`

- **Symptom.** `/usr/include/linux/types.h:5:10: fatal error: 'asm/types.h' file not found`.
- **How found.** First build of `pb_bpf_obj` in the VM.
- **Root cause.** Debian and Ubuntu keep the arch-specific `asm/` headers in `/usr/include/<triplet>/`. The host compiler searches that directory by default. `clang -target bpf` does not, because BPF has no multiarch triplet.
- **Fix.** `bpf/CMakeLists.txt` adds `-idirafter /usr/include/${CMAKE_LIBRARY_ARCHITECTURE}` and falls back to `cc -print-multiarch`, so the same build works on x86_64 CI runners.

## 6. bpftrace filter on the program name never matched

- **Symptom.** `xdp_action_histogram.bt` filtered with `args->xdp_prog->aux->name == "xdp_packetbal"`. It would have printed nothing, forever.
- **How found.** Before trusting the filter, it was checked against a probe that does fire under test runs, `fexit:vmlinux:bpf_prog_test_run_xdp`: `@eq[0]: 2811` for `==` against `@ncmp[1]: 2811` for `strncmp(..., 13)`.
- **Root cause.** `aux->name` is `char[16]` and holds the name truncated to 15 characters, `xdp_packetbalan` (`bpftool prog show` prints the same). `==` compares whole strings, so a prefix never matches.
- **Fix.** `strncmp(args->xdp_prog->aux->name, "xdp_packetbal", 13) == 0`.

## 7. bpftrace 0.20 generated a program the verifier rejects

- **Symptom.** `ERROR: Error loading program: tracepoint:xdp:xdp_exception`.
- **How found.** `bpftrace -v` printed the verifier log, which ended at `4: (7b) *(u64 *)(r10 -53) = r1`, a misaligned 8-byte stack store.
- **Root cause.** A bpftrace 0.20 codegen problem: the map key was a tuple that mixed ints with a string (`@name[act]`), and bpftrace laid it out unaligned on the stack.
- **Fix.** The key is now all numeric, `(prog_id, act, ifindex)`, and the action numbers are listed in a comment.

## 8. bpftrace printed the adjust_head delta as 4294967276

- **Symptom.** `@delta[4294967276]: 4321` where `-20` was expected.
- **How found.** The first run of `xdp_adjust_head.bt` against test-run traffic.
- **Root cause.** The helper's `int delta` arrives zero-extended in a 64-bit register, and casting it to `int64` keeps it positive.
- **Fix.** Cast to `int32` for the argument and for the return value. It now prints `@delta[-20]`.
