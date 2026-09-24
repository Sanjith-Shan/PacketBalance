// SPDX-License-Identifier: MIT
//
// Prometheus text exposition (format 0.0.4) of the per-CPU counters, summed,
// plus control-plane state (real health, weights, ring generations, XDP mode).
// Metric names are fixed by docs/API.md.
//
// pb_conntrack_entries walks the connection table's keys with get_next_key, so
// a scrape costs O(entries) syscalls: about a millisecond per few thousand
// flows. At the default 1M-entry table a full table makes a scrape take on the
// order of a second, which is acceptable at a 15 s scrape interval and keeps
// the data plane free of a shared counter. It is the one expensive metric.
#pragma once

#include <string>

#include "lb_state.h"
#include "map_reader.h"
#include "packetbalance/config.h"

namespace pb {

std::string render_metrics(const LbState& state, const MapReader& reader, XdpMode attached_mode);

}  // namespace pb
