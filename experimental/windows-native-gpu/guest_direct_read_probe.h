// SPDX-License-Identifier: MIT
// Diagnostic sampling only; never supplies a fence value or bypasses MMIO checks.
#pragma once
#include <cstdint>
#include <limits>
#ifndef WDDM_BRIDGE_SAMPLED_DIRECT_READS
#define WDDM_BRIDGE_SAMPLED_DIRECT_READS 0
#endif
#if WDDM_BRIDGE_SAMPLED_DIRECT_READS != 0 && WDDM_BRIDGE_SAMPLED_DIRECT_READS != 1
#error Direct-read sampling selection must be zero or one
#endif
namespace guest_direct_read {
constexpr unsigned ProbeLoads = WDDM_BRIDGE_SAMPLED_DIRECT_READS ? 2u : 10000u;

template<class Read>
std::uint64_t fenceProbe(Read read) {
    std::uint64_t observed = 0;
    for (unsigned n = 0; n < ProbeLoads; ++n) observed = read();
    return observed;
}

template<class Read>
void allocationEndsProbe(Read read) {
    for (unsigned n = 0; n < ProbeLoads; ++n) {
        const auto observed = read(n & 1u);
        (void)observed;
    }
}

constexpr bool retired(std::uint64_t observed, std::uint64_t target) {
    return observed != std::numeric_limits<std::uint64_t>::max() && observed >= target;
}
} // namespace guest_direct_read
