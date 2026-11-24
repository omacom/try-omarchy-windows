// SPDX-License-Identifier: MIT
// Bounded native buffer descriptions; texture acceptance remains independent.
#pragma once
#include <cstdint>
namespace driver_shared {
constexpr std::uint64_t MaxSharedBufferBytes = 64ull * 1024 * 1024;
struct BufferDescription {
    std::uint32_t dimension;
    std::uint64_t alignment, width;
    std::uint32_t height, layers, levels, format, samples, quality, layout, flags;
};
inline bool validBufferDescription(BufferDescription d) {
    // D3D12 BUFFER / UNKNOWN format / ROW_MAJOR, one level/layer/sample.
    // No render-target, depth, cross-adapter, video or simultaneous-access flags.
    return d.dimension == 1 && (d.alignment == 0 || d.alignment == 65536) &&
        d.width && d.width <= MaxSharedBufferBytes && d.height == 1 && d.layers == 1 && d.levels == 1 &&
        d.format == 0 && d.samples == 1 && d.quality == 0 && d.layout == 1 && (d.flags == 0 || d.flags == 4);
}
}
