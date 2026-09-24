#!/usr/bin/env bash
# Tear down everything lab/up.sh created, and nothing else: processes running
# inside the lab namespaces (daemons, nginx, echo servers, conncheck), pktgen
# devices in the client namespace, IPVS tables in lb1/lb2, the namespaces
# client lb1 lb2 real1..real5, the root-side pb-* veths, br0, /run/pblab and
# the lab daemons' pin directories /sys/fs/bpf/packetbalance/{lb1,lb2}.
# Other pin directories (for example another developer's daemon pinned at
# /sys/fs/bpf/packetbalance itself, or the pbdev namespace) are left alone.
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root

for ns in "${ALL_NS[@]}"; do
    ip netns list | grep -qw "$ns" || continue
    if [[ $ns == client && -d /proc/net ]]; then
        # shellcheck disable=SC2016
        # pktgen is per namespace; stop any run before the namespace goes away.
        nsx client sh -c 'if [ -w /proc/net/pktgen/pgctrl ]; then echo stop > /proc/net/pktgen/pgctrl;
            for t in /proc/net/pktgen/kpktgend_*; do echo rem_device_all > "$t"; done; fi' 2>/dev/null || true
    fi
    if [[ $ns == lb1 || $ns == lb2 ]]; then
        nsx "$ns" ipvsadm -C 2>/dev/null || true
        nsx "$ns" ip link set dev veth0 xdp off 2>/dev/null || true
        nsx "$ns" ip link set dev veth0 xdpgeneric off 2>/dev/null || true
    fi
    pids=$(ip netns pids "$ns" 2>/dev/null || true)
    if [[ -n $pids ]]; then
        log "kill in $ns: $(echo "$pids" | tr '\n' ' ')"
        # shellcheck disable=SC2086
        kill -TERM $pids 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            pids=$(ip netns pids "$ns" 2>/dev/null || true)
            [[ -z $pids ]] && break
            sleep 0.2
        done
        # shellcheck disable=SC2086
        [[ -n $pids ]] && kill -KILL $pids 2>/dev/null || true
    fi
    run ip netns del "$ns"
done

for dev in $(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | cut -d@ -f1 | grep -E '^pb-(client|lb[12]|real[1-5])$' || true); do
    run ip link del "$dev"
done
if ip link show "$BR" >/dev/null 2>&1; then run ip link del "$BR"; fi
for ns in "${LB_NS[@]}"; do
    [[ -e $PIN_ROOT/$ns ]] && run rm -rf "${PIN_ROOT:?}/$ns"
done
[[ -d $RUN ]] && run rm -rf "$RUN"
log "lab is down"
