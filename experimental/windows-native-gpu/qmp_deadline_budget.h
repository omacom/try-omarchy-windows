// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <stdexcept>

namespace driver_qmp {
constexpr std::uint32_t ControlCommandTimeoutMs = 5000;
constexpr std::uint32_t SdlRetirementFenceTimeoutMs = 5000;
constexpr std::uint32_t SdlCommandReplyAllowanceMs = 2000;
enum class ChannelPurpose { Control, SdlGraphics };

// QEMU's retirement fence may consume its entire five-second allowance before
// object deletion, boot-console restoration and the correlated reply. Keep a
// separate bounded budget on the dedicated graphics connection. Input and
// ordinary control retain their existing deadlines. No guest/CLI override.
inline std::uint32_t commandTimeoutMs(ChannelPurpose purpose) {
    switch (purpose) {
    case ChannelPurpose::Control: return ControlCommandTimeoutMs;
    case ChannelPurpose::SdlGraphics:
        return SdlRetirementFenceTimeoutMs + SdlCommandReplyAllowanceMs;
    }
    throw std::runtime_error("Invalid QMP channel purpose");
}
}
