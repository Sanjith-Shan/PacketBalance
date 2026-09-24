#!/usr/bin/env bash
# Run the PacketBalance experiments from the spec and append JSON rows to
# results/*.jsonl. Every configuration is run REPEATS times (default 3).
#
#   sudo lab/experiments.sh [exp1|exp2|exp3|exp4|exp6|restart|all] ...
#
# Env:
#   REPEATS=3          repeats per configuration
#   DURATION=30        seconds per Exp 1 / Exp 6 window and per wrk run
#   EXP1_SWEEP=1       also run the offered-load sweep (results/exp1_sweep.jsonl)
#   EXP1_RATES="..."   rates for the sweep, pps, 0 = unthrottled
#   EXP_LBS="..."      only run these LB labels, e.g. "ipvs-mh ipvs-rr none".
#                      Labels: packetbalance-native packetbalance-generic
#                      packetbalance-noct packetbalance-modulo ipvs-mh ipvs-rr ipvs-mh-tun none
#   CONNS=10000        conncheck connections for Exp 3 and 4
#   PB_XDP_MODE=native XDP mode for Exp 2, 3, 4, 6
#   NOTES="..."        appended to every row's notes
#   PKTGEN_THREADS=4   pktgen kernel threads for Exp 1 and 6
#   EXP6_DONE="1:false:1048576 ..."  Exp 6 windows (repeat:conntrack:size) to skip when resuming
#
# PacketBalance configurations are skipped (with a log line) when the daemon or
# pbctl is not built. Only one forwarding plane owns the VIP at any time: every
# configuration starts from all_lbs_off (no daemon, no IPVS, no VIP on lo).
# Ctrl-C stops everything and runs lab/down.sh.
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root
load_host_env
export LAB_HOST

REPEATS=${REPEATS:-3}
DURATION=${DURATION:-30}
CONNS=${CONNS:-10000}
PB_XDP_MODE=${PB_XDP_MODE:-native}
EXP1_SWEEP=${EXP1_SWEEP:-1}
EXP1_RATES=${EXP1_RATES:-"100000 250000 500000 1000000 2000000 4000000 0"}
NOTES=${NOTES:-}
# Four pktgen threads saturate the 6-vCPU lab VM through IPVS; one thread is
# generator bound (it stops at one CPU). Recorded on every row.
export PKTGEN_THREADS=${PKTGEN_THREADS:-4}
EXP_LBS=${EXP_LBS:-}
LABUTIL="python3 $LAB_DIR/labutil.py"
VIP_SPECS=("$VIP:80/tcp" "$VIP:5000/udp" "$VIP:7000/tcp")

# ---------------------------------------------------------------------------
# Safety: stop every generator and forwarding plane on exit, full down on ^C.
# ---------------------------------------------------------------------------
bg_pids=()
stop_everything() {
    local p
    for p in "${bg_pids[@]}"; do kill "$p" 2>/dev/null || true; done
    "$LAB_DIR/pktgen.sh" --stop client >/dev/null 2>&1 || true
    if lab_is_up; then
        udp_echo_signal CONT
        all_lbs_off
        route_via lb1 2>/dev/null || true
    fi
}
on_interrupt() {
    log "interrupted: stopping everything and tearing the lab down"
    trap - EXIT INT TERM
    stop_everything
    "$LAB_DIR/down.sh" || true
    exit 130
}
trap stop_everything EXIT
trap on_interrupt INT TERM

lab_is_up || "$LAB_DIR/up.sh"

have_pb() { find_bin packetbalance >/dev/null && find_bin pbctl >/dev/null; }
wanted() {  # wanted <label>: EXP_LBS filter + PacketBalance availability
    local label=$1
    if [[ -n $EXP_LBS && " $EXP_LBS " != *" $label "* ]]; then return 1; fi
    if [[ $label == packetbalance* ]] && ! have_pb; then
        log "SKIP $label: packetbalance/pbctl not built"
        return 1
    fi
    return 0
}
sleep_until() {  # sleep_until <epoch seconds, fractional ok>
    local d
    d=$(python3 -c "import time; print(max(0.0, $1 - time.time()))")
    sleep "$d"
}
append() { local file=$1; shift; $LABUTIL row "$@" >>"$RESULTS/$file"; tail -n1 "$RESULTS/$file" >&2; }

# measure: lab/measure.sh, retried when the window fails the host gate (exit 3,
# row in results/rejected.jsonl), up to MEASURE_TRIES windows in total. Any
# other failure is logged and the experiment moves on.
MEASURE_TRIES=${MEASURE_TRIES:-4}
measure() {
    local try rc
    for ((try = 1; try <= MEASURE_TRIES; try++)); do
        rc=0; "$LAB_DIR/measure.sh" "$@" || rc=$?
        [[ $rc == 3 ]] || break
        log "window failed the host gate (try $try of $MEASURE_TRIES), retrying"
    done
    [[ $rc == 0 ]] || log "WARN: measure.sh exited $rc"
    return 0
}

# check_vip: the VIP answers HTTP through whatever is configured. Prints the body.
check_vip() { nsx client curl -s --max-time 3 "http://$VIP/" || echo "FAILED"; }

# ---------------------------------------------------------------------------
# Forwarding-plane setup. Each returns non-zero if the plane did not come up.
# ---------------------------------------------------------------------------
# setup_pb <ns> <xdp_mode> <hash> <conntrack true|false> <ct_size> [extra reals...]
setup_pb() {
    local ns=$1 mode=$2 hash=$3 ct=$4 ctsize=$5; shift 5
    local flags=(--detach-on-exit)
    [[ $hash != maglev ]] && flags+=(--hash "$hash")
    [[ $ct == false ]] && flags+=(--no-conntrack)
    flags+=(--conntrack-size "$ctsize")
    pb_stop "$ns"
    ipvs "$ns" down >/dev/null
    write_pb_config "$ns" "$hash" "$ct" "$ctsize" "$@" >/dev/null
    pb_start "$ns" "$mode" "${flags[@]}"
}
setup_ipvs() {  # setup_ipvs <ns> <sched>[-tun] [extra reals...]
    local ns=$1 sched=${2%-tun} fwd=dr
    [[ $2 == *-tun ]] && fwd=tun
    shift 2
    pb_stop "$ns"
    IPVS_REALS="${DEFAULT_REALS[*]} $*" ipvs "$ns" up "$sched" "$fwd"
}

# label -> "plane xdp_mode hash conntrack"
describe() {
    case $1 in
        packetbalance-native)  echo "packetbalance native maglev true" ;;
        packetbalance-generic) echo "packetbalance generic maglev true" ;;
        packetbalance-noct)    echo "packetbalance $PB_XDP_MODE maglev false" ;;
        packetbalance-modulo)  echo "packetbalance $PB_XDP_MODE modulo true" ;;
        packetbalance)         echo "packetbalance $PB_XDP_MODE maglev true" ;;
        ipvs-mh)               echo "ipvs-mh n/a mh true" ;;
        ipvs-mh-tun)           echo "ipvs-mh-tun n/a mh true" ;;
        ipvs-rr)               echo "ipvs-rr n/a rr true" ;;
        none)                  echo "none n/a n/a n/a" ;;
    esac
}

# setup_label <label> <ns> [ct_size] [extra reals...]
setup_label() {
    local label=$1 ns=$2 ctsize=${3:-1048576}; shift 3 || shift $#
    local plane mode hash ct
    read -r plane mode hash ct <<<"$(describe "$label")"
    case $plane in
        packetbalance) setup_pb "$ns" "$mode" "$hash" "$ct" "$ctsize" "$@" ;;
        ipvs-*) setup_ipvs "$ns" "${plane#ipvs-}" "$@" ;;
        none) pb_stop "$ns"; ipvs "$ns" down >/dev/null ;;
    esac
}

# real_del/real_add <label> <ns> <addr>: the Exp 3 churn events
real_del() {
    local label=$1 ns=$2 addr=$3 v
    case $label in
        packetbalance*) for v in "${VIP_SPECS[@]}"; do pbctl_ns "$ns" real del "$v" "$addr"; done ;;
        ipvs-*) ipvs "$ns" del "$addr" ;;
    esac
}
real_add() {
    local label=$1 ns=$2 addr=$3 v
    case $label in
        packetbalance*) for v in "${VIP_SPECS[@]}"; do pbctl_ns "$ns" real add "$v" "$addr" --weight 1; done ;;
        ipvs-*) ipvs "$ns" add "$addr" ;;
    esac
}

label_fields() {  # label_fields <label>: row fields describing the configuration
    local plane mode hash ct
    read -r plane mode hash ct <<<"$(describe "$1")"
    local f=("lb=$plane" "config=$1")
    if [[ $mode == n/a ]]; then f+=("xdp_mode:=null"); else f+=("xdp_mode=$mode"); fi
    if [[ $hash == n/a ]]; then f+=("hash:=null"); else f+=("hash=$hash"); fi
    if [[ $ct == n/a ]]; then f+=("conntrack:=null"); else f+=("conntrack:=$ct"); fi
    printf '%s\n' "${f[@]}"
}
LF=()
set_lf() { mapfile -t LF < <(label_fields "$1"); }

# ===========================================================================
# Experiment 1: packet rate at saturation
# ===========================================================================
exp1() {
    local labels=(packetbalance-native packetbalance-generic ipvs-mh ipvs-rr ipvs-mh-tun none)
    local rep label plane mode hash ct body
    for ((rep = 1; rep <= REPEATS; rep++)); do
        for label in "${labels[@]}"; do
            wanted "$label" || continue
            set_lf "$label"
            all_lbs_off; route_via lb1
            if ! setup_label "$label" lb1 1048576; then
                append exp1_pps.jsonl experiment=exp1 "${LF[@]}" "repeat:=$rep" \
                    "notes=forwarding plane failed to start. $NOTES"
                continue
            fi
            body=""; [[ $label != none ]] && body=$(check_vip)
            read -r plane mode hash ct <<<"$(describe "$label")"
            measure --name exp1_pps --experiment exp1 --lb "$plane" --lbns lb1 \
                --xdp-mode "$mode" --hash "$hash" --conntrack "$ct" \
                --conntrack-size "$([[ $plane == packetbalance ]] && echo 1048576 || echo null)" \
                --flows 10000 --duration "$DURATION" --repeat "$rep" \
                --notes "config=$label; curl via VIP: ${body:-n/a}; $NOTES"
        done
    done
    [[ $EXP1_SWEEP == 1 ]] || return 0
    local rate
    for label in "${labels[@]}"; do
        wanted "$label" || continue
        all_lbs_off; route_via lb1
        setup_label "$label" lb1 1048576 || continue
        read -r plane mode hash ct <<<"$(describe "$label")"
        for rate in $EXP1_RATES; do
            measure --name exp1_sweep --experiment exp1_sweep --lb "$plane" --lbns lb1 \
                --xdp-mode "$mode" --hash "$hash" --conntrack "$ct" \
                --conntrack-size "$([[ $plane == packetbalance ]] && echo 1048576 || echo null)" \
                --flows 10000 --duration 10 --rate "$rate" --repeat 1 --notes "config=$label; $NOTES"
        done
    done
    all_lbs_off
}

# ===========================================================================
# Experiment 2: latency and throughput through the LB (wrk)
# ===========================================================================
exp2() {
    local labels=(packetbalance packetbalance-generic ipvs-mh ipvs-rr ipvs-mh-tun none)
    local rep label url out
    for ((rep = 1; rep <= REPEATS; rep++)); do
        for label in "${labels[@]}"; do
            wanted "$label" || continue
            set_lf "$label"
            all_lbs_off; route_via lb1
            setup_label "$label" lb1 1048576 || { append exp2_http.jsonl experiment=exp2 \
                "${LF[@]}" "repeat:=$rep" "notes=forwarding plane failed to start"; continue; }
            url="http://$VIP/"
            [[ $label == none ]] && url="http://$(real_ip 1)/"
            out="$RUN/wrk.out"
            # Host gate before and after each run (common.sh); a run whose
            # canary afterwards is below CANARY_MIN goes to rejected.jsonl and
            # is repeated, up to MEASURE_TRIES runs.
            local try
            for ((try = 1; try <= MEASURE_TRIES; try++)); do
                read -r gate_wait gate_canary < <(host_gate)
                log "exp2 $label rep $rep: wrk -t4 -c256 -d${DURATION}s --latency $url"
                nsx client wrk -t4 -c256 "-d${DURATION}s" --latency "$url" >"$out" 2>&1 || true
                gate_canary_after=$(cpu_canary)
                $LABUTIL wrk "$out" >"$RUN/wrk.json"
                ((gate_canary_after >= CANARY_MIN)) && break
                $LABUTIL row --merge "$RUN/wrk.json" experiment=exp2 "${LF[@]}" "url=$url" \
                    "cpu_canary:=[$gate_canary, $gate_canary_after]" "cpu_canary_min_required:=$CANARY_MIN" \
                    host_gate=fail rejected_from=exp2_http.jsonl "repeat:=$rep" "notes=$NOTES" >>"$RESULTS/rejected.jsonl"
                log "exp2 $label rep $rep: canary after the run $gate_canary_after < $CANARY_MIN, rejected (try $try of $MEASURE_TRIES)"
            done
            ((gate_canary_after >= CANARY_MIN)) || { log "exp2 $label rep $rep: every try failed the host gate, no row"; continue; }
            append exp2_http.jsonl --merge "$RUN/wrk.json" experiment=exp2 "${LF[@]}" \
                "url=$url" "cpu_canary:=[$gate_canary, $gate_canary_after]" "cpu_canary_min_required:=$CANARY_MIN" \
                host_gate=pass "quiet_wait_s:=$gate_wait" \
                "wrk_threads:=4" "wrk_connections:=256" "duration_s:=$DURATION" "repeat:=$rep" \
                "notes=$([[ $label == none ]] && echo 'direct to real1 only (1 nginx), not 4 reals; ')$NOTES"
        done
    done
    all_lbs_off
}

# ===========================================================================
# conncheck run with timed events. run_conncheck <out.json> <duration> <event_at_s> <fn> [<event_at_s> <fn>]...
# Each fn is eval'ed at its offset from conncheck's start.
# ===========================================================================
run_conncheck() {
    local out=$1 dur=$2; shift 2
    local bin t0 cc_pid
    bin=$(find_bin conncheck) || die "conncheck not built"
    rm -f "$out"
    ip netns exec client "$bin" --vip "$VIP:7000" --connections "$CONNS" --heartbeat-ms 100 \
        --duration-s "$dur" --json "$out" --connect-rate 5000 --ramp-ms 100 \
        </dev/null >"$RUN/conncheck.out" 2>&1 &
    cc_pid=$!
    bg_pids+=("$cc_pid")
    t0=$(date +%s.%N)
    while [[ $# -ge 2 ]]; do
        sleep_until "$(python3 -c "print($t0 + $1)")"
        log "t=+$1s: $2"
        eval "$2" || log "WARN: event '$2' failed"
        shift 2
    done
    wait "$cc_pid" || true
    tail -n 1 "$RUN/conncheck.out" >&2
}

# ===========================================================================
# Experiment 3: connection survival through backend churn
# ===========================================================================
exp3() {
    local labels=(packetbalance packetbalance-noct packetbalance-modulo ipvs-mh ipvs-rr)
    local rep label removed
    removed=$(real_ip 3)
    for ((rep = 1; rep <= REPEATS; rep++)); do
        for label in "${labels[@]}"; do
            wanted "$label" || continue
            set_lf "$label"
            all_lbs_off; route_via lb1
            setup_label "$label" lb1 1048576 || { append exp3_churn.jsonl experiment=exp3 \
                "${LF[@]}" "repeat:=$rep" "notes=forwarding plane failed to start"; continue; }
            run_conncheck "$RUN/cc.json" 30 \
                10 "real_del $label lb1 $removed" \
                20 "real_add $label lb1 $removed"
            append exp3_churn.jsonl --merge "$RUN/cc.json" experiment=exp3 "${LF[@]}" \
                "removed_real=$removed" "removed_backend_id:=3" \
                "events=real del $removed at t=10s, real add at t=20s, end t=30s" \
                "repeat:=$rep" "notes=$NOTES"
            sleep 2
        done
    done
    all_lbs_off
}

# ===========================================================================
# Experiment 4: connection survival through LB failover (lb1 -> lb2)
# ===========================================================================
exp4() {
    local rep label drift sloppy extra
    local cases=(
        "packetbalance 0 -" "packetbalance-modulo 0 -" "ipvs-rr 0 1" "ipvs-mh 0 1"
        "ipvs-rr 0 0" "ipvs-mh 0 0"
        "packetbalance 1 -" "packetbalance-modulo 1 -" "ipvs-mh 1 1"
    )
    for ((rep = 1; rep <= REPEATS; rep++)); do
        for c in "${cases[@]}"; do
            read -r label drift sloppy <<<"$c"
            wanted "$label" || continue
            set_lf "$label"
            all_lbs_off; route_via lb1
            extra=()
            [[ $drift == 1 ]] && extra=("$(real_ip 5)")
            [[ $sloppy != - ]] && export IPVS_SLOPPY_TCP=$sloppy
            if ! setup_label "$label" lb1 1048576 || ! setup_label "$label" lb2 1048576 "${extra[@]}"; then
                append exp4_failover.jsonl experiment=exp4 "${LF[@]}" "repeat:=$rep" \
                    "drift:=$([[ $drift == 1 ]] && echo true || echo false)" "notes=forwarding plane failed to start"
                unset IPVS_SLOPPY_TCP
                continue
            fi
            nsx client ping -c1 -W1 "$(ns_ip lb2)" >/dev/null || true   # warm the ARP entry for lb2
            run_conncheck "$RUN/cc.json" 30 10 "route_via lb2"
            route_via lb1
            local extra_fields=()
            [[ $sloppy != - ]] && extra_fields+=("ipvs_sloppy_tcp:=$sloppy")
            if [[ $drift == 1 ]]; then
                extra_fields+=("drift:=true" "lb2_extra_real=$(real_ip 5)" "reals_lb1:=4" "reals_lb2:=5"
                    "min_fraction_moved:=0.2")
            else
                extra_fields+=("drift:=false" "reals_lb1:=4" "reals_lb2:=4")
            fi
            append exp4_failover.jsonl --merge "$RUN/cc.json" experiment=exp4 "${LF[@]}" \
                "${extra_fields[@]}" "events=client route flipped lb1 -> lb2 at t=10s, end t=30s" \
                "repeat:=$rep" "notes=$NOTES"
            unset IPVS_SLOPPY_TCP
            sleep 2
        done
    done
    all_lbs_off
}

# ===========================================================================
# Experiment 6: what the connection table costs (Exp 1 across table sizes)
# ===========================================================================
exp6() {
    local rep c ct size body mem_note
    local cases=("false 1048576" "true 65536" "true 1048576" "true 8388608")
    wanted packetbalance || return 0
    for ((rep = 1; rep <= REPEATS; rep++)); do
        for c in "${cases[@]}"; do
            read -r ct size <<<"$c"
            # EXP6_DONE="rep:ct:size ..." skips windows already measured (resume)
            if [[ " ${EXP6_DONE:-} " == *" $rep:$ct:$size "* ]]; then log "exp6: skip $rep:$ct:$size (EXP6_DONE)"; continue; fi
            all_lbs_off; route_via lb1
            mem_note="MemAvailable before start: $(awk '/MemAvailable/ {print $2 " kB"}' /proc/meminfo)"
            if ! setup_pb lb1 "$PB_XDP_MODE" maglev "$ct" "$size"; then
                append exp6_conntrack.jsonl experiment=exp6 lb=packetbalance "xdp_mode=$PB_XDP_MODE" hash=maglev \
                    "conntrack:=$ct" "conntrack_size:=$size" "repeat:=$rep" "forwarded_pps:=null" \
                    "notes=daemon failed to start with this conntrack size (LRU_PERCPU_HASH, --conntrack-size $size total flows, $(nproc) CPUs); $mem_note; log: $(tail -n 3 "$RUN/pb-lb1.log" 2>/dev/null | tr '\n' ' ' | tr -d '\"')"
                continue
            fi
            body=$(check_vip)
            measure --name exp6_conntrack --experiment exp6 --lb packetbalance --lbns lb1 \
                --xdp-mode "$PB_XDP_MODE" --hash maglev --conntrack "$ct" --conntrack-size "$size" \
                --flows 10000 --duration "$DURATION" --repeat "$rep" \
                --notes "curl via VIP: $body; $mem_note; $NOTES"
        done
    done
    all_lbs_off
}

# ===========================================================================
# Daemon restart keeps flows: conncheck holds RESTART_CONNS connections through
# lb1; the daemon (started WITHOUT --detach-on-exit) gets SIGTERM at t=10 s and
# is started again at t=15 s. The XDP program and the pinned maps (connection
# table included) outlive the process, so nothing should break.
# ===========================================================================
RESTART_CONNS=${RESTART_CONNS:-1000}
pb_term() {  # pb_term <lbns>: SIGTERM the daemon and wait for it to exit; keep XDP and pins
    local ns=$1 pid i
    pid=$(cat "$RUN/pb-$ns.pid" 2>/dev/null || true)
    [[ -n $pid ]] || { log "pb_term: no pid for $ns"; return 1; }
    kill -TERM "$pid"
    for ((i = 0; i < 100; i++)); do kill -0 "$pid" 2>/dev/null || break; sleep 0.05; done
    kill -0 "$pid" 2>/dev/null && { log "pb_term: daemon in $ns did not exit"; return 1; }
    RESTART_XDP_WHILE_DOWN=$(nsx "$ns" ip -d link show dev veth0 | grep -Eo 'prog/xdp[a-z]* id [0-9]+' | head -n1)
    log "daemon in $ns exited; XDP while down: ${RESTART_XDP_WHILE_DOWN:-none}"
}
flow_count() { pbctl_ns "$1" --json flows --limit 1000000 2>/dev/null |
    python3 -c 'import json, sys; print(len({(f["src"], f["sport"], f["dst"], f["dport"], f["proto"]) for f in json.load(sys.stdin)}))' ||
    echo null; }
restart() {
    local rep before after mode=$PB_XDP_MODE
    wanted packetbalance || return 0
    for ((rep = 1; rep <= REPEATS; rep++)); do
        all_lbs_off; route_via lb1
        write_pb_config lb1 maglev true 1048576 >/dev/null
        if ! pb_start lb1 "$mode" --conntrack-size 1048576; then
            append restart.jsonl experiment=restart lb=packetbalance "xdp_mode=$mode" "repeat:=$rep" \
                "notes=daemon failed to start. $NOTES"
            continue
        fi
        RESTART_XDP_WHILE_DOWN=""
        CONNS=$RESTART_CONNS run_conncheck "$RUN/cc.json" 30 \
            9 'before=$(flow_count lb1)' \
            10 "pb_term lb1" \
            15 "pb_start lb1 $mode --conntrack-size 1048576" \
            16 'after=$(flow_count lb1)'
        append restart.jsonl --merge "$RUN/cc.json" experiment=restart lb=packetbalance config=packetbalance \
            "xdp_mode=$mode" hash=maglev "conntrack:=true" "conntrack_size:=1048576" \
            "flows_tracked_before:=${before:-null}" "flows_tracked_after_restart:=${after:-null}" \
            "xdp_while_daemon_down=${RESTART_XDP_WHILE_DOWN:-none}" \
            "events=daemon SIGTERM at t=10s (no --detach-on-exit), started again at t=15s, end t=30s" \
            "repeat:=$rep" "notes=$NOTES"
        pb_stop lb1
        sleep 2
    done
}

# ---------------------------------------------------------------------------
[[ $# -gt 0 ]] || set -- all
mkdir -p "$RESULTS"
for e in "$@"; do
    case $e in
        exp1) exp1 ;;
        exp2) exp2 ;;
        exp3) exp3 ;;
        exp4) exp4 ;;
        exp6) exp6 ;;
        restart) restart ;;
        all) exp1; exp2; exp3; exp4; exp6; restart ;;
        *) die "unknown experiment $e (exp1 exp2 exp3 exp4 exp6 restart all)" ;;
    esac
done
log "done. Tables: python3 lab/render_tables.py [--write]"
