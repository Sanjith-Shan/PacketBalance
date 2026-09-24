// SPDX-License-Identifier: MIT
//
// Command-line flags. Flags override the YAML file, and they keep overriding
// it on `reload`, so `--hash modulo` survives a reload of a file that says
// maglev.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "log.h"
#include "packetbalance/config.h"

namespace pb {

struct Options {
    std::string config_path = "/etc/packetbalance/packetbalance.yaml";
    std::optional<std::string> interface;
    std::optional<XdpMode> xdp_mode;
    std::optional<HashMode> hash;
    bool no_conntrack = false;
    std::optional<uint32_t> conntrack_size;
    std::optional<std::string> socket;
    std::optional<std::string> pin_path;
    std::optional<std::string> metrics_listen;
    bool no_health_check = false;
    bool detach_on_exit = false;
    bool recreate_maps = false;
    log::Level log_level = log::Level::Info;

    void apply_to(Config& cfg) const;
};

// Parses argv. Prints usage and exits 0 on --help, prints an error and exits
// 2 on a bad flag.
Options parse_options(int argc, char** argv);

}  // namespace pb
