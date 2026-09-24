# shellcheck shell=bash
# Variables here are consumed by the scripts that source this file.
# shellcheck disable=SC2034
# Shared definitions for the PacketBalance lab scripts. Sourced, never run.
#
# Everything the lab creates is named here, so down.sh can remove exactly that
# and nothing else: the namespaces client, lb1, lb2, real1..real5, the bridge
# br0, root-side veth peers pb-<ns>, runtime state under /run/pblab, and the
# lab's own bpffs mounted at /run/pblab/bpf holding the daemons' pins
# (/run/pblab/bpf/lb1, /run/pblab/bpf/lb2).

LAB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$LAB_DIR/.." && pwd)"
GEN="$LAB_DIR/gen"            # generated daemon configs (lab/gen/lb1.yaml ...)
RUN=/run/pblab                # pid files, logs, nginx prefixes (tmpfs, root owned)
RESULTS=${PB_RESULTS_DIR:-$REPO/results}   # override for scratch runs

VIP=198.51.100.1
VIP_NET=198.51.100.0/24
BR=br0
LB_NS=(lb1 lb2)
REAL_NS=(real1 real2 real3 real4 real5)
ALL_NS=(client lb1 lb2 real1 real2 real3 real4 real5)
DEFAULT_REALS=(10.0.0.21 10.0.0.22 10.0.0.23 10.0.0.24)   # real5 is only for the Exp 4 drift case
# Not /sys/fs/bpf: `ip netns exec` gives the command a private mount namespace
# with a fresh sysfs on /sys, which hides the host's bpffs at /sys/fs/bpf. A
# bpffs mounted under /run in the root mount namespace is inherited by every
# `ip netns exec`, so pins survive daemon restarts. up.sh mounts it.
PIN_ROOT=$RUN/bpf

# The client's route to the VIP carries mtu 1480 so TCP's MSS is 1440: a full
# segment plus the 20-byte outer IPIP header still fits the 1500-byte veths.
# Without it, a 1500-byte client packet cannot be encapsulated (the LB would
# have to drop it or send ICMP frag-needed, which is Katran's PMTU story).
# IPVS DR does not encapsulate, but both LBs get the same route for fairness.
ROUTE_MTU=1480

log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }
die()  { log "FATAL: $*"; exit 1; }
run()  { log "+ $*"; "$@"; }
nsx()  { local ns=$1; shift; ip netns exec "$ns" "$@"; }

ns_ip() {  # ns_ip <ns> -> the lab address of that namespace
    case $1 in
        client) echo 10.0.0.10 ;;
        lb1) echo 10.0.0.2 ;;
        lb2) echo 10.0.0.3 ;;
        real[1-9]) echo "10.0.0.2${1#real}" ;;
        *) die "unknown namespace $1" ;;
    esac
}

ns_mac() {  # deterministic MAC for veth0 in <ns>: 02:00:0a:00:00:<last octet>
    local ip last
    ip=$(ns_ip "$1")
    last=${ip##*.}
    printf '02:00:0a:00:00:%02x\n' "$last"
}

real_ip() { echo "10.0.0.2$1"; }   # real_ip 3 -> 10.0.0.23

need_root() { [[ $(id -u) -eq 0 ]] || die "run as root (sudo $0)"; }

lab_is_up() { ip netns list 2>/dev/null | grep -qw client && ip link show "$BR" >/dev/null 2>&1; }

# ---------------------------------------------------------------------------
# Binaries. PB_BUILD_DIR overrides; otherwise the first build dir that has it.
# ---------------------------------------------------------------------------
find_bin() {
    local name=$1 d c
    for d in ${PB_BUILD_DIR:+"$PB_BUILD_DIR"} "$REPO/build" "$REPO/build-lab"; do
        for c in "$d/$name" "$d/src/daemon/$name" "$d/src/pbctl/$name" "$d/tools/conncheck/$name" \
                 "$d/tools/$name" "$d/bin/$name"; do
            if [[ -x $c && -f $c ]]; then echo "$c"; return 0; fi
        done
    done
    return 1
}

# ---------------------------------------------------------------------------
# Host description for result rows (lab/host.env, written by up.sh)
# ---------------------------------------------------------------------------
load_host_env() {
    LAB_HOST="unknown host"
    # shellcheck disable=SC1091
    [[ -f $LAB_DIR/host.env ]] && source "$LAB_DIR/host.env"
    : "${LAB_HOST:=unknown host}"
}

# ---------------------------------------------------------------------------
# Client routing
# ---------------------------------------------------------------------------
route_via() {  # route_via lb1|lb2
    local gw
    gw=$(ns_ip "$1")
    nsx client ip route replace "$VIP_NET" via "$gw" dev veth0 mtu "$ROUTE_MTU"
    log "client: $VIP_NET via $gw ($1) mtu $ROUTE_MTU"
}

# ---------------------------------------------------------------------------
# UDP echo servers are paused (SIGSTOP) during packet-rate measurements so a
# userspace echo process replying to millions of pps does not load the VM.
# Received packets are counted at the reals' interfaces, below the socket.
# ---------------------------------------------------------------------------
udp_echo_signal() {  # udp_echo_signal STOP|CONT
    local r pid
    for r in "${REAL_NS[@]}"; do
        pid=$(cat "$RUN/$r/udp_echo.pid" 2>/dev/null || true)
        [[ -n $pid ]] && kill "-$1" "$pid" 2>/dev/null || true
    done
}

# ---------------------------------------------------------------------------
# PacketBalance daemon control (one daemon per LB namespace)
# ---------------------------------------------------------------------------
pb_sock() { echo "$RUN/pb-$1.sock"; }
pb_pin()  { echo "$PIN_ROOT/$1"; }

pbctl_ns() {  # pbctl_ns <lbns> args...   (unix sockets are not netns bound)
    local ns=$1; shift
    local bin
    bin=$(find_bin pbctl) || die "pbctl not built"
    "$bin" --socket "$(pb_sock "$ns")" "$@"
}

# write_pb_config <lbns> <hash> <conntrack true|false> <ct_size> [extra real ...]
write_pb_config() {
    local ns=$1 hash=$2 ct=$3 ctsize=$4; shift 4
    local reals=("${DEFAULT_REALS[@]}" "$@")
    local out="$GEN/$ns.yaml" port proto r
    mkdir -p "$GEN"
    {
        echo "# Generated by lab/common.sh write_pb_config for namespace $ns. Do not edit."
        echo "interface: veth0"
        echo "xdp_mode: auto"
        echo "hash: $hash"
        echo "encap_src_prefix: 10.99.0.0/24"
        echo "conntrack:"
        echo "  enabled: $ct"
        echo "  size: $ctsize"
        echo "socket: $(pb_sock "$ns")"
        echo "pin_path: $(pb_pin "$ns")"
        echo "metrics:"
        echo "  listen: 127.0.0.1:9101"
        echo "health_check:"
        echo "  interval_ms: 1000"
        echo "  timeout_ms: 500"
        echo "  fall: 3"
        echo "  rise: 2"
        echo "vips:"
        for port in 80/tcp 5000/udp 7000/tcp; do
            proto=${port#*/}
            echo "  - address: $VIP"
            echo "    port: ${port%/*}"
            echo "    proto: $proto"
            echo "    reals:"
            for r in "${reals[@]}"; do echo "      - { address: $r, weight: 1 }"; done
        done
    } >"$out"
    echo "$out"
}

# pb_start <lbns> <xdp_mode native|generic> [daemon flags...]
# Config comes from $GEN/<lbns>.yaml (write_pb_config first).
pb_start() {
    local ns=$1 mode=$2; shift 2
    local bin sock pin i
    bin=$(find_bin packetbalance) || die "packetbalance daemon not built (cmake --build build)"
    sock=$(pb_sock "$ns"); pin=$(pb_pin "$ns")
    mountpoint -q "$PIN_ROOT" || die "$PIN_ROOT is not a bpffs mount (run lab/up.sh)"
    mkdir -p "$pin"
    rm -f "$sock"
    log "start packetbalance in $ns: --xdp-mode $mode $*"
    ip netns exec "$ns" setsid "$bin" --config "$GEN/$ns.yaml" --xdp-mode "$mode" \
        --pin-path "$pin" --socket "$sock" "$@" \
        </dev/null >"$RUN/pb-$ns.log" 2>&1 &
    echo $! >"$RUN/pb-$ns.pid"
    for ((i = 0; i < 100; i++)); do
        if [[ -S $sock ]] && pbctl_ns "$ns" ping >/dev/null 2>&1; then
            log "packetbalance in $ns is up (pid $(cat "$RUN/pb-$ns.pid"))"
            pb_wait_rings "$ns"
            return 0
        fi
        if ! kill -0 "$(cat "$RUN/pb-$ns.pid")" 2>/dev/null; then
            log "packetbalance in $ns exited during start; log follows"
            tail -n 20 "$RUN/pb-$ns.log" >&2 || true
            return 1
        fi
        sleep 0.2
    done
    log "packetbalance in $ns did not answer ping within 20 s"
    tail -n 20 "$RUN/pb-$ns.log" >&2 || true
    return 1
}

# pb_wait_rings <lbns>: the daemon adds a real to a VIP's ring only once its
# neighbor (MAC) is resolved, so right after start the rings fill over ~100 ms.
# Wait until every configured real of every VIP is in its ring (max 10 s).
pb_wait_rings() {
    local ns=$1 i
    for ((i = 0; i < 50; i++)); do
        if pbctl_ns "$ns" --json vip list 2>/dev/null | python3 -c '
import json, sys
v = json.load(sys.stdin)
sys.exit(0 if v and all(r.get("in_ring", True) for x in v for r in x.get("reals", [])) else 1)'; then
            return 0
        fi
        sleep 0.2
    done
    log "WARN: not every real of $ns is in its ring after 10 s"
}

# pb_stop <lbns>: stop the daemon, detach XDP, remove its pinned maps so the
# next configuration starts with fresh maps (conntrack size is fixed at load).
pb_stop() {
    local ns=$1 pid i
    pid=$(cat "$RUN/pb-$ns.pid" 2>/dev/null || true)
    if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        for ((i = 0; i < 50; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    rm -f "$RUN/pb-$ns.pid" "$(pb_sock "$ns")"
    if ip netns list | grep -qw "$ns"; then
        nsx "$ns" ip link set dev veth0 xdp off 2>/dev/null || true
        nsx "$ns" ip link set dev veth0 xdpgeneric off 2>/dev/null || true
    fi
    rm -rf "$(pb_pin "$ns")"
}

ipvs() { "$LAB_DIR/ipvs.sh" "$@"; }

# Only one forwarding plane may own the VIP in a namespace at a time.
all_lbs_off() {
    local ns
    for ns in "${LB_NS[@]}"; do
        pb_stop "$ns"
        ipvs "$ns" down >/dev/null 2>&1 || true
    done
}

# ---------------------------------------------------------------------------
# /proc/net/dev counters inside a namespace
# ---------------------------------------------------------------------------
dev_rx_packets() {  # dev_rx_packets <ns> <dev>
    # shellcheck disable=SC2016
    nsx "$1" awk -v d="$2:" '$1 == d { print $3; found = 1 } END { if (!found) print 0 }' /proc/net/dev
}

reals_rx_sum() {  # reals_rx_sum <dev> <ns...>
    local dev=$1 s=0 ns v; shift
    for ns in "$@"; do
        v=$(dev_rx_packets "$ns" "$dev")
        s=$((s + v))
    done
    echo "$s"
}

# ---------------------------------------------------------------------------
# Host contention guards for timed measurements (Exp 1, 2, 6).
# The lab VM's vCPUs are threads on a shared host: other VMs, host apps and the
# host's scheduler (Apple silicon has performance and efficiency cores) change
# how fast they run, by 2x in this lab (see docs/bugs/lab.md). Two signals:
#  * vm_busy_procs: builds inside the VM (compilers, linkers, ninja, apt).
#  * cpu_canary: a fixed single-threaded Python loop pinned to the VM's last
#    CPU (the pktgen threads use CPUs 0..3), median iterations per second of
#    three 0.2 s runs. A slow host shows up as a low canary.
# host_gate waits (up to GATE_MAX_WAIT s) for no builds and a canary of at least
# CANARY_MIN before a window starts; the measurement re-checks the canary during
# and after the window and a row whose minimum canary is below CANARY_MIN is
# written to results/rejected.jsonl instead, and the window is retried.
# ---------------------------------------------------------------------------
CANARY_MIN=${CANARY_MIN:-7000000}
GATE_MAX_WAIT=${GATE_MAX_WAIT:-600}
BUSY_RE='^(cc1|cc1plus|clang|clang\+\+|clang-[0-9]+|ld|ld\.lld|lld|ninja|make|cmake|ctest|as|c\+\+|g\+\+|gcc|apt|apt-get|dpkg)$'
vm_busy_procs() { ps -eo comm= | grep -cE "$BUSY_RE" || true; }
cpu_canary() {
    taskset -c "$(($(nproc) - 1))" python3 -c '
import statistics, time
def once():
    t = time.perf_counter(); n = 0
    while time.perf_counter() - t < 0.2:
        n += 1
    return n / 0.2
print(int(statistics.median(once() for _ in range(3))))'
}
# host_gate: prints "<seconds waited> <canary>" once the VM is quiet and fast.
host_gate() {
    local waited=0 c b
    while :; do
        b=$(vm_busy_procs); c=$(cpu_canary)
        if [[ $b -eq 0 && $c -ge $CANARY_MIN ]]; then break; fi
        if ((waited >= GATE_MAX_WAIT)); then log "host_gate: gave up after ${waited}s (builds=$b canary=$c)"; break; fi
        ((waited % 30 == 0)) && log "host_gate: waiting (builds in VM: $b, cpu canary $c < $CANARY_MIN?)"
        sleep 2; waited=$((waited + 3))
    done
    echo "$waited $c"
}
