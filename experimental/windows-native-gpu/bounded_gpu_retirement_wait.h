// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace driver_bridge {
// Startup/state-map retry only. Read actual retirement; never write a fence.
// The clock and attempt bounds also terminate a stalled/future dependency.
template<class Ready,class Clock,class Yield>
bool boundedGpuRetirementWait(Ready ready,Clock now,Yield yield) {
    const std::uint64_t start=now();
    for(unsigned attempt=0;attempt<256;++attempt) {
        if(ready()) return true;
        if(now()-start>=250) return false;
        yield();
    }
    return ready();
}
}
