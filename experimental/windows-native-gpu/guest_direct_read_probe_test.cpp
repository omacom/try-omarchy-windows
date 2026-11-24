// SPDX-License-Identifier: MIT
// Volatile CPU-memory controls, not a GPU/VM performance benchmark.
#include "guest_direct_read_probe.h"
#include <cassert>
#include <cstdio>
#include <vector>
int main() {
    using namespace guest_direct_read;
    static_assert(ProbeLoads == (WDDM_BRIDGE_SAMPLED_DIRECT_READS ? 2u : 10000u));
    volatile std::uint64_t fence = 7;
    unsigned reads = 0;
    const auto observed = fenceProbe([&]() { ++reads;return fence; });
    assert(reads == ProbeLoads && observed == 7 && fence == 7);
    assert(retired(observed,7) && retired(observed,6) && !retired(observed,8));
    assert(!retired(UINT64_MAX,0) && !retired(UINT64_MAX,UINT64_MAX));
    assert(retired(UINT64_MAX-1,UINT64_MAX-1) && !retired(UINT64_MAX-1,UINT64_MAX));
    assert(retired(0,0) && !retired(0,1));

    reads = 0;fence = 1;
    const auto advanced = fenceProbe([&]() {
        if (++reads == ProbeLoads) fence = 9;
        return fence;
    });
    assert(advanced == 9 && reads == ProbeLoads && retired(advanced,9));
    reads = 0;
    const auto lost = fenceProbe([&]() {
        if (++reads == ProbeLoads) fence = UINT64_MAX;
        return fence;
    });
    assert(lost == UINT64_MAX && !retired(lost,9));

    volatile const std::uint64_t ends[2] = {11,29};
    reads = 0;unsigned first_reads = 0,last_reads = 0;
    std::vector<unsigned> order;
    allocationEndsProbe([&](unsigned end) {
        assert(end < 2);++reads;order.push_back(end);
        if (end) ++last_reads;else ++first_reads;
        return ends[end];
    });
    assert(reads == ProbeLoads && first_reads == ProbeLoads/2 && last_reads == ProbeLoads/2);
    assert(ends[0] == 11 && ends[1] == 29);
    for (unsigned n = 0;n < ProbeLoads;++n) assert(order[n] == (n & 1u));
    std::printf("DIRECT_READ_PROBE loads=%u actualVolatileReads=true bothAllocationEnds=true latestFenceRequired=true deviceLossRejected=true gpu=false\n",ProbeLoads);
}
