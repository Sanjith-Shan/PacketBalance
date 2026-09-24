// SPDX-License-Identifier: MIT
//
// A deliberately tiny HTTP/1.0 server: GET /metrics -> 200 with the
// exposition, anything else -> 404. One thread, one request per connection,
// served inline. Prometheus scrapes every few seconds; nothing here needs to
// be concurrent, and a hand-written server keeps the daemon dependency-free.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "unique_fd.h"

namespace pb {

class MetricsServer {
public:
    using Render = std::function<std::string()>;

    MetricsServer(std::string listen, Render render);  // listen = "ADDR:PORT"
    ~MetricsServer();

    void start();  // throws std::runtime_error
    void stop();

private:
    void loop();
    void serve(int fd);

    std::string listen_;
    Render render_;
    UniqueFd fd_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace pb
