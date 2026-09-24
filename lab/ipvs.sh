#!/usr/bin/env bash
# IPVS baseline in direct-routing mode (-g, IPVS's DSR) inside an LB namespace.
#
#   lab/ipvs.sh <lb1|lb2> up   {rr|mh|wrr}     VIP 198.51.100.1: 80/tcp, 5000/udp, 7000/tcp
#   lab/ipvs.sh <lb1|lb2> down
#   lab/ipvs.sh <lb1|lb2> del  <real-ip>       remove a real from every service (Exp 3)
#   lab/ipvs.sh <lb1|lb2> add  <real-ip>       add it back
#   lab/ipvs.sh <lb1|lb2> status
#
# Env: IPVS_REALS="10.0.0.21 10.0.0.22 10.0.0.23 10.0.0.24" (default)
#      IPVS_SLOPPY_TCP=1 (default 1, see below)
#
# Notes that matter for the comparison with PacketBalance:
#  * DR rewrites only the destination MAC, so the director must own the VIP
#    (IPVS works in LOCAL_IN) and so must each real (they have it on lo). "up"
#    puts the VIP on the director's lo, "down" removes it, so a director with
#    IPVS off does not accept VIP traffic into its own stack.
#  * mh hashes only the SOURCE ADDRESS unless the service has the mh-port flag.
#    Every lab connection comes from 10.0.0.10, so without -b mh-port all 10,000
#    conncheck connections land on one real. We always pass -b mh-port.
#  * net.ipv4.vs.sloppy_tcp=1 lets IPVS create a connection entry from a non-SYN
#    packet. Without it, a director that takes over mid-connection (Exp 4) does
#    not schedule the ACKs of existing flows at all: they fall through to its
#    own stack, which answers RST because nothing listens on the VIP port. A
#    stateless director tier (Exp 4) therefore needs sloppy_tcp. It is set the
#    same for rr and mh, so only the scheduler differs; Exp 4 also records
#    rows with sloppy_tcp=0.
#  * net.ipv4.vs.expire_nodest_conn=1: when a real is removed, the next packet
#    of a connection bound to it expires the entry instead of being silently
#    dropped for the entry's lifetime (900 s for established TCP). That is the
#    fair counterpart of `pbctl real del` in Exp 3: the connections on the
#    removed real break immediately and visibly.
#  * net.ipv4.vs.conntrack stays 0: DR does not need netfilter conntrack.
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root

ns=${1:-}; action=${2:-}; arg=${3:-}
[[ $ns == lb1 || $ns == lb2 ]] || die "usage: $0 <lb1|lb2> {up rr|mh|wrr | down | del IP | add IP | status}"
read -r -a reals <<<"${IPVS_REALS:-${DEFAULT_REALS[*]}}"
services=("-t $VIP:80" "-u $VIP:5000" "-t $VIP:7000")

add_real() {  # add_real <real-ip>
    local svc
    for svc in "${services[@]}"; do
        # shellcheck disable=SC2086
        nsx "$ns" ipvsadm -a $svc -r "$1" -g -w 1
    done
}

case $action in
up)
    sched=${arg:-mh}
    [[ $sched == rr || $sched == mh || $sched == wrr ]] || die "scheduler must be rr, mh or wrr"
    modprobe ip_vs && modprobe "ip_vs_$sched"
    nsx "$ns" ipvsadm -C
    nsx "$ns" sysctl -qw net.ipv4.vs.expire_nodest_conn=1 net.ipv4.vs.conntrack=0 \
        "net.ipv4.vs.sloppy_tcp=${IPVS_SLOPPY_TCP:-1}"
    flags=()
    [[ $sched == mh ]] && flags=(-b mh-port)
    for svc in "${services[@]}"; do
        # shellcheck disable=SC2086
        nsx "$ns" ipvsadm -A $svc -s "$sched" "${flags[@]}"
    done
    for r in "${reals[@]}"; do add_real "$r"; done
    nsx "$ns" ip addr replace "$VIP/32" dev lo
    log "ipvs up in $ns: sched=$sched ${flags[*]} reals=${reals[*]} sloppy_tcp=${IPVS_SLOPPY_TCP:-1}"
    ;;
down)
    nsx "$ns" ipvsadm -C 2>/dev/null || true
    nsx "$ns" ip addr del "$VIP/32" dev lo 2>/dev/null || true
    log "ipvs down in $ns"
    ;;
del)
    [[ -n $arg ]] || die "del needs a real IP"
    for svc in "${services[@]}"; do
        # shellcheck disable=SC2086
        nsx "$ns" ipvsadm -d $svc -r "$arg"
    done
    log "ipvs $ns: removed real $arg"
    ;;
add)
    [[ -n $arg ]] || die "add needs a real IP"
    add_real "$arg"
    log "ipvs $ns: added real $arg"
    ;;
status)
    nsx "$ns" ipvsadm -Ln --stats
    ;;
*) die "usage: $0 <lb1|lb2> {up rr|mh|wrr | down | del IP | add IP | status}" ;;
esac
