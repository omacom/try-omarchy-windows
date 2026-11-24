// SPDX-License-Identifier: MIT
#include "native_buffer_preparation.h"
#include <cstdio>
#include <cstdlib>
using namespace driver_bridge;
static void require(bool value) { if (!value) std::abort(); }
int main() {
    const NativeGpuBufferDesc buffer{65536, 0, 0};
    const std::map<std::uint32_t, std::uint32_t> devices{{11, 21}, {12, 21}};
    require(validNativeGpuBufferCaps(NativeGpuBufferProfile));
    for (const auto bytes : {UINT64_C(0), NativeGpuBufferProfile.maxBytes + 1, UINT64_MAX}) require(!validNativeGpuBuffer({bytes, 0, 0}));
    for (const unsigned flags : {1u, 2u, 8u, 0xffffffffu}) require(!validNativeGpuBuffer({65536, flags, 0}));
    require(validNativeGpuBuffer({NativeGpuBufferProfile.maxBytes, 4, 0}) && !validNativeGpuBuffer({65536, 0, 1}));
    auto caps = NativeGpuBufferProfile; caps.version = 2; require(!validNativeGpuBufferCaps(caps));
    caps = NativeGpuBufferProfile; caps.resourceFlags = 7; require(!validNativeGpuBufferCaps(caps));
    caps = NativeGpuBufferProfile; caps.maxBytes *= 2; require(!validNativeGpuBufferCaps(caps));
    NativeBufferPreparations p;
    require(!p.stage(0, buffer, devices) && !p.stage(1, buffer, {}) && !p.stage(1, buffer, {{0,21}}));
    require(p.stage(1, buffer, devices) && p.pendingForDevice(11) && !p.pendingForDevice(99));
    require(!p.stage(1, buffer, devices) && !p.take(2, 11, devices));
    const auto taken = p.take(1, 11, devices); require(taken && taken->bytes == 65536 && p.empty());
    require(!p.take(1, 11, devices));
    require(p.stage(1, buffer, devices) && !p.take(1, 11, {{11,22}}) && p.empty());
    require(p.stage(1, buffer, devices) && !p.take(1, 99, devices) && p.empty());
    require(p.stage(1, buffer, devices)); p.destroyDevice(11);
    require(!p.pendingForDevice(11) && p.pendingForDevice(12));
    require(!p.take(1, 11, devices) && p.empty());
    require(p.stage(1, buffer, devices)); p.cancel(1); require(p.empty());
    for (unsigned i = 1; i <= 16; ++i) require(p.stage(i, buffer, devices));
    require(!p.stage(17, buffer, devices)); p.destroyDevice(11); p.destroyDevice(12); require(p.empty());
    std::printf("NATIVE_BUFFER_PREPARATION_PASS typedBounds=true ownedDevice=true threadBound=true consumedOnce=true capacity=16 gpu=false\n");
}
