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
