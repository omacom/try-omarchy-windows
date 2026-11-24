// SPDX-License-Identifier: MIT
// Real D3D12 queue signals test the candidate Linux eventfd wait bridge.
// CPU writes below affect notification counters only, never GPU fence words.
#pragma once
#include "guest-kernel/wddm_aperture.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace async_wait_test {
using Microsoft::WRL::ComPtr;
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline void success(const char* stage, HRESULT hr) {
    std::printf("ASYNC_CPU_WAIT_HRESULT stage=%s value=%08x\n", stage, static_cast<unsigned>(hr));
    require(SUCCEEDED(hr), stage);
}
struct Event {
    int fd = static_cast<int>(syscall(SYS_eventfd2, 0, EFD_CLOEXEC | EFD_NONBLOCK));
    Event() { require(fd >= 0, "eventfd"); }
    Event(const Event&) = delete;
    ~Event() { close(); }
    void close() { if (fd >= 0) { syscall(SYS_close, fd); fd = -1; } }
};
inline HANDLE handle(int fd) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd)); }
inline void unsignaled(int fd, int milliseconds = 0) {
    pollfd item{fd, POLLIN, 0};
    require(poll(&item, 1, milliseconds) == 0, "event notified before its GPU dependency");
}
inline void consume(int fd, std::uint64_t expected = 1, bool overflow = false) {
    pollfd item{fd, POLLIN, 0};
    require(poll(&item, 1, 5000) == 1 && (item.revents & POLLIN) &&
        !(item.revents & (POLLHUP | POLLNVAL)) && (overflow || !(item.revents & POLLERR)), "eventfd wait");
    std::uint64_t value = 0;
    require(syscall(SYS_read, fd, &value, sizeof value) == sizeof value && value == expected, "eventfd counter");
}
inline ComPtr<ID3D12Fence> fence(ID3D12Device* device) {
    ComPtr<ID3D12Fence> result;
    success("create-fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&result)));
    require(result->GetCompletedValue() == 0, "new GPU fence did not start at zero");
    return result;
}
inline void gpuValue(ID3D12Fence* source, std::uint64_t target) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        const auto value = source->GetCompletedValue();
        require(value != UINT64_MAX, "GPU device lost");
        if (value >= target) return;
        require(std::chrono::steady_clock::now() < deadline, "GPU signal timeout");
        usleep(100);
    }
}
inline wddm_fence_eventfd_stats stats() {
    using Function = int (*)(wddm_fence_eventfd_stats*);
    const auto query = reinterpret_cast<Function>(dlsym(RTLD_DEFAULT, "wddm_bridge_async_cpu_wait_info"));
    require(query != nullptr, "async wait statistics hook");
    wddm_fence_eventfd_stats value{};
    require(query(&value) == 0 && value.version == 1 && !value.reserved, "async wait statistics");
    return value;
}
inline wddm_fence_eventfd_stats settled() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (;;) {
        const auto value = stats();
        if (!value.held && !value.pending && value.created == value.released) {
            require(!value.errors && value.notified == value.created, "async notification errors");
            return value;
        }
        require(std::chrono::steady_clock::now() < deadline, "async notification release timeout");
        usleep(1000);
    }
}
inline void registered(const wddm_fence_eventfd_stats& before, unsigned count = 1) {
    const auto value = stats();
    require(value.created == before.created + count && value.pending == count &&
        value.held == count && value.notified == before.notified && !value.errors,
        "wait registration was not pending and nonblocking");
}
inline int run(ID3D12Device* device, ID3D12CommandQueue* queue) {
    try {
        ComPtr<ID3D12Device1> extended;
        success("device-interface", device->QueryInterface(IID_PPV_ARGS(&extended)));
        const auto initial = settled();
#ifdef WDDM_BRIDGE_TEST_ASYNC_EXIT
        auto source = fence(device);
        Event event;
        success("exit-control-register", source->SetEventOnCompletion(999, handle(event.fd)));
        registered(initial);
        unsignaled(event.fd);
        std::printf("ASYNC_CPU_WAIT_EXIT_CONTROL pending=1 notified=0 futureTarget=999 cpuFenceStores=false\n");
        std::fflush(stdout);
        _exit(0);
#endif
        {
            const auto before = settled();
            auto source = fence(device);
            Event event;
            success("pending-register", source->SetEventOnCompletion(1, handle(event.fd)));
            registered(before); unsignaled(event.fd, 20);
            success("pending-gpu-signal", queue->Signal(source.Get(), 1));
            consume(event.fd); gpuValue(source.Get(), 1); settled();
            std::puts("ASYNC_CPU_WAIT_CASE name=pending passed=true gpuSignal=true");
        }
        {
            const auto before = settled();
            auto first = fence(device), second = fence(device);
            ID3D12Fence* sources[]{first.Get(), second.Get()};
            const UINT64 targets[]{3, 7}; Event event;
            success("all-register", extended->SetEventOnMultipleFenceCompletion(sources, targets, 2,
                D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, handle(event.fd)));
            registered(before); unsignaled(event.fd);
            success("all-first-gpu-signal", queue->Signal(first.Get(), targets[0]));
            gpuValue(first.Get(), targets[0]); unsignaled(event.fd, 20);
            success("all-second-gpu-signal", queue->Signal(second.Get(), targets[1]));
            consume(event.fd); gpuValue(second.Get(), targets[1]); settled();
            std::puts("ASYNC_CPU_WAIT_CASE name=all passed=true firstAloneDidNotNotify=true");
        }
        {
            const auto before = settled();
            auto first = fence(device), second = fence(device);
            ID3D12Fence* sources[]{first.Get(), second.Get()};
            const UINT64 targets[]{5, 9}; Event event;
            success("any-register", extended->SetEventOnMultipleFenceCompletion(sources, targets, 2,
                D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, handle(event.fd)));
            registered(before); unsignaled(event.fd);
            success("any-second-gpu-signal", queue->Signal(second.Get(), targets[1]));
            consume(event.fd); gpuValue(second.Get(), targets[1]);
            require(first->GetCompletedValue() == 0, "ANY advanced an unsignaled GPU fence");
            settled();
            std::puts("ASYNC_CPU_WAIT_CASE name=any passed=true otherFenceRemainedZero=true");
        }
        {
            const auto before = settled();
            auto source = fence(device);
            Event original;
            const int number = original.fd;
            const int observer = fcntl(number, F_DUPFD_CLOEXEC, 3);
            require(observer >= 0, "eventfd observer duplicate");
            success("reuse-register", source->SetEventOnCompletion(11, handle(number)));
            registered(before); original.close();
            Event replacement;
            require(replacement.fd == number, "numeric eventfd reuse was not achieved");
            success("reuse-gpu-signal", queue->Signal(source.Get(), 11));
            consume(observer); syscall(SYS_close, observer);
            unsignaled(replacement.fd, 20); gpuValue(source.Get(), 11); settled();
            std::puts("ASYNC_CPU_WAIT_CASE name=fd-reuse passed=true replacementNotSignaled=true");
        }
        {
            const auto before = settled();
            auto source = fence(device);
            std::vector<std::unique_ptr<Event>> events;
            for (unsigned index = 0; index < WDDM_FENCE_EVENTFD_MAX_WAITS; ++index) {
                events.emplace_back(new Event);
                success("quota-register", source->SetEventOnCompletion(29, handle(events.back()->fd)));
            }
            registered(before, WDDM_FENCE_EVENTFD_MAX_WAITS);
            Event excess;
            const auto refused = source->SetEventOnCompletion(29, handle(excess.fd));
            require(FAILED(refused), "wait quota did not reject excess registration");
            const auto limited = stats();
            require(limited.created == before.created + WDDM_FENCE_EVENTFD_MAX_WAITS &&
                limited.pending == WDDM_FENCE_EVENTFD_MAX_WAITS, "quota changed pending registrations");
            success("quota-gpu-signal", queue->Signal(source.Get(), 29));
            for (const auto& event : events) consume(event->fd);
            unsignaled(excess.fd, 20); gpuValue(source.Get(), 29); settled();
            std::puts("ASYNC_CPU_WAIT_CASE name=quota passed=true accepted=64 refused=1 allReleased=true");
        }
        {
            const auto before = settled();
            auto source = fence(device);
            Event event;
            const std::uint64_t full = UINT64_MAX - 1;
            require(syscall(SYS_write, event.fd, &full, sizeof full) == sizeof full, "fill eventfd counter");
            success("full-counter-register", source->SetEventOnCompletion(43, handle(event.fd)));
            registered(before);
            success("full-counter-gpu-signal", queue->Signal(source.Get(), 43));
            gpuValue(source.Get(), 43); settled();
            consume(event.fd, UINT64_MAX, true);
            std::puts("ASYNC_CPU_WAIT_CASE name=full-counter passed=true kernelDidNotBlock=true");
        }
        const auto final = settled();
        std::printf("ASYNC_CPU_WAIT_TEST_COMPLETE verified=true cases=6 created=%llu notified=%llu released=%llu errors=0 held=0 pending=0 cpuFenceStores=false shaderExecution=false\n",
            static_cast<unsigned long long>(final.created - initial.created),
            static_cast<unsigned long long>(final.notified - initial.notified),
            static_cast<unsigned long long>(final.released - initial.released));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "ASYNC_CPU_WAIT_TEST_FAIL reason=%s\n", error.what());
        return 1;
    }
}
}
