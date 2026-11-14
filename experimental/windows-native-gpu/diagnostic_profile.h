// SPDX-License-Identifier: MIT
// A separate executable/initramfs pair; never selected by the app launcher.
#pragma once
#include <cstdint>
#if defined(WDDM_BRIDGE_4K_LAB) && WDDM_BRIDGE_4K_LAB != 1
#error WDDM_BRIDGE_4K_LAB must be 1 or absent
#endif
#if defined(WDDM_BRIDGE_STREAM_LAB) && (!defined(WDDM_BRIDGE_4K_LAB) || WDDM_BRIDGE_STREAM_LAB != 1)
#error WDDM_BRIDGE_STREAM_LAB requires the explicit 4K lab profile
#endif
namespace gpu_diagnostic {
#ifdef WDDM_BRIDGE_STREAM_LAB
constexpr bool Streaming = true;
#else
constexpr bool Streaming = false;
#endif
#ifdef WDDM_BRIDGE_4K_LAB
constexpr bool HighResolution = true;
#else
constexpr bool HighResolution = false;
#endif
constexpr unsigned Width = HighResolution ? 3840 : 130;
constexpr unsigned Height = HighResolution ? 2160 : 73;
// The private high-resolution image can also contain the Mesa GL frontend.
// Leave room for its initramfs and shader compiler without tmpfs exhaustion.
constexpr unsigned GuestMemoryMiB = HighResolution ? 1024 : 512;
constexpr unsigned MaxRowPitch = HighResolution ? 15360 : 4096;
constexpr std::uint64_t MaxReadbackBytes = HighResolution ? 32ull * 1024 * 1024 : 1048576;
constexpr std::uint32_t ExpandedCpuBytes = (HighResolution ? 96u : 32u) * 1024 * 1024;
constexpr std::uint32_t MaxMappingBytes = (HighResolution ? 64u : 4u) * 1024 * 1024;
constexpr std::uint32_t ExpandedGpuBytes = (HighResolution ? 256u : 64u) * 1024 * 1024;
constexpr std::uint64_t ReportedGpuBytes = (HighResolution ? 256ull : 64ull) * 1024 * 1024;
constexpr std::uint32_t Capability = HighResolution ? UINT32_C(0x80000000) : 0;
} // namespace gpu_diagnostic
