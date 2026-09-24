#!/usr/bin/env bash
# MTU / MSS check through whatever forwarding plane owns the VIP on <lb>.
#
#   sudo lab/mtucheck.sh [lb1|lb2] [--no-route-mtu]
#
# 1. GET a 100 KB file through the VIP (response path: real -> client, DSR).
# 2. PUT 1 MB through the VIP (request path: client -> LB -> IPIP -> real), so
#    the client sends full-size segments that the LB must encapsulate. With the
#    client route's mtu 1480 (common.sh ROUTE_MTU) the MSS is 1440 and an
#    encapsulated segment is exactly 1500 bytes. --no-route-mtu removes that mtu
#    for the duration of the check to show the failure it prevents (the PMTU
#    black hole in docs/DESIGN.md), then restores it.
# Prints the largest IPIP frame seen on every real and PacketBalance drop_mtu.
set -uo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root
lb=${1:-lb1}; nomtu=${2:-}
gw=$(ns_ip "$lb")
if [[ $nomtu == --no-route-mtu ]]; then
    nsx client ip route replace "$VIP_NET" via "$gw" dev veth0
    nsx client ip route flush cache
    log "client route to $VIP_NET via $gw WITHOUT mtu"
else
    route_via "$lb"
fi
body=$RUN/mtucheck.1m
head -c 1048576 /dev/urandom >"$body"
for r in "${REAL_NS[@]}"; do
    nsx "$r" timeout 8 tcpdump -lni veth0 ip proto 4 >"$RUN/mtucheck.$r.txt" 2>/dev/null &
done
sleep 0.5
fail=0
for i in 1 2 3 4; do
    out=$(nsx client curl -s -o /dev/null --max-time 5 -w '%{http_code} %{size_download} %{time_total}' "http://$VIP/100k.bin") || true
    echo "GET 100k.bin: $out"; [[ $out == "200 102400 "* ]] || fail=1
done
for i in 1 2 3 4; do
    out=$(nsx client curl -s -o /dev/null --max-time 5 -w '%{http_code} %{size_upload} %{time_total}' \
        -T "$body" "http://$VIP/upload/mtucheck-$i.bin") || true
    echo "PUT 1 MB: $out"; [[ $out == 20[14]" 1048576 "* ]] || fail=1
done
wait 2>/dev/null
max=0
for r in "${REAL_NS[@]}"; do
    # tcpdump prints the inner segment length; the outer IP packet is 20 (outer
    # IP) + 20 (inner IP) + TCP header (32 with timestamps) + that length.
    m=$(grep -o 'length [0-9]*' "$RUN/mtucheck.$r.txt" | awk '{print $2}' | sort -n | tail -n1)
    echo "$r: largest encapsulated TCP payload ${m:-none}"
    [[ -n $m && $m -gt $max ]] && max=$m
    rm -f "$RUN/$r/www/upload/"mtucheck-*.bin
done
rm -f "$body" "$RUN"/mtucheck.real*.txt
if pbctl_ns "$lb" ping >/dev/null 2>&1; then
    pbctl_ns "$lb" --json stats | python3 -c '
import json, sys
r = json.load(sys.stdin)
d = {k: sum(int(c.get(k, 0)) for c in [r.get("global", {})] + list((r.get("vips") or {}).values()))
     for k in ("drop_mtu", "drop_adj_head", "drop_other", "tx")}
print("packetbalance counters:", d)'
fi
[[ $nomtu == --no-route-mtu ]] && route_via "$lb"
if [[ $fail == 0 ]]; then echo "mtucheck: PASS (largest encapsulated payload $max)"; else echo "mtucheck: FAIL"; fi
exit $fail
