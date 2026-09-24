#!/usr/bin/env bash
# Kernel pktgen from a lab namespace (normally the client).
#
#   lab/pktgen.sh <netns> <dev> <dst_ip> <dst_port> <count|0> <flows> [rate_pps]
#       configure and run. Blocks until <count> packets per thread are sent;
#       with count 0 it runs until `lab/pktgen.sh --stop <netns>` (run it in
#       the background and stop it). Prints the per-thread pktgen result.
#   lab/pktgen.sh --stop <netns>     stop a running pktgen in that namespace
#   lab/pktgen.sh --sent <netns>     total packets sent so far (all threads)
#
# Env: PKTGEN_THREADS (default 1): kpktgend_0..N-1, one device clone each.
#      PKT_SIZE (default 60): pktgen's pkt_size is the frame WITHOUT the 4-byte
#        Ethernet FCS, so 60 = the classic 64-byte minimum frame on a wire.
#        veth carries no FCS, so the skb is 60 bytes (14 Eth + 20 IP + 8 UDP + 18).
#      DST_MAC: override the next-hop MAC. By default it is resolved from the
#        namespace's route to dst_ip (the LB's veth MAC for the VIP, the real's
#        MAC for a direct send), because pktgen writes the frame itself and
#        bypasses routing and ARP.
#
# Flows: the UDP source port walks udp_src_min..udp_src_min+flows-1
# sequentially (1024..), so exactly <flows> distinct 5-tuples are offered.
#
# pktgen is per network namespace (its /proc/net/pktgen and kpktgend threads
# belong to the netns), so every write happens inside `ip netns exec <netns>`.
# veth lacks IFF_TX_SKB_SHARING, so clone_skb must stay 0 and burst 1.
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root

PG=/proc/net/pktgen

pg_write() {  # pg_write <ns> <file> <command>
    local ns=$1 file=$2 cmd=$3
    ip netns exec "$ns" sh -c "echo '$cmd' > '$PG/$file'" ||
        die "pktgen: '$cmd' > $file failed"
    if [[ $file != pgctrl ]]; then
        local res
        res=$(ip netns exec "$ns" cat "$PG/$file" | sed -n 's/^Result: //p' | head -n1)
        [[ -z $res || $res == OK* ]] || die "pktgen: '$cmd' > $file: $res"
    fi
}

sent_total() {
    local ns=$1
    ip netns exec "$ns" sh -c "cat $PG/*@* 2>/dev/null" |
        awk '/pkts-sofar:/ { for (i = 1; i <= NF; i++) if ($i == "pkts-sofar:") s += $(i + 1) } END { print s + 0 }'
}

case ${1:-} in
--stop)
    ip netns exec "$2" sh -c "echo stop > $PG/pgctrl" 2>/dev/null || true
    exit 0 ;;
--sent)
    sent_total "$2"
    exit 0 ;;
esac

[[ $# -ge 6 ]] || die "usage: $0 <netns> <dev> <dst_ip> <dst_port> <count|0> <flows> [rate_pps]"
ns=$1 dev=$2 dst=$3 dport=$4 count=$5 flows=$6 rate=${7:-0}
threads=${PKTGEN_THREADS:-1}
size=${PKT_SIZE:-60}

modprobe pktgen 2>/dev/null || die "pktgen module unavailable (apt install linux-modules-extra-$(uname -r))"
ip netns exec "$ns" test -w "$PG/pgctrl" || die "no $PG/pgctrl in namespace $ns"

src=$(ip -n "$ns" -4 -o addr show dev "$dev" | awk '{print $4}' | cut -d/ -f1 | head -n1)
[[ -n $src ]] || die "$dev in $ns has no IPv4 address"
mac=${DST_MAC:-}
if [[ -z $mac ]]; then
    nh=$(ip -n "$ns" route get "$dst" | sed -n 's/.* via \([0-9.]*\).*/\1/p' | head -n1)
    nh=${nh:-$dst}
    ip netns exec "$ns" ping -c1 -W1 "$nh" >/dev/null 2>&1 || true
    mac=$(ip -n "$ns" neigh show "$nh" dev "$dev" | awk '{for (i = 1; i <= NF; i++) if ($i == "lladdr") print $(i + 1)}' | head -n1)
    [[ -n $mac ]] || die "cannot resolve the MAC of next hop $nh"
fi

pg_write "$ns" pgctrl stop
for t in $(ip netns exec "$ns" sh -c "ls $PG" | grep '^kpktgend_'); do pg_write "$ns" "$t" rem_device_all; done

per_thread_rate=0
((rate > 0)) && per_thread_rate=$(((rate + threads - 1) / threads))
for ((t = 0; t < threads; t++)); do
    d="$dev@$t"
    pg_write "$ns" "kpktgend_$t" "add_device $d"
    pg_write "$ns" "$d" "count $count"
    pg_write "$ns" "$d" "clone_skb 0"
    pg_write "$ns" "$d" "burst 1"
    pg_write "$ns" "$d" "pkt_size $size"
    pg_write "$ns" "$d" "delay 0"
    ((per_thread_rate > 0)) && pg_write "$ns" "$d" "ratep $per_thread_rate"
    pg_write "$ns" "$d" "dst_mac $mac"
    pg_write "$ns" "$d" "src_min $src"
    pg_write "$ns" "$d" "src_max $src"
    pg_write "$ns" "$d" "dst_min $dst"
    pg_write "$ns" "$d" "dst_max $dst"
    pg_write "$ns" "$d" "udp_dst_min $dport"
    pg_write "$ns" "$d" "udp_dst_max $dport"
    pg_write "$ns" "$d" "udp_src_min 1024"
    pg_write "$ns" "$d" "udp_src_max $((1024 + flows - 1))"
    ((threads > 1)) && pg_write "$ns" "$d" "flag QUEUE_MAP_CPU"
done

log "pktgen $ns: $threads thread(s) $dev -> $dst:$dport mac $mac, pkt_size $size, flows $flows, count $count, rate ${rate:-0} pps (0 = unthrottled)"
# Blocks until every device has sent `count` packets, or until --stop.
ip netns exec "$ns" sh -c "echo start > $PG/pgctrl" || true
for ((t = 0; t < threads; t++)); do
    ip netns exec "$ns" cat "$PG/$dev@$t" | sed -n '/^Result:/,$p'
done
