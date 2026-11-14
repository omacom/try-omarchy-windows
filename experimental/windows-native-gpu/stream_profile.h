// SPDX-License-Identifier: MIT
// Explicit, fixed lab contract; no implicit expansion of older session quotas.
#pragma once
#include <cstdint>
namespace driver_bridge {
struct StreamProfile {
    std::uint32_t version, frames, pendingCommands, pendingSignals;
    std::uint32_t totalCommands, totalSignals, requestLimit, reserved;
};
static_assert(sizeof(StreamProfile) == 32, "fixed stream negotiation");
constexpr StreamProfile GuestStreamProfile{1, 64, 16, 16, 1024, 256, 50000, 0};
inline bool validStreamProfile(StreamProfile p) {
    return p.version == 1 && p.frames == 64 && p.pendingCommands == 16 && p.pendingSignals == 16 &&
           p.totalCommands == 1024 && p.totalSignals == 256 && p.requestLimit == 50000 && !p.reserved;
}
inline unsigned streamAuditRound(std::uint64_t frame) { return frame == 1 ? 1 : frame == 64 ? 2 : 0; }
}
