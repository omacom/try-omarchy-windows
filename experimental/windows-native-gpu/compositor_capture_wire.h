// SPDX-License-Identifier: MIT
// Bounded diagnostic capture of an owned compositor buffer. No host HANDLEs.
#pragma once
#include <cstdint>
namespace driver_bridge {
struct CompositorCaptureDesc {
    std::uint32_t syncTicket, width, height, format, stride, flags;
    std::uint64_t fence;
};
static_assert(sizeof(CompositorCaptureDesc) == 32, "fixed compositor capture layout");
inline bool validCompositorCapture(CompositorCaptureDesc d) {
    return d.syncTicket && d.width && d.height && d.width <= 4096 && d.height <= 4096 &&
        d.stride == d.width * 4 && !d.flags && d.fence && d.fence != UINT64_MAX &&
        (d.format == 0x34325241u || d.format == 0x34325258u || d.format == 0x34324241u || d.format == 0x34324258u);
}
inline std::uint32_t compositorDxgiFormat(std::uint32_t fourcc) {
    return fourcc == 0x34325241u || fourcc == 0x34325258u ? 87u :
           fourcc == 0x34324241u || fourcc == 0x34324258u ? 28u : 0u;
}
constexpr std::uint32_t CompositorFrameAudit = 1;
// The final reply acknowledges output-consumer retirement and live detach.
constexpr std::uint32_t CompositorFrameFinal = 2;
inline bool validCompositorFrame(CompositorCaptureDesc d) {
    const auto flags = d.flags; d.flags = 0;
    return !(flags & ~(CompositorFrameAudit|CompositorFrameFinal)) && validCompositorCapture(d);
}
}
