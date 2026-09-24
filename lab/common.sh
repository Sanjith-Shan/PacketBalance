# shellcheck shell=bash
# Variables here are consumed by the scripts that source this file.
# shellcheck disable=SC2034
# Shared definitions for the PacketBalance lab scripts. Sourced, never run.
#
# Everything the lab creates is named here, so down.sh can remove exactly that
# and nothing else: the namespaces client, lb1, lb2, real1..real5, the bridge
# br0, root-side veth peers pb-<ns>, runtime state under /run/pblab, and the
# daemon pin directories /sys/fs/bpf/packetbalance/lb1 and .../lb2.

LAB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$LAB_DIR/.." && pwd)"
GEN="$LAB_DIR/gen"            # generated daemon configs (lab/gen/lb1.yaml ...)
RUN=/run/pblab                # pid files, logs, nginx prefixes (tmpfs, root owned)
RESULTS="$REPO/results"

VIP=198.51.100.1
VIP_NET=198.51.100.0/24
BR=br0
LB_NS=(lb1 lb2)
REAL_NS=(real1 real2 real3 real4 real5)
ALL_NS=(client lb1 lb2 real1 real2 real3 real4 real5)
DEFAULT_REALS=(10.0.0.21 10.0.0.22 10.0.0.23 10.0.0.24)   # real5 is only for the Exp 4 drift case
PIN_ROOT=/sys/fs/bpf/packetbalance

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
    mkdir -p "$RUN" "$pin"
    rm -f "$sock"
    log "start packetbalance in $ns: --xdp-mode $mode $*"
    ip netns exec "$ns" setsid "$bin" --config "$GEN/$ns.yaml" --xdp-mode "$mode" "$@" \
        </dev/null >"$RUN/pb-$ns.log" 2>&1 &
    echo $! >"$RUN/pb-$ns.pid"
    for ((i = 0; i < 100; i++)); do
        if [[ -S $sock ]] && pbctl_ns "$ns" ping >/dev/null 2>&1; then
            log "packetbalance in $ns is up (pid $(cat "$RUN/pb-$ns.pid"))"
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
