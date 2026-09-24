// SPDX-License-Identifier: MIT
//
// A worker thread that runs a step, sleeps, and can be woken early or stopped
// promptly. Shared by the health checker and the neighbour resolver.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace pb {

class PeriodicThread {
public:
    // `step` returns how long to sleep before the next run.
    using Step = std::function<std::chrono::milliseconds()>;

    PeriodicThread() = default;
    ~PeriodicThread() { stop(); }
    PeriodicThread(const PeriodicThread&) = delete;
    PeriodicThread& operator=(const PeriodicThread&) = delete;

    void start(Step step) {
        thread_ = std::thread([this, step = std::move(step)] {
            std::unique_lock lock(mu_);
            while (!stopping_) {
                lock.unlock();
                const auto pause = step();
                lock.lock();
                cv_.wait_for(lock, pause, [this] { return stopping_ || woken_; });
                woken_ = false;
            }
        });
    }

    void wake() {
        std::lock_guard lock(mu_);
        woken_ = true;
        cv_.notify_all();
    }

    void stop() {
        {
            std::lock_guard lock(mu_);
            stopping_ = true;
            cv_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool woken_ = false;
    std::thread thread_;
};

}  // namespace pb
