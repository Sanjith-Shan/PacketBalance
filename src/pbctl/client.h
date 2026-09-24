// SPDX-License-Identifier: MIT
//
// One request, one response over the daemon's unix socket.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace pbctl {

// Sends `request` as one JSON line and returns the parsed response object.
// Throws std::runtime_error on connection or protocol failure.
nlohmann::json call(const std::string& socket_path, const nlohmann::json& request);

}  // namespace pbctl
