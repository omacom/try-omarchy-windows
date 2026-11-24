// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace native_gpu {
enum class FenceCompletion { Complete, DeviceRemoved, ArmFailed, WaitFailed, Incomplete };

// A signaled event is only a wake-up. Confirm the actual target after the wait,
// including the UINT64_MAX device-removal sentinel. No fence value is written.
// The callbacks keep failure controls CPU-only and use this same production path.
template<class Read, class Arm, class Wait>
FenceCompletion checkedFenceCompletion(std::uint64_t target, Read read, Arm arm, Wait wait) {
    auto observed = read();
    if (observed == UINT64_MAX) return FenceCompletion::DeviceRemoved;
    if (observed >= target) return FenceCompletion::Complete;
    if (!arm()) return FenceCompletion::ArmFailed;
    if (!wait()) return FenceCompletion::WaitFailed;
    observed = read();
    if (observed == UINT64_MAX) return FenceCompletion::DeviceRemoved;
    return observed >= target ? FenceCompletion::Complete : FenceCompletion::Incomplete;
}
} // namespace native_gpu
