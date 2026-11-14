// SPDX-License-Identifier: MIT
// Explicit candidate profile; default callers retain the historical ABI policy.
#pragma once
#include "guest-kernel/wddm_aperture.h"
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <system_error>
#include <vector>
#ifndef WDDM_BRIDGE_ASYNC_CPU_WAITS
#define WDDM_BRIDGE_ASYNC_CPU_WAITS 0
#endif
#if WDDM_BRIDGE_ASYNC_CPU_WAITS != 0 && WDDM_BRIDGE_ASYNC_CPU_WAITS != 1
#error WDDM_BRIDGE_ASYNC_CPU_WAITS must be 0 or 1
#endif
namespace guest_async {
constexpr bool Enabled = WDDM_BRIDGE_ASYNC_CPU_WAITS == 1;
inline void submit(const std::vector<wddm_fence_eventfd_point>& points,
                   std::uintptr_t event, std::uint32_t flags) {
    if (!Enabled) throw std::system_error(EOPNOTSUPP, std::generic_category());
    if (points.empty() || points.size() > WDDM_FENCE_EVENTFD_MAX_POINTS ||
        event > INT_MAX || (flags & ~WDDM_FENCE_EVENTFD_WAIT_ANY))
        throw std::system_error(EINVAL, std::generic_category());
    wddm_fence_eventfd_wait wait{};
    wait.version = WDDM_APERTURE_VERSION; wait.flags = flags;
    wait.count = static_cast<std::uint32_t>(points.size());
    wait.event_fd = static_cast<int>(event);
    wait.points = reinterpret_cast<std::uintptr_t>(points.data());
    if (syscall(SYS_ioctl, points.front().lease_fd, WDDM_FENCE_EVENTFD_WAIT, &wait))
        throw std::system_error(errno, std::generic_category(), "Register GPU fence eventfd");
}
}
static_assert(sizeof(wddm_fence_eventfd_point) == 24 && sizeof(wddm_fence_eventfd_wait) == 40 &&
              sizeof(wddm_fence_eventfd_stats) == 48, "Fixed async CPU notification ABI");
