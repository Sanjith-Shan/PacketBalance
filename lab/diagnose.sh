#!/usr/bin/env bash
# "The VIP is black-holing" runbook, automated. Read-only apart from one curl
# through the VIP and a 3-second tcpdump.
#
#   sudo lab/diagnose.sh [lb1|lb2]      (default lb1)
#
# Every line is PASS, FAIL or INFO. Walks the packet path in order: client
# route -> LB attach and maps -> LB counters -> reals' DSR config -> the wire.
set -uo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root

lb=${1:-lb1}
[[ $lb == lb1 || $lb == lb2 ]] || die "usage: $0 [lb1|lb2]"
fails=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; fails=$((fails + 1)); }
info() { printf 'INFO  %s\n' "$*"; }
indent() { sed 's/^/        /'; }

lab_is_up || { fail "lab is not up (no client namespace or $BR): run lab/up.sh"; exit 1; }

# --- 1. client route ------------------------------------------------------------
route=$(nsx client ip route get "$VIP" 2>&1)
if [[ $route == *"via $(ns_ip "$lb")"* ]]; then pass "client routes $VIP via $lb: $route"
else info "client route to $VIP does not point at $lb: $route"; fi

# --- 2. which forwarding plane owns the VIP on the LB -----------------------------
xdp_line=$(nsx "$lb" ip -d link show dev veth0 | grep -Eo 'prog/xdp[a-z]* id [0-9]+|xdp(generic|drv|offload)?' | head -n1)
ipvs_rules=$(nsx "$lb" ipvsadm -Ln 2>/dev/null | grep -cE '^(TCP|UDP)' || true)
if [[ -n $xdp_line ]]; then
    pass "$lb veth0 has an XDP program attached ($xdp_line)"
    plane=packetbalance
elif [[ ${ipvs_rules:-0} -gt 0 ]]; then
    info "$lb has no XDP program; IPVS has $ipvs_rules services"
    plane=ipvs
else
    fail "$lb has neither an XDP program on veth0 nor IPVS services: nothing forwards the VIP"
    plane=none
fi
nsx "$lb" ip -d link show dev veth0 | indent
if [[ -n $xdp_line && ${ipvs_rules:-0} -gt 0 ]]; then
    fail "$lb has BOTH XDP and IPVS configured; only one plane should own the VIP"
fi

if [[ $plane == packetbalance ]]; then
    info "bpftool prog show (xdp programs):"
    bpftool prog show 2>/dev/null | grep -A2 -E 'xdp' | indent
    pin=$(pb_pin "$lb")
    for m in vip_map reals neigh; do
        if [[ -e $pin/$m ]]; then
            n=$(bpftool map dump pinned "$pin/$m" 2>/dev/null | grep -c '^key' || true)
            info "map $m ($pin/$m): $n entries shown by bpftool map dump"
        else
            fail "pinned map $pin/$m does not exist"
        fi
    done
    if [[ -e $pin/reals ]]; then
        bpftool map dump pinned "$pin/reals" 2>/dev/null | grep -B1 -A3 -v 'value: 00 00 00 00 00 00 00 00' | head -n 20 | indent
    fi
    if pbctl_ns "$lb" ping >/dev/null 2>&1; then
        pass "daemon in $lb answers ping on $(pb_sock "$lb")"
        stats=$(pbctl_ns "$lb" --json stats 2>/dev/null)
        echo "$stats" | python3 -c '
import json, sys
r = json.load(sys.stdin)
def show(scope, c):
    drops = {k: v for k, v in c.items() if k.startswith("drop") and v}
    tag = "FAIL" if drops else "PASS"
    print("%s  %s: packets=%s tx=%s drops=%s" % (tag, scope, c.get("packets"), c.get("tx"), drops or 0))
show("global", r.get("global", {}))
for vip, c in (r.get("vips") or {}).items():
    show(vip, c)
' 2>/dev/null || info "could not parse pbctl stats"
        info "pbctl health:"
        pbctl_ns "$lb" health 2>&1 | indent
    else
        fail "daemon socket $(pb_sock "$lb") does not answer (maps and XDP may still be attached; stats unavailable)"
    fi
elif [[ $plane == ipvs ]]; then
    nsx "$lb" ipvsadm -Ln --stats | indent
    if nsx "$lb" ip -4 addr show dev lo | grep -q "$VIP/32"; then pass "$lb has the VIP on lo (IPVS DR needs it)"
    else fail "$lb has IPVS services but no VIP on lo: the stack will not accept VIP packets"; fi
fi

# --- 3. the reals ---------------------------------------------------------------
for ns in real1 real2 real3 real4; do
    tun=$(cat "$RUN/$ns/tundev" 2>/dev/null || echo tunl0)
    bad=""
    for k in all default lo veth0 "$tun"; do
        v=$(nsx "$ns" sysctl -n "net.ipv4.conf.$k.rp_filter" 2>/dev/null || echo "?")
        [[ $v == 0 ]] || bad+="$k=$v "
    done
    if [[ -z $bad ]]; then pass "$ns rp_filter is 0 on all, default, lo, veth0, $tun"
    else fail "$ns rp_filter not 0: $bad(decapsulated packets arrive on $tun but route back via veth0)"; fi
    if nsx "$ns" ip -4 addr show dev lo | grep -q "$VIP/32"; then pass "$ns has $VIP/32 on lo"
    else fail "$ns is missing $VIP/32 on lo"; fi
    if nsx "$ns" ip link show "$tun" 2>/dev/null | grep -q '<[^>]*UP'; then pass "$ns $tun is up"
    else fail "$ns $tun is missing or down (no IPIP decapsulation)"; fi
    ai=$(nsx "$ns" sysctl -n net.ipv4.conf.all.arp_ignore); aa=$(nsx "$ns" sysctl -n net.ipv4.conf.all.arp_announce)
    if [[ $ai == 1 && $aa == 2 ]]; then pass "$ns arp_ignore=1 arp_announce=2"
    else fail "$ns arp_ignore=$ai arp_announce=$aa (a real may answer ARP for the VIP)"; fi
    if nsx "$ns" ss -ltn 'sport = :80' | grep -q LISTEN; then pass "$ns nginx listens on :80"
    else fail "$ns nothing listens on :80"; fi
done

# --- 4. ARP from the LB toward the reals -----------------------------------------
for n in 1 2 3 4; do
    ip=$(real_ip "$n"); want=$(ns_mac "real$n")
    nsx "$lb" ping -c1 -W1 "$ip" >/dev/null 2>&1 || true
    got=$(nsx "$lb" ip neigh show "$ip" dev veth0 | awk '{for (i = 1; i <= NF; i++) if ($i == "lladdr") print $(i + 1)}')
    if [[ $got == "$want" ]]; then pass "$lb resolves real$n $ip -> $got"
    else fail "$lb neighbor entry for $ip is '${got:-none}', expected $want"; fi
done

# --- 5. the wire: one request through the VIP, watched on real1 -------------------
cap="$RUN/diag.pcap.txt"
mkdir -p "$RUN"
filter='ip proto 4'
[[ $plane == ipvs ]] && filter="tcp port 80 and dst host $VIP"
nsx real1 timeout 3 tcpdump -ni veth0 -c 50 "$filter" >"$cap" 2>/dev/null &
tpid=$!
sleep 0.5
body=""
for _ in 1 2 3 4 5 6 7 8; do
    b=$(nsx client curl -s --max-time 1 "http://$VIP/" 2>/dev/null) && body+="${b}; "
done
wait "$tpid" 2>/dev/null
seen=$(grep -c . "$cap" || true)
if [[ -n $body ]]; then pass "curl http://$VIP/ answered: $body"
else fail "curl http://$VIP/ got no answer"; fi
if [[ $seen -gt 0 ]]; then pass "tcpdump on real1 veth0 saw $seen packets matching '$filter' in 3 s"
else info "tcpdump on real1 veth0 saw no '$filter' packets in 3 s (fine if the hash sent all 8 requests elsewhere)"; fi
head -n 4 "$cap" | indent

echo
if [[ $fails -eq 0 ]]; then echo "diagnose: all checks passed"; else echo "diagnose: $fails check(s) FAILED"; fi
exit $((fails > 0))
