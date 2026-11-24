// SPDX-License-Identifier: MIT
#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
namespace driver_bridge {
// The one native stream still has one owner at a time. FIFO acquisition keeps
// a busy caller from repeatedly overtaking another client's cleanup RPCs.
class FairRpcMutex {
    std::mutex state;
    std::condition_variable ready;
    std::uint64_t next,serving;
public:
    explicit FairRpcMutex(std::uint64_t first=0):next(first),serving(first) {}
    FairRpcMutex(const FairRpcMutex&)=delete;
    FairRpcMutex& operator=(const FairRpcMutex&)=delete;
    void lock() {
        std::unique_lock<std::mutex> guard(state);
        const auto ticket=next++;
        ready.wait(guard,[&]{return ticket==serving;});
    }
    void unlock() {
        { std::lock_guard<std::mutex> guard(state);++serving; }
        ready.notify_all();
    }
    std::uint64_t pending() {
        std::lock_guard<std::mutex> guard(state);return next-serving;
    }
};
}
