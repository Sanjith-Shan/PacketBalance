// SPDX-License-Identifier: MIT
#include "log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <stdexcept>

namespace pb::log {

namespace {
std::atomic<Level> g_level{Level::Info};
std::mutex g_write_mutex;  // keeps lines from different threads whole

const char* level_name(Level l) {
    switch (l) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?    ";
}
}  // namespace

Level parse_level(const std::string& s) {
    if (s == "debug") return Level::Debug;
    if (s == "info") return Level::Info;
    if (s == "warn") return Level::Warn;
    if (s == "error") return Level::Error;
    throw std::invalid_argument("bad log level (debug|info|warn|error): " + s);
}

void set_level(Level level) { g_level.store(level); }
bool enabled(Level level) { return level >= g_level.load(std::memory_order_relaxed); }

std::string now_iso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return std::format("{}.{:03}Z", buf, ms);
}

void write(Level level, std::string_view message) {
    const std::string line = std::format("{} {} {}\n", now_iso8601(), level_name(level), message);
    std::lock_guard lock(g_write_mutex);
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
}

}  // namespace pb::log
