// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace driver_mapping {
// The caller must establish reservation/device ownership and non-overlap.
// Virtual mappings in an owned reservation consume its address capacity;
// standalone mappings retain their separate, fixed byte quota. Residency is
// accounted by the native memory guard, never by the sum of virtual mappings.
inline bool fits(std::uint32_t totalPages,std::uint32_t reservedPages,std::uint64_t requestedPages,
                 std::uint32_t standaloneLimitPages,std::uint64_t reservationLimitPages,bool covered) {
    if(reservedPages>totalPages || reservedPages>reservationLimitPages || !requestedPages ||
       requestedPages>UINT32_MAX-totalPages) return false;
    const auto standalone=totalPages-reservedPages;
    if(standalone>standaloneLimitPages) return false;
    return covered ? requestedPages<=reservationLimitPages-reservedPages : requestedPages<=standaloneLimitPages-standalone;
}
}
