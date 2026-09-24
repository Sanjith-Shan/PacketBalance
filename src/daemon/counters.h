// SPDX-License-Identifier: MIT
//
// Names of the data plane's per-VIP counters (enum pb_counter in abi.h),
// lower-cased without the PB_CNT_ prefix. These strings are API: `pbctl stats`
// keys and the lab scripts use them.
#pragma once

#include <array>
#include <cstdint>

#include "packetbalance/abi.h"

namespace pb {

inline constexpr std::array<const char*, PB_CNT_MAX> kCounterNames = {
    "packets",      "bytes",       "ct_hit",        "ct_miss",
    "hash",         "syn",         "tx",            "pass",
    "drop_frag",    "drop_opts",   "drop_no_real",  "drop_adj_head",
    "drop_mtu",     "drop_short",  "drop_other",    "icmp_pmtu_fwd",
};
static_assert(PB_CNT_MAX == 16, "update kCounterNames when enum pb_counter changes");

using Counters = std::array<uint64_t, PB_CNT_MAX>;

}  // namespace pb
