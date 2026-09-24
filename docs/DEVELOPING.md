# Developing PacketBalance

## Layout

```
include/packetbalance/   abi.h (maps, structs, flags shared with the BPF program), hash.h (flow hash)
bpf/                     packetbalance.bpf.c, the XDP program. Builds to packetbalance.bpf.o + packetbalance.skel.h
src/core/                libpb_core: maglev.cpp, vip parsing, YAML config. Portable, unit tested
src/daemon/              packetbalance: loader, map state, health checker, JSON API, /metrics
src/pbctl/               pbctl: CLI over the unix socket
tools/conncheck/         long-lived connection survival client and server (Experiments 3 and 4)
tools/hashquality/       Experiment 5, one million tuples through maglev and modulo
tools/bpftrace/          live inspection scripts
tests/core/              Google Test, runs on any platform
tests/dataplane/         BPF_PROG_TEST_RUN tests for the XDP program, Linux and root
tests/daemon/            LbState, MapReader, JSON commands and /metrics on real (unpinned) maps, Linux and root
lab/                     lima.yaml, up.sh, down.sh, ipvs.sh, pktgen.sh, measure.sh, experiments.sh, diagnose.sh
deploy/                  systemd unit and example config
docs/                    DESIGN, RUNBOOK, CAPACITY, BUGS, API
results/                 JSON rows produced by lab/experiments.sh, committed
```

## Build

On Linux with libbpf, clang, bpftool:

```
cmake -B build -G Ninja && cmake --build build && ctest --test-dir build
```

On x86-64 hosts `clang -target bpf` pulls in glibc's 32-bit stub header, so install
`libc6-dev-i386` (Debian/Ubuntu) or `glibc-devel.i686` (Fedora) before building the
data plane. aarch64 does not need it.

On macOS only the core, pbctl, tools and core tests build (`PB_BUILD_BPF=OFF`,
`PB_BUILD_DAEMON=OFF` are the defaults there).

## The lab VM

```
brew install lima
limactl start --name=packetbalance lab/lima.yaml
limactl shell packetbalance
cd ~/Documents/PacketBalance    # the repo is mounted at the same path
```

## Conventions

- `include/packetbalance/abi.h` is the contract between the data plane and the
  control plane. Change it in one place and rebuild both.
- Addresses and ports in maps are network byte order.
- The ring for a VIP depends only on the set of (address, weight) pairs, never on
  insertion order or real_id, so two daemons with the same config build the same ring.
- Every measured number lives in `results/*.json` with host, kernel, XDP mode, packet
  size, flow count, conntrack size and date on the row.

## Reproducing on an x86-64 cloud VM

`lab/cloud-run.sh` runs the whole pipeline on a fresh Ubuntu 24.04 x86-64 host as
root: packages (the `lab/lima.yaml` set plus `libc6-dev-i386` and
`linux-tools-generic`), bpffs, clone, build, `ctest`, `lab/up.sh`,
`lab/experiments.sh all`, `lab/render_tables.py`, and a tarball of
`results/*.jsonl`. It is safe to rerun, and it moves any earlier result rows
(including the committed Lima rows in a fresh clone) into `results/prior-<date>/`
so the tables it prints describe that host only.

```
# 1. Create the box (Hetzner shown; any fresh Ubuntu 24.04 x86-64 VM works)
hcloud server create --name pb-lab --type cx22 --image ubuntu-24.04 --ssh-key <your-key>

# 2. Copy the script over
scp lab/cloud-run.sh root@<ip>:/root/

# 3. Run it (use tmux or nohup: the run outlives a flaky ssh session)
ssh root@<ip> 'tmux new -d -s pb "bash /root/cloud-run.sh \"Hetzner CX22 x86-64\" 2>&1 | tee /root/cloud-run.log"'
```

When it finishes, the log ends with the `scp` command that fetches
`/root/packetbalance-results-<date>.tar.gz`. Delete the server afterwards
(`hcloud server delete pb-lab`).

Runtime and cost, stated honestly. This has not yet been timed on x86-64. From the
experiment plan (three repeats of 30 s windows for Experiments 1, 2 and 6, the
Experiment 1 sweep, 10,000-connection runs for Experiments 3 and 4) plus about 10
minutes of packages and build, expect roughly 1.5 to 2 hours. A CX22 bills by the
hour at a few euro cents, so a run costs well under 1 EUR; check current pricing.
Caveats that belong next to any number from it: a CX22 has 2 shared vCPUs and 4 GB,
fewer than the 6-vCPU lab VM, so `PKTGEN_THREADS=4` oversubscribes it and the
generator competes with the forwarding plane (set `PKTGEN_THREADS=1`, or use a
4-vCPU or dedicated-vCPU type such as CX32 or CCX13, and record which); shared
vCPUs are noisy, so the repeat spread matters more than the mean; and it is still a
VM with veths, so every number is a lower bound, never a NIC number. If the
provider's kernel has no `linux-modules-extra-$(uname -r)`, pktgen may be missing and
Experiments 1 and 6 cannot generate load; the script warns about this at install time.
