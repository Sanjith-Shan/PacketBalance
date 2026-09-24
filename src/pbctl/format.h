// SPDX-License-Identifier: MIT
//
// Human-readable rendering of API results. `--json` bypasses all of this.
#pragma once

#include <iosfwd>
#include <string>

#include <nlohmann/json.hpp>

namespace pbctl {

// `command` is the API command name ("vip.list", "stats", ...).
void print_result(std::ostream& out, const std::string& command, const nlohmann::json& result);

}  // namespace pbctl
