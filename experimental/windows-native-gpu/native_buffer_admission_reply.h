// SPDX-License-Identifier: MIT
#pragma once
#include "driver_wire.h"
#include <stdexcept>

namespace driver_bridge {
struct NativeBufferAdmissionReply { bool supported; std::int32_t transportStatus, nativeStatus; };
inline NativeBufferAdmissionReply parseNativeBufferAdmissionReply(const std::vector<std::uint8_t>& packet) {
    if (packet.size() < sizeof(Header)) throw std::runtime_error("Truncated buffer admission header");
    Header h{}; std::memcpy(&h, packet.data(), sizeof h);
    if (h.type != static_cast<std::uint32_t>(Op::QueryNativeGpuBufferAdmission) || h.handle || h.padding)
        throw std::runtime_error("Invalid buffer admission header");
    if (h.status) {
        if (h.status > 0 || h.status < -4095 || packet.size() != sizeof h)
            throw std::runtime_error("Invalid buffer admission error extent");
        if (h.status == -95 || h.status == -38) return {false, 0, 0};
        return {true, h.status, 0};
    }
    if (packet.size() != sizeof h + sizeof(Reply)) throw std::runtime_error("Invalid buffer admission reply extent");
    Reply result{}; std::memcpy(&result, packet.data() + sizeof h, sizeof result);
    if (result.reserved || result.value || result.ntstatus > 0) throw std::runtime_error("Invalid buffer admission result");
    return {result.ntstatus != static_cast<std::int32_t>(0xc00000bbu), 0, result.ntstatus};
}
}
