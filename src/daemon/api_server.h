// SPDX-License-Identifier: MIT
//
// The control socket: a Unix stream socket speaking newline-delimited JSON
// (docs/API.md). One thread accepts; each connection gets its own thread and
// is served one request line at a time. Connections are few (pbctl, the lab
// harness) and short, so a pool would be ceremony.
#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>

#include "unique_fd.h"

namespace pb {

class ApiServer {
public:
    // Maps one request line (without '\n') to one response line.
    using Handler = std::function<std::string(const std::string&)>;

    ApiServer(std::string path, Handler handler);
    ~ApiServer();

    // Binds (replacing a stale socket file, refusing a live one) and starts
    // accepting. Throws std::runtime_error.
    void start();
    void stop();

private:
    void accept_loop();
    void serve(int fd);

    std::string path_;
    Handler handler_;
    UniqueFd listen_fd_;
    std::atomic<bool> stopping_{false};
    std::thread acceptor_;
    std::mutex clients_mu_;
    struct Client {
        int fd;
        std::thread thread;
        bool done = false;
    };
    std::list<Client> clients_;
};

}  // namespace pb
