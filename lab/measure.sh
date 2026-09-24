#!/usr/bin/env bash
# Measure ONE configuration under a UDP packet flood and append one JSON row to
# results/<name>.jsonl. The forwarding plane must already be running (the
# caller, normally lab/experiments.sh, starts packetbalance or IPVS).
#
#   lab/measure.sh --name exp1_pps --lb packetbalance|ipvs-mh|ipvs-rr|none
#                  [--lbns lb1] [--xdp-mode native|generic] [--hash maglev|modulo]
#                  [--conntrack true|false] [--conntrack-size N]
#                  [--flows 10000] [--duration 30] [--rate PPS (0 = unthrottled)]
#                  [--warmup 3] [--repeat 1] [--experiment exp1] [--notes TEXT]
#
# Window: the generator starts, runs --warmup seconds, then every counter is
# sampled, `mpstat -P ALL 1 <duration>` runs for exactly the window, every
# counter is sampled again, and the generator stops. All rates are deltas over
# that window divided by its measured length.
#
#   offered_pps    pktgen pkts-sofar delta (sum over pktgen threads)
#   forwarded_pps  packetbalance: sum of the per-VIP `tx` counters (pbctl --json stats)
#                  ipvs-*:        sum of InPkts over services (ipvsadm -Ln --stats --exact);
#                                 in DR mode every accepted packet is forwarded
#                  none:          null
#   received_pps   rx_packets delta on veth0 of every real (/proc/net/dev in each netns)
#   lb_cpu_util    100 - %idle averaged over ALL CPUs of the VM. In the lab every
#                  namespace shares the VM's CPUs, so this includes the generator,
#                  the bridge and the reals, not only the LB. See results/README.md.
#   pps_per_core   forwarded_pps / (lb_cpu_util/100 * ncpu), an estimate, and a
#                  lower bound on the LB's own per-core rate for the same reason.
#   pps_per_core_received  the same with received_pps (what arrived at the reals)
#   pb_counters_delta / pb_drops_delta  packetbalance: every counter (global +
#                  VIPs) over the window, and the nonzero drop reasons
#   lb_veth_xdp_delta  the LB veth0's per-queue XDP stats summed (ethtool -S),
#                  e.g. xdp_tx_errors = XDP_TX frames the peer ring refused
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root
load_host_env

name=exp1_pps lb=none lbns=lb1 xdp_mode=n/a hash=n/a conntrack=n/a ct_size=null
flows=10000 duration=30 rate=0 warmup=3 repeat=1 experiment="" notes=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --name) name=$2 ;;
        --lb) lb=$2 ;;
        --lbns) lbns=$2 ;;
        --xdp-mode) xdp_mode=$2 ;;
        --hash) hash=$2 ;;
        --conntrack) conntrack=$2 ;;
        --conntrack-size) ct_size=$2 ;;
        --flows) flows=$2 ;;
        --duration) duration=$2 ;;
        --rate) rate=$2 ;;
        --warmup) warmup=$2 ;;
        --repeat) repeat=$2 ;;
        --experiment) experiment=$2 ;;
        --notes) notes=$2 ;;
        *) die "unknown argument $1" ;;
    esac
    shift 2
done
lab_is_up || die "lab is not up (lab/up.sh)"
mkdir -p "$RESULTS" "$RUN"

dst=$VIP
[[ $lb == none ]] && dst=$(real_ip 1)

# --- generator ------------------------------------------------------------------
generator=pktgen
if ! modprobe pktgen 2>/dev/null; then
    generator=udpblast
    UDPBLAST=$(find_bin udpblast) || die "pktgen unavailable and udpblast not built"
fi

read_forwarded() {
    case $lb in
        packetbalance)
            pbctl_ns "$lbns" --json stats | python3 -c '
import json, sys
r = json.load(sys.stdin)
vips = r.get("vips") or {}
tx = sum(int(v.get("tx", 0)) for v in vips.values())
if not vips:
    tx = int((r.get("global") or {}).get("tx", 0))
print(tx)' ;;
        ipvs-*)
            nsx "$lbns" ipvsadm -Ln --stats --exact | awk '$1 == "TCP" || $1 == "UDP" { s += $4 } END { print s + 0 }' ;;
        *) echo 0 ;;
    esac
}
# sample_forwarded: prints "<counter> <epoch>". IPVS on kernel >= 6.2 folds its
# per-CPU packet counters into what ipvsadm reports only when the estimator
# kthread (ipvs-e:*) visits the service, about every 2 s, so a plain read is up
# to 2 s stale (right after traffic starts it reads 0). We therefore wait for the
# reported value to change and timestamp that moment, which aligns both ends of
# the window to estimator ticks. See docs/bugs/lab.md.
sample_forwarded() {
    local v0 v i
    case $lb in
        ipvs-*)
            v0=$(read_forwarded)
            for ((i = 0; i < 80; i++)); do
                v=$(read_forwarded)
                [[ $v != "$v0" ]] && { echo "$v $(now)"; return; }
                sleep 0.05
            done
            echo "$v0 $(now)" ;;
        *) echo "$(read_forwarded) $(now)" ;;
    esac
}
read_sent() {
    if [[ $generator == pktgen ]]; then "$LAB_DIR/pktgen.sh" --sent client
    else
        # shellcheck disable=SC2016
        dev_tx=$(nsx client awk '$1 == "veth0:" { print $11 }' /proc/net/dev); echo "${dev_tx:-0}"
    fi
}
tun_rx_sum() {
    local s=0 ns tun v
    for ns in "${REAL_NS[@]}"; do
        tun=$(cat "$RUN/$ns/tundev" 2>/dev/null || echo tunl0)
        v=$(dev_rx_packets "$ns" "$tun"); s=$((s + v))
    done
    echo "$s"
}
now() { date +%s.%N; }
# Frames that reached a real's veth0 but were dropped there because the real's
# receive backlog was full (net.core.netdev_max_backlog; /proc/net/softnet_stat
# column 2 rises with it). They left the LB and crossed the bridge but never
# reached the real's IP stack, so they are NOT in received_pps.
reals_rx_dropped() {
    local s=0 ns v
    for ns in "${REAL_NS[@]}"; do v=$(nsx "$ns" cat /sys/class/net/veth0/statistics/rx_dropped); s=$((s + v)); done
    echo "$s"
}

cleanup() {
    if [[ $generator == pktgen ]]; then "$LAB_DIR/pktgen.sh" --stop client || true; fi
    [[ -n ${gen_pid:-} ]] && { kill "$gen_pid" 2>/dev/null || true; wait "$gen_pid" 2>/dev/null || true; }
    udp_echo_signal CONT
}
trap cleanup EXIT

# Contention guard (common.sh host_gate): wait for a quiet, fast VM before the
# generator starts; the canary is taken again after the generator stops, and
# every 10 s inside the window (informational: a loaded CPU slows it too).
read -r quiet_wait canary0 < <(host_gate)
udp_echo_signal STOP
log "measure: $name lb=$lb xdp=$xdp_mode hash=$hash ct=$conntrack/$ct_size flows=$flows rate=$rate dur=${duration}s repeat=$repeat gen=$generator"
if [[ $generator == pktgen ]]; then
    "$LAB_DIR/pktgen.sh" client veth0 "$dst" 5000 0 "$flows" "$rate" >"$RUN/pktgen.out" 2>&1 &
else
    nsx client "$UDPBLAST" --dst "$dst:5000" --flows "$flows" --size "${PKT_SIZE:-60}" \
        --threads "${PKTGEN_THREADS:-1}" --rate "$rate" --duration $((duration + warmup + 5)) \
        >"$RUN/pktgen.out" 2>&1 &
fi
gen_pid=$!
sleep "$warmup"
kill -0 "$gen_pid" 2>/dev/null || { cat "$RUN/pktgen.out" >&2; die "generator exited early"; }

# Full counter snapshots at both ends of the window: PacketBalance's per-reason
# counters (drops by reason must be on the row, not only tx) and the LB veth's
# XDP statistics (xdp_tx_errors: frames the program XDP_TX'd that the peer's
# ring refused; the program already counted them as tx).
# Plus every netdev on the path (packet accounting): rx/tx packets and dropped
# of client veth0, the bridge ports, br0, the LB veth0 and each real's veth0,
# and ethtool -S of the LB veth0 and of its bridge-side peer pb-<lbns>.
netdev_line() {  # netdev_line <ns|root> <dev>
    local ns=$1 dev=$2 f v out="$1:$2"
    for f in rx_packets rx_dropped tx_packets tx_dropped; do
        if [[ $ns == root ]]; then v=$(cat "/sys/class/net/$dev/statistics/$f" 2>/dev/null || echo 0)
        else v=$(nsx "$ns" cat "/sys/class/net/$dev/statistics/$f" 2>/dev/null || echo 0); fi
        out+=" $f=$v"
    done
    echo "$out"
}
snap() {  # snap <tag>
    if [[ $lb == packetbalance ]]; then pbctl_ns "$lbns" --json stats >"$RUN/pbstats.$1.json" 2>/dev/null || echo '{}' >"$RUN/pbstats.$1.json"
    else echo '{}' >"$RUN/pbstats.$1.json"; fi
    nsx "$lbns" ethtool -S veth0 >"$RUN/ethtool.$1.txt" 2>/dev/null || : >"$RUN/ethtool.$1.txt"
    ethtool -S "pb-$lbns" >"$RUN/ethtool-peer.$1.txt" 2>/dev/null || : >"$RUN/ethtool-peer.$1.txt"
    {
        netdev_line client veth0
        netdev_line "$lbns" veth0
        local d ns
        for d in pb-client "pb-$lbns" "$BR"; do netdev_line root "$d"; done
        for ns in "${REAL_NS[@]}"; do netdev_line root "pb-$ns"; netdev_line "$ns" veth0; done
        awk '{ d += strtonum("0x" $2) } END { print "softnet_dropped " d + 0 }' /proc/net/softnet_stat
    } >"$RUN/netdev.$1.txt"
}
# Count seconds with a build running in the VM, and sample the canary.
( n=0; cs=""
  for ((i = 1; i <= duration; i++)); do
      [[ $(vm_busy_procs) -gt 0 ]] && n=$((n + 1))
      if ((i % 10 == 0 && i < duration)); then cs+="$(cpu_canary) "; else sleep 1; fi
  done
  echo "$n" >"$RUN/busy.out"; echo "$cs" >"$RUN/canary.out" ) &
busy_pid=$!
snap 0
read -r f0 tf0 < <(sample_forwarded)
t0=$(now); s0=$(read_sent); r0=$(reals_rx_sum veth0 "${REAL_NS[@]}"); u0=$(tun_rx_sum); d0=$(reals_rx_dropped)
# mpstat covers exactly the window: <duration> one-second samples, then the
# "Average:" rows are used. The sample count is checked below.
LC_ALL=C mpstat -P ALL 1 "$duration" >"$RUN/mpstat.out"
t1=$(now); s1=$(read_sent); r1=$(reals_rx_sum veth0 "${REAL_NS[@]}"); u1=$(tun_rx_sum); d1=$(reals_rx_dropped)
read -r f1 tf1 < <(sample_forwarded)
snap 1
wait "$busy_pid" 2>/dev/null || true
busy_s=$(cat "$RUN/busy.out" 2>/dev/null || echo null)
canary_mid=$(cat "$RUN/canary.out" 2>/dev/null || true)
cleanup
trap - EXIT
canary1=$(cpu_canary)

out="$RESULTS/$name.jsonl"
T0=$t0 T1=$t1 TF0=$tf0 TF1=$tf1 S0=$s0 S1=$s1 F0=$f0 F1=$f1 R0=$r0 R1=$r1 U0=$u0 U1=$u1 D0=$d0 D1=$d1 \
LB=$lb XDP=$xdp_mode HASH=$hash CT=$conntrack CTSIZE=$ct_size FLOWS=$flows DUR=$duration \
RATE=$rate REPEAT=$repeat LBNS=$lbns VARIANT=${MEASURE_VARIANT:-standard} EXPERIMENT=${experiment:-${name%%_*}} NOTES=$notes GEN=$generator \
HOSTDESC=$LAB_HOST DST=$dst PKTSIZE=${PKT_SIZE:-60} THREADS=${PKTGEN_THREADS:-1} MPSTAT="$RUN/mpstat.out" RUNDIR=$RUN \
QUIET_WAIT=$quiet_wait BUSY_S=$busy_s CANARY="$canary0 $canary1" CANARY_MID="$canary_mid" CANARY_MIN=$CANARY_MIN \
python3 - >"$RUN/row.json" <<'PY'
import datetime, json, os, platform
e = os.environ
win = float(e["T1"]) - float(e["T0"])
def rate(a, b):
    return round((int(e[b]) - int(e[a])) / win, 1)
ncpu = os.cpu_count()
per_cpu, util, soft = {}, None, None
for line in open(e["MPSTAT"]):
    f = line.split()
    if len(f) > 3 and f[0] == "Average:" and f[1] != "CPU":
        busy = round(100.0 - float(f[-1]), 2)
        if f[1] == "all":
            util, soft = busy, float(f[7])
        else:
            per_cpu[f[1]] = busy
lb = e["LB"]
offered = rate("S0", "S1")
received = rate("R0", "R1")
fwin = float(e["TF1"]) - float(e["TF0"])
forwarded = None if lb == "none" else round((int(e["F1"]) - int(e["F0"])) / fwin, 1)
basis = received if forwarded is None else forwarded
ppc = ppc_rx = None
if util:
    ppc = round(basis / (util / 100.0 * ncpu), 1)
    ppc_rx = round(received / (util / 100.0 * ncpu), 1)
canary = [int(x) for x in e["CANARY"].split()]
busy = None if e["BUSY_S"] == "null" else int(e["BUSY_S"])
gate = "pass" if canary and min(canary) >= int(e["CANARY_MIN"]) and not busy else "fail"
mpstat_samples = sum(1 for line in open(e["MPSTAT"])
                     if len(line.split()) > 3 and line.split()[1] == "all" and line.split()[0] != "Average:")

def pb_counters(path):
    try:
        r = json.load(open(os.path.join(e["RUNDIR"], path)))
    except (OSError, ValueError):
        return {}
    tot = {}
    for c in [r.get("global") or {}] + list((r.get("vips") or {}).values()):
        for k, v in c.items():
            tot[k] = tot.get(k, 0) + int(v)
    return tot
pb0, pb1 = pb_counters("pbstats.0.json"), pb_counters("pbstats.1.json")
pb_delta = {k: pb1[k] - pb0.get(k, 0) for k in pb1} if pb1 else None
pb_drops = {k: v for k, v in (pb_delta or {}).items() if k.startswith("drop") and v} if pb1 else None

def ethtool(path):
    tot = {}
    try:
        for line in open(os.path.join(e["RUNDIR"], path)):
            k, _, v = line.strip().partition(": ")
            if k.startswith("rx_queue_") and v.strip().isdigit():
                name = k.split("_", 3)[3]          # rx_queue_3_xdp_tx -> xdp_tx
                tot[name] = tot.get(name, 0) + int(v)
    except OSError:
        pass
    return tot
et0, et1 = ethtool("ethtool.0.txt"), ethtool("ethtool.1.txt")
lb_veth_xdp = {k: et1[k] - et0.get(k, 0) for k in et1 if k.startswith("xdp")} or None
ep0, ep1 = ethtool("ethtool-peer.0.txt"), ethtool("ethtool-peer.1.txt")
peer_xdp = {k: ep1[k] - ep0.get(k, 0) for k in ep1 if k.startswith("xdp")} or None

def netdev(path):
    out = {}
    try:
        for line in open(os.path.join(e["RUNDIR"], path)):
            f = line.split()
            if f[0] == "softnet_dropped":
                out["softnet_dropped"] = int(f[1])
                continue
            out[f[0]] = {k: int(v) for k, v in (x.split("=") for x in f[1:])}
    except OSError:
        pass
    return out
nd0, nd1 = netdev("netdev.0.txt"), netdev("netdev.1.txt")
netdev_delta = {}
for k, v in nd1.items():
    if isinstance(v, dict):
        netdev_delta[k] = {f: v[f] - nd0.get(k, {}).get(f, 0) for f in v}
    else:
        netdev_delta[k] = v - nd0.get(k, 0)

# Packet accounting over the window, for PacketBalance: every frame the
# program XDP_TX'd (its tx counter) must be received by a real or dropped at a
# named place. Counts, not rates. The windows of the stats snapshot and the
# netdev snapshot differ by a few ms, so "unaccounted" is small but not 0.
acct = None
lbns = e["LBNS"]
if pb_delta and netdev_delta:
    reals = [k for k in netdev_delta if k.startswith("real") and k.endswith(":veth0")]
    rx_reals = sum(netdev_delta[k]["rx_packets"] for k in reals)
    acct = {
        # before the program: the bridge-side peer could not hand the frame to
        # the LB veth (its receive ring was full); the program never saw it
        "offered_by_client": netdev_delta.get("client:veth0", {}).get("tx_packets", 0),
        "dropped_before_lb_program": netdev_delta.get("root:pb-" + lbns, {}).get("tx_dropped", 0),
        "pb_tx": pb_delta.get("tx", 0),
        "received_at_reals": rx_reals,
        # the LB veth could not hand an XDP_TX'd / transmitted frame to its
        # peer (peer ring full). One event, three counters: lb veth0
        # tx_dropped (native and generic), lb veth0 xdp_tx_errors (native
        # only) and pb-<lbns> rx_dropped. Only lb_veth_tx_dropped is subtracted.
        "lb_veth_tx_dropped": netdev_delta.get(lbns + ":veth0", {}).get("tx_dropped", 0),
        "same_event_lb_veth_xdp_tx_errors": (lb_veth_xdp or {}).get("xdp_tx_errors", 0),
        "same_event_peer_rx_dropped": netdev_delta.get("root:pb-" + lbns, {}).get("rx_dropped", 0),
        "peer_xdp_drops": (peer_xdp or {}).get("xdp_drops", 0),
        # the real's receive backlog was full. Same event as the bridge port's
        # tx_dropped toward that real; subtracted once.
        "reals_rx_dropped": sum(netdev_delta[k]["rx_dropped"] for k in reals),
        "non_vip_pass": pb_delta.get("pass", 0),
    }
    acct["unaccounted"] = (acct["pb_tx"] - acct["received_at_reals"] - acct["lb_veth_tx_dropped"]
                           - acct["peer_xdp_drops"] - acct["reals_rx_dropped"])
def num(x):
    return None if x in ("null", "", "n/a") else int(x)
def boolish(x):
    return {"true": True, "false": False}.get(x, None)
row = {
    "experiment": e["EXPERIMENT"],
    "host": e["HOSTDESC"],
    "cpus": ncpu,
    "kernel": platform.release(),
    "arch": platform.machine(),
    "generator": e["GEN"],
    "generator_threads": int(e["THREADS"]),
    "lb": lb,
    "xdp_mode": None if e["XDP"] == "n/a" else e["XDP"],
    "hash": None if e["HASH"] == "n/a" else e["HASH"],
    "conntrack": boolish(e["CT"]),
    "conntrack_size": num(e["CTSIZE"]),
    "pkt_size": int(e["PKTSIZE"]) + 4,
    "pkt_size_skb": int(e["PKTSIZE"]),
    "flows": int(e["FLOWS"]),
    "dst": e["DST"],
    "target_rate_pps": int(e["RATE"]),
    "duration_s": round(win, 3),
    "forwarded_window_s": None if lb == "none" else round(fwin, 3),
    "offered_pps": offered,
    "forwarded_pps": forwarded,
    "received_pps": received,
    "received_tunnel_pps": rate("U0", "U1"),
    "reals_rx_dropped_pps": rate("D0", "D1"),
    "lb_cpu_util": util,
    "softirq_util": soft,
    "per_cpu_util": per_cpu,
    "pps_per_core": ppc,
    "pps_per_core_basis": "received_pps" if forwarded is None else "forwarded_pps",
    "pps_per_core_received": ppc_rx,
    "mpstat_samples": mpstat_samples,
    "pb_counters_delta": pb_delta,
    "pb_drops_delta": pb_drops,
    "lb_veth_xdp_delta": lb_veth_xdp,
    "lb_veth_xdp_tx_errors": (lb_veth_xdp or {}).get("xdp_tx_errors"),
    "lb_veth_xdp_drops": (lb_veth_xdp or {}).get("xdp_drops"),
    "lb_veth_tx_dropped_pps": round(netdev_delta.get(e["LBNS"] + ":veth0", {}).get("tx_dropped", 0) / win, 1) if netdev_delta else None,
    "peer_rx_dropped": netdev_delta.get("root:pb-" + e["LBNS"], {}).get("rx_dropped") if netdev_delta else None,
    "peer_xdp_delta": peer_xdp,
    "netdev_delta": netdev_delta or None,
    "packet_accounting": acct,
    "vm_build_seconds_in_window": busy,
    "quiet_wait_s": int(e["QUIET_WAIT"]),
    "cpu_canary": canary,
    "cpu_canary_in_window": [int(x) for x in e["CANARY_MID"].split()],
    "cpu_canary_min_required": int(e["CANARY_MIN"]),
    "host_gate": gate,
    "variant": e["VARIANT"],
    "date": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    "repeat": int(e["REPEAT"]),
    "notes": e["NOTES"],
}
print(json.dumps(row))
PY
# A window that failed the host gate goes to results/rejected.jsonl (kept, not
# hidden) and the exit status is 3 so the caller can retry it.
if python3 -c 'import json, sys; sys.exit(0 if json.load(open(sys.argv[1]))["host_gate"] == "pass" else 1)' "$RUN/row.json"; then
    cat "$RUN/row.json" >>"$out"
    tail -n1 "$out" >&2
else
    python3 -c 'import json, sys; r = json.load(open(sys.argv[1])); r["rejected_from"] = sys.argv[2]; print(json.dumps(r))' \
        "$RUN/row.json" "$name.jsonl" >>"$RESULTS/rejected.jsonl"
    log "host gate FAILED (canary $(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["cpu_canary"])' "$RUN/row.json") < $CANARY_MIN or a build ran); row written to results/rejected.jsonl"
    exit 3
fi
