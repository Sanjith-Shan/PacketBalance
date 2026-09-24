#!/usr/bin/env bash
# Point the client's VIP route at a load balancer: lab/route-via.sh lb1|lb2
set -euo pipefail
# shellcheck source=lab/common.sh
source "$(dirname "$0")/common.sh"
need_root
[[ ${1:-} == lb1 || ${1:-} == lb2 ]] || die "usage: $0 lb1|lb2"
route_via "$1"
