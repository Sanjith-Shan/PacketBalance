#!/usr/bin/env bash
# One-shot PacketBalance experiment run on a fresh Ubuntu 24.04 x86-64 cloud VM
# (Hetzner CX22 or any similar box). Run as root over ssh:
#
#   ssh root@HOST 'bash -s' -- "Hetzner CX22 x86-64" < lab/cloud-run.sh
#   ssh root@HOST 'bash /root/cloud-run.sh "Hetzner CX22 x86-64"'
#
# Args:  $1  LAB_HOST label recorded on every result row (required)
#        $2  optional path to an existing checkout (default: clone into /root/packetbalance)
# Env:   PB_REPO_URL  git URL to clone (default https://github.com/Sanjith-Shan/packetbalance)
#        PB_REF       branch or tag to check out (default: the remote's default branch)
#        PB_BUILD_DIR build directory (default <repo>/build), read by lab/*.sh too
#        REPEATS, DURATION, CONNS, EXP_LBS, PKTGEN_THREADS, NOTES pass through to
#        lab/experiments.sh unchanged.
#
# Phases: packages, bpffs, source, build, ctest, lab up, experiments, tables, tarball.
# Safe to rerun: apt is idempotent, bpffs is mounted only if missing, an existing
# clone is fast-forwarded, cmake reuses its cache, lab/up.sh tears down any previous
# lab first, and result rows from earlier runs (including the committed Lima rows in
# a fresh clone) are moved aside so the tables and the tarball hold this host only.
set -euo pipefail

log() { printf '[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }
die() { log "FATAL: $*"; exit 1; }
phase() { log "==== phase: $* ===="; }

[[ $(id -u) -eq 0 ]] || die "run as root"
[[ $# -ge 1 && -n $1 ]] || die "usage: $0 \"<host label, e.g. Hetzner CX22 x86-64>\" [repo path]"
LAB_HOST=$1
REPO=${2:-/root/packetbalance}
PB_REPO_URL=${PB_REPO_URL:-https://github.com/Sanjith-Shan/packetbalance}
PB_REF=${PB_REF:-}
STAMP=$(date -u +%Y%m%d-%H%M%S)
START=$(date +%s)

[[ $(uname -m) == x86_64 ]] || log "WARN: this script targets x86-64, running on $(uname -m)"
if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    [[ ${VERSION_ID:-} == 24.04 ]] || log "WARN: tested for Ubuntu 24.04, this is ${PRETTY_NAME:-unknown}"
fi

# ---------------------------------------------------------------------------
phase "packages"
# Same set as lab/lima.yaml's provision block, plus libc6-dev-i386 (x86-64
# `clang -target bpf` pulls in glibc's 32-bit stub header) and
# linux-tools-generic (bpf/CMakeLists.txt uses its versioned bpftool when the
# running kernel has no linux-tools package of its own).
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git \
    clang llvm lld libbpf-dev libelf-dev zlib1g-dev libc6-dev-i386 \
    linux-tools-common linux-tools-generic \
    libgtest-dev libgmock-dev libyaml-cpp-dev nlohmann-json3-dev \
    ipvsadm iproute2 iputils-ping tcpdump bpftrace ethtool \
    nginx-light iperf3 wrk sysstat curl jq python3 python3-venv \
    socat netcat-openbsd ca-certificates
# Kernel-specific packages: optional, because some cloud kernels have none.
apt-get install -y --no-install-recommends "linux-tools-$(uname -r)" \
    || log "WARN: no linux-tools-$(uname -r); bpftool comes from linux-tools-generic"
apt-get install -y --no-install-recommends "linux-modules-extra-$(uname -r)" \
    || log "WARN: no linux-modules-extra-$(uname -r); pktgen may be missing and Exp 1/6 will fail"
systemctl disable --now nginx 2>/dev/null || true

# ---------------------------------------------------------------------------
phase "bpffs"
# The lab mounts its own bpffs at /run/pblab/bpf (lab/up.sh); /sys/fs/bpf is
# mounted here only for bpftool use from the root namespace.
if ! mountpoint -q /sys/fs/bpf; then
    mount -t bpf bpf /sys/fs/bpf
fi
log "/sys/fs/bpf: $(stat -f -c %T /sys/fs/bpf)"

# ---------------------------------------------------------------------------
phase "source: $REPO"
if [[ -d $REPO/.git ]]; then
    git -C "$REPO" fetch --tags origin
    if [[ -n $PB_REF ]]; then git -C "$REPO" checkout "$PB_REF"; fi
    git -C "$REPO" pull --ff-only || log "WARN: could not fast-forward $REPO, using it as is"
elif [[ -e $REPO ]]; then
    [[ -f $REPO/lab/up.sh ]] || die "$REPO exists but is not a PacketBalance checkout"
    log "using existing tree at $REPO (not a git clone)"
else
    git clone "$PB_REPO_URL" "$REPO"
    if [[ -n $PB_REF ]]; then git -C "$REPO" checkout "$PB_REF"; fi
fi
COMMIT=$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)
log "commit: $COMMIT"

export PB_BUILD_DIR=${PB_BUILD_DIR:-$REPO/build}
export LAB_HOST

# ---------------------------------------------------------------------------
phase "build: $PB_BUILD_DIR"
if [[ ! -f $PB_BUILD_DIR/CMakeCache.txt ]]; then
    cmake -S "$REPO" -B "$PB_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
fi
cmake --build "$PB_BUILD_DIR"

# ---------------------------------------------------------------------------
phase "ctest (as root, includes the BPF_PROG_TEST_RUN data-plane tests)"
ctest --test-dir "$PB_BUILD_DIR" --output-on-failure

# ---------------------------------------------------------------------------
phase "results: move earlier rows aside"
if compgen -G "$REPO/results/*.jsonl" >/dev/null; then
    prior="$REPO/results/prior-$STAMP"
    mkdir -p "$prior"
    mv "$REPO"/results/*.jsonl "$prior"/
    log "moved earlier results/*.jsonl to $prior"
fi

# ---------------------------------------------------------------------------
phase "lab up (LAB_HOST=$LAB_HOST)"
# Already built above; up.sh records LAB_HOST into lab/host.env.
LAB_SKIP_BUILD=1 "$REPO/lab/up.sh"

# ---------------------------------------------------------------------------
phase "experiments: all"
NOTES="${NOTES:+$NOTES; }cloud-run commit $COMMIT" "$REPO/lab/experiments.sh" all

# ---------------------------------------------------------------------------
phase "tables"
python3 "$REPO/lab/render_tables.py"

# ---------------------------------------------------------------------------
phase "tarball"
TARBALL=/root/packetbalance-results-$STAMP.tar.gz
(cd "$REPO" && tar -czf "$TARBALL" results/*.jsonl)
ip4=$(curl -4 -fsS --max-time 5 https://ifconfig.me 2>/dev/null || hostname -I | awk '{print $1}')
log "done in $(( ($(date +%s) - START) / 60 )) min, commit $COMMIT, host label: $LAB_HOST"
echo
echo "Results: $TARBALL"
echo "Fetch with:"
echo "  scp root@${ip4:-HOST}:$TARBALL ."
