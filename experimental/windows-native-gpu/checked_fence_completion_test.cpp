// SPDX-License-Identifier: MIT
#include "checked_fence_completion.h"
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using native_gpu::checkedFenceCompletion;
using native_gpu::FenceCompletion;

static void require(bool value) {
    if (!value) throw std::runtime_error("Fence completion fault control failed");
}
static void checkCase(std::initializer_list<std::uint64_t> values, bool arm, bool wake,
                      FenceCompletion expected, unsigned expectedReads, unsigned expectedArms,
                      unsigned expectedWaits, std::uint64_t target = 7) {
    const std::vector<std::uint64_t> observed(values);
    unsigned reads = 0, arms = 0, waits = 0;
    const auto result = checkedFenceCompletion(target,
        [&] { require(reads < observed.size()); return observed[reads++]; },
        [&] { ++arms; return arm; },
        [&] { ++waits; return wake; });
    require(result == expected && reads == expectedReads && arms == expectedArms && waits == expectedWaits);
}
int main() {
    checkCase({7}, false, false, FenceCompletion::Complete, 1, 0, 0);
    checkCase({11}, false, false, FenceCompletion::Complete, 1, 0, 0);
    checkCase({0}, false, false, FenceCompletion::Complete, 1, 0, 0, 0);
    checkCase({UINT64_MAX}, true, true, FenceCompletion::DeviceRemoved, 1, 0, 0);
    checkCase({UINT64_MAX}, true, true, FenceCompletion::DeviceRemoved, 1, 0, 0, 0);
    checkCase({0}, false, true, FenceCompletion::ArmFailed, 1, 1, 0);
    checkCase({0}, true, false, FenceCompletion::WaitFailed, 1, 1, 1);
    checkCase({0, 7}, true, true, FenceCompletion::Complete, 2, 1, 1);
    checkCase({0, 11}, true, true, FenceCompletion::Complete, 2, 1, 1);
    checkCase({0, 6}, true, true, FenceCompletion::Incomplete, 2, 1, 1);
    checkCase({0, 0}, true, true, FenceCompletion::Incomplete, 2, 1, 1);
    checkCase({0, UINT64_MAX}, true, true, FenceCompletion::DeviceRemoved, 2, 1, 1);
    // This pair reproduces the old false-success condition: event signaled,
    // but the second observation is either unretired or device removed.
    bool threw = false;
    bool waitCalled = false;
    volatile bool injectArmFailure = true;
    try {
        checkedFenceCompletion(7, []() -> std::uint64_t { return 0; },
            [&] {
                if (injectArmFailure) throw std::runtime_error("injected arm failure");
                return true;
            },
            [&] { waitCalled = true; return true; });
    } catch (const std::runtime_error& error) {
        threw = std::string(error.what()) == "injected arm failure";
    }
    require(threw && !waitCalled);
    std::cout << "CHECKED_FENCE_COMPLETION_CONTROL cases=13 postWakeTargetRequired=true removalRejected=true gpu=false\n";
}
