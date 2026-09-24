// SPDX-License-Identifier: MIT
//
// The JSON commands of docs/API.md: parse arguments, call LbState or
// MapReader, shape the result. Transport-free, so it is the same code whether
// the request came from pbctl, socat or the lab harness.
#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "lb_state.h"
#include "map_reader.h"

namespace pb {

class CommandHandler {
public:
    struct Hooks {
        std::function<void()> reload;                 // re-read YAML and apply; throws
        std::function<nlohmann::json()> config;       // effective daemon config
        std::function<void()> reals_changed;          // e.g. wake the ARP resolver
    };

    CommandHandler(LbState& state, const MapReader& reader, Hooks hooks);

    // One request line in, one response line out. Never throws.
    std::string handle_line(const std::string& line);

private:
    nlohmann::json dispatch(const nlohmann::json& req);

    nlohmann::json vip_list() const;
    nlohmann::json stats(const nlohmann::json& req) const;
    nlohmann::json flows(const nlohmann::json& req) const;
    nlohmann::json ring_show(const nlohmann::json& req) const;
    nlohmann::json health() const;

    LbState& state_;
    const MapReader& reader_;
    Hooks hooks_;
};

}  // namespace pb
