// SPDX-License-Identifier: MIT
//
// Line-oriented logging to stderr, which systemd's journal captures. Every line
// starts with an ISO 8601 UTC timestamp with milliseconds, because the lab
// experiments (backend churn, Experiment 3) line health transitions up against
// client-side breakage by timestamp.
//
//   2026-09-23T22:31:02.123Z INFO  health: vip=198.51.100.1:80/tcp real=10.0.0.23 UP->DOWN ...
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace pb::log {

enum class Level { Debug = 0, Info, Warn, Error };

Level parse_level(const std::string& s);  // debug|info|warn|error, throws
void set_level(Level level);
bool enabled(Level level);
void write(Level level, std::string_view message);

// ISO 8601 UTC with milliseconds, for log lines and for API timestamps.
std::string now_iso8601();

template <typename... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace pb::log
