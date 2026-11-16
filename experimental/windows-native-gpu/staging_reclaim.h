// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
namespace driver_shared {
constexpr std::uint64_t ResidencyStagingReserve=64ull*1024*1024;
inline std::uint64_t stagingPressureTarget(std::uint64_t usage,std::uint64_t limit) {
    if(usage>limit || limit<ResidencyStagingReserve) return 0;
    const auto headroom=limit-usage;
    return headroom<ResidencyStagingReserve?ResidencyStagingReserve-headroom:0;
}
struct StagingReclaimState {
    bool present=false,occupied=false;
    std::uint64_t bytes=0,reusedAfter=0;
};
struct StagingReclaimResult { std::uint64_t bytes=0;unsigned textures=0;bool failed=false; };
// The caller holds its slot mutex throughout selection and the real release.
// Slot zero supplies the audit footprint and is never a pressure candidate.
template<class Release>
StagingReclaimResult reclaimIdleStaging(const std::array<StagingReclaimState,3>& states,
    std::uint64_t inputCompleted,std::uint64_t outputCompleted,std::uint64_t target,Release release) {
    StagingReclaimResult result{};
    if(!target) return result;
    if(inputCompleted==UINT64_MAX || outputCompleted==UINT64_MAX) { result.failed=true;return result; }
    for(unsigned index=2;index>0 && result.bytes<target;--index) {
        const auto& state=states[index];
        if(!state.present || state.occupied || !state.reusedAfter ||
           state.reusedAfter>inputCompleted || state.reusedAfter>outputCompleted) continue;
        if(!state.bytes || state.bytes>UINT64_MAX-result.bytes || !release(index,state.bytes)) {
            result.failed=true;return result;
        }
        result.bytes+=state.bytes;++result.textures;
    }
    return result;
}
}
