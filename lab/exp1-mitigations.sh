#!/usr/bin/env bash
# Exp 1 packet accounting and lab mitigations for native XDP on veth, written
# to results/exp1_mitigations.jsonl with a `variant` field (see results/README.md).
#
#   sudo PB_BUILD_DIR=... lab/exp1-mitigations.sh standard|rps|pg2
#
#   standard  PacketBalance native, the published configuration, with the
#             packet-accounting fields
#   rps       rps_cpus=0x30 on every rx queue of pb-lb1 (skb work after the LB
#             veth's peer on CPUs 4-5): native, generic, IPVS mh
#   pg2       PKTGEN_THREADS=2: native, generic, IPVS mh
# Env: REPS (default 3). NOTES is appended to each row's notes.
set -uo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root
cd "$REPO" || exit 1
C=${NOTES:-}
REPS=${REPS:-3}
start_plane() {  # native|generic|ipvs-mh
  all_lbs_off; route_via lb1
  case $1 in
    native|generic) write_pb_config lb1 maglev true 1048576 >/dev/null; pb_start lb1 $1 --detach-on-exit --conntrack-size 1048576 >/dev/null 2>&1 ;;
    ipvs-mh) ipvs lb1 up mh dr >/dev/null 2>&1 ;;
  esac
}
m() {  # m <plane> <variant>
  local plane=$1 v=$2 lbl xm h
  case $plane in native|generic) lbl=packetbalance; xm=$plane; h=maglev ;; *) lbl=$plane; xm=n/a; h=mh ;; esac
  local ctsize=null
  [[ $lbl == packetbalance ]] && ctsize=1048576
  for ((r=1; r<=REPS; r++)); do
    MEASURE_VARIANT=$v "$LAB_DIR/measure.sh" --name exp1_mitigations --experiment exp1 --lb $lbl --lbns lb1 --xdp-mode $xm --hash $h \
      --conntrack true --conntrack-size "$ctsize" --flows 10000 --duration 30 --repeat $r \
      --notes "variant=$v; $C" 2>&1 | grep -E 'gate|FATAL' ; done
}
rps() {  # rps <hexmask|0>
  for q in /sys/class/net/pb-lb1/queues/rx-*/rps_cpus; do echo $1 > $q; done
}
case ${1:-} in
standard) start_plane native; PKTGEN_THREADS=4 m native standard ;;
rps) rps 30; for p in native generic ipvs-mh; do start_plane $p; PKTGEN_THREADS=4 m $p rps-pb-lb1-cpus4-5; done; rps 0 ;;
pg2) for p in native generic ipvs-mh; do start_plane $p; PKTGEN_THREADS=2 m $p pktgen-2-threads; done ;;
esac
all_lbs_off
