#!/usr/bin/env bash
# Build the PacketBalance lab inside one Linux VM (run as root). Idempotent:
# tears down any previous lab first.
#
#   client (10.0.0.10) ---+
#   lb1    (10.0.0.2)  ---+--- br0 ---+--- real1..real5 (10.0.0.21..25)
#   lb2    (10.0.0.3)  ---+
#
# VIP 198.51.100.1. Each real has the VIP on lo, an IPIP decap device, rp_filter
# off, DSR ARP settings, nginx on :80, a UDP echo on :5000, conncheck-server on
# :7000. The client routes 198.51.100.0/24 via lb1. Neither LB forwards
# anything until a forwarding plane is started (packetbalance, or lab/ipvs.sh).
#
# Env: LAB_SKIP_BUILD=1 skips the cmake build. PB_BUILD_DIR picks the build dir.
#      LAB_HOST="M3 Pro via Lima vz" is recorded into lab/host.env for result rows.
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root
# Children (nginx, conncheck-server, the daemon) inherit a high fd limit.
ulimit -n 1048576 2>/dev/null || ulimit -n "$(ulimit -Hn)"

"$LAB_DIR/down.sh"

# --- build ---------------------------------------------------------------------
build() {
    local dir=${PB_BUILD_DIR:-$REPO/build}
    log "build: $dir"
    if [[ ! -f $dir/CMakeCache.txt ]]; then
        cmake -S "$REPO" -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Release >"$RUN/build.log" 2>&1 || {
            log "cmake configure of $dir failed, see $RUN/build.log"; return 1; }
    fi
    if ! cmake --build "$dir" >>"$RUN/build.log" 2>&1; then
        log "full build failed (daemon or data plane not ready?), building the lab tools only; see $RUN/build.log"
        cmake --build "$dir" --target conncheck conncheck-server >>"$RUN/build.log" 2>&1 || return 1
    fi
}
mkdir -p "$RUN"
if [[ ${LAB_SKIP_BUILD:-0} != 1 ]]; then
    build || die "build failed, see $RUN/build.log"
fi
CC_SERVER=$(find_bin conncheck-server) || die "conncheck-server not built"
log "conncheck-server: $CC_SERVER"

# --- host description for result rows -----------------------------------------
if [[ -n ${LAB_HOST:-} || ! -f $LAB_DIR/host.env ]]; then
    host=${LAB_HOST:-"$(uname -m) VM ($(systemd-detect-virt 2>/dev/null || echo unknown))"}
    printf 'LAB_HOST=%q\n' "$host" >"$LAB_DIR/host.env"
    log "wrote lab/host.env: LAB_HOST=$host"
fi

# --- modules ------------------------------------------------------------------
# ipip must be loaded before the namespaces exist so each gets its fallback
# tunl0 (it would also be created in existing ones, but order keeps it simple).
for m in ipip ip_vs ip_vs_rr ip_vs_wrr ip_vs_mh veth bridge; do
    modprobe "$m" 2>/dev/null || log "WARN: modprobe $m failed"
done
modprobe pktgen 2>/dev/null || log "WARN: modprobe pktgen failed (install linux-modules-extra-$(uname -r))"

# --- bridge -------------------------------------------------------------------
run ip link add "$BR" type bridge stp_state 0 forward_delay 0
run ip addr add 10.0.0.1/24 dev "$BR"
run ip link set "$BR" up
# Bridged frames must not go through iptables if br_netfilter happens to be loaded.
sysctl -qw net.bridge.bridge-nf-call-iptables=0 2>/dev/null || true

# --- namespaces ---------------------------------------------------------------
QUEUES=$(nproc)
mk_ns() {
    local ns=$1 ip mac
    ip=$(ns_ip "$ns"); mac=$(ns_mac "$ns")
    run ip netns add "$ns"
    # veth0 inside the namespace (the daemon config says interface: veth0),
    # pb-<ns> on the bridge. Multi-queue so XDP/NAPI can use more than one CPU.
    run ip link add "pb-$ns" numtxqueues "$QUEUES" numrxqueues "$QUEUES" type veth \
        peer name veth0 numtxqueues "$QUEUES" numrxqueues "$QUEUES" netns "$ns"
    ip link set "pb-$ns" master "$BR" mtu 1500 up
    nsx "$ns" ip link set lo up
    nsx "$ns" ip link set veth0 address "$mac" mtu 1500 up
    nsx "$ns" ip addr add "$ip/24" dev veth0
    nsx "$ns" sysctl -qw net.ipv4.conf.all.rp_filter=0 net.ipv4.conf.default.rp_filter=0 \
        net.ipv4.conf.lo.rp_filter=0 net.ipv4.conf.veth0.rp_filter=0
    log "$ns: veth0 $ip/24 $mac"
}
for ns in "${ALL_NS[@]}"; do mk_ns "$ns"; done

# --- client ---------------------------------------------------------------------
nsx client sysctl -qw net.ipv4.ip_local_port_range="1024 65535" net.ipv4.tcp_tw_reuse=1 \
    net.core.somaxconn=65535
route_via lb1

# --- load balancers ---------------------------------------------------------------
# Neither IPVS DR nor XDP forwarding needs ip_forward: IPVS consumes VIP packets
# in LOCAL_IN (the VIP is on lo while IPVS is up) and XDP never reaches the
# stack. Keeping it off means a detached LB does not route VIP packets anywhere.
for ns in "${LB_NS[@]}"; do
    nsx "$ns" sysctl -qw net.ipv4.ip_forward=0
    # A veth only accepts XDP_TX'd / redirected frames if its PEER runs NAPI,
    # which needs an XDP program or GRO on the peer. The peer of lb's veth0 is
    # pb-<lb> on the bridge. Without this, native-mode XDP_TX frames vanish.
    ethtool -K "pb-$ns" gro on >/dev/null 2>&1 || log "WARN: ethtool -K pb-$ns gro on failed"
done

# --- reals --------------------------------------------------------------------
write_nginx_conf() {
    local ns=$1 n=${1#real} dir="$RUN/$1"
    mkdir -p "$dir/www" "$dir/logs"
    printf 'real%s %s\n' "$n" "$(real_ip "$n")" >"$dir/www/index.html"
    chmod -R a+rX "$dir"
    cat >"$dir/nginx.conf" <<EOF
# PacketBalance lab, $ns. Generated by lab/up.sh.
worker_processes 2;
worker_rlimit_nofile 65535;
pid $dir/nginx.pid;
error_log $dir/logs/error.log warn;
events { worker_connections 16384; }
http {
    access_log off;
    default_type text/plain;
    keepalive_requests 1000000;
    server {
        listen 80 backlog=65535;
        root $dir/www;
        index index.html;
    }
}
EOF
}

setup_real() {
    local ns=$1 n=${1#real} tun=tunl0
    local ip; ip=$(real_ip "$n")
    nsx "$ns" ip addr add "$VIP/32" dev lo
    # Standard DSR real config: never answer ARP for the VIP (it lives on lo),
    # and source ARP requests from the veth address, not the VIP.
    nsx "$ns" sysctl -qw net.ipv4.conf.all.arp_ignore=1 net.ipv4.conf.all.arp_announce=2 \
        net.ipv4.conf.veth0.arp_ignore=1 net.ipv4.conf.veth0.arp_announce=2 \
        net.core.somaxconn=65535 net.ipv4.tcp_max_syn_backlog=65535
    # IPIP decap. The ipip module creates a fallback tunl0 (local any, remote any)
    # in every namespace unless net.core.fb_tunnels_only_for_init_net=1; it takes
    # every IPIP packet addressed to a local address.
    if ! nsx "$ns" ip link show tunl0 >/dev/null 2>&1; then
        tun=ipip0
        log "$ns: no per-netns tunl0, creating $tun (ipip local $ip remote any)"
        nsx "$ns" ip tunnel add "$tun" mode ipip local "$ip"
    fi
    nsx "$ns" ip link set "$tun" up
    # rp_filter must be 0 on the decap device: the inner source (the client) is
    # routed via veth0, not via the tunnel, so strict mode drops every packet.
    nsx "$ns" sysctl -qw "net.ipv4.conf.$tun.rp_filter=0"
    echo "$tun" >"$RUN/$ns/tundev"

    write_nginx_conf "$ns"
    nsx "$ns" nginx -c "$RUN/$ns/nginx.conf" -p "$RUN/$ns/" -e "$RUN/$ns/logs/error.log"
    ip netns exec "$ns" setsid python3 "$LAB_DIR/udp_echo.py" 5000 </dev/null >"$RUN/$ns/udp_echo.log" 2>&1 &
    echo $! >"$RUN/$ns/udp_echo.pid"
    ip netns exec "$ns" setsid "$CC_SERVER" --port 7000 --id "$n" </dev/null >"$RUN/$ns/conncheck.log" 2>&1 &
    echo $! >"$RUN/$ns/conncheck.pid"
    log "$ns: VIP $VIP on lo, $tun up, nginx :80, udp echo :5000, conncheck-server :7000 id=$n"
}
for ns in "${REAL_NS[@]}"; do
    mkdir -p "$RUN/$ns"
    setup_real "$ns"
done

# --- sanity -------------------------------------------------------------------
sleep 1
fail=0
for ns in "${REAL_NS[@]}"; do
    ip=$(ns_ip "$ns")
    if ! out=$(nsx client curl -s --max-time 2 "http://$ip/"); then
        log "FAIL: client cannot reach nginx on $ns ($ip)"; fail=1
    else
        log "ok: http://$ip/ -> $out"
    fi
done
for lb in "${LB_NS[@]}"; do
    nsx client ping -c1 -W1 "$(ns_ip "$lb")" >/dev/null || { log "FAIL: client cannot ping $lb"; fail=1; }
done
[[ $fail == 0 ]] || die "lab sanity checks failed"
log "lab is up. VIP $VIP has no forwarding plane yet: start one with"
log "  lab/ipvs.sh lb1 up mh      or      ip netns exec lb1 <build>/packetbalance --config lab/gen/lb1.yaml"
