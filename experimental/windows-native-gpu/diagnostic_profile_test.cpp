// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include "cpu_aperture.h"
#include <cstdlib>
#include <iostream>
static void require(bool ok) { if (!ok) std::abort(); }
int main() {
    using namespace driver_bridge;
    using namespace gpu_diagnostic;
    require(vendorCpuByteLimit(0) == 16 * 1024 * 1024);
    require(vendorGpuPageLimit(0) == 32 * 1024 * 1024 / 4096);
    require(vendorCpuByteLimit(ExpandedVendorCpuCapability) == ExpandedCpuBytes);
    require(vendorGpuPageLimit(ExpandedVendorGpuCapability) == ExpandedGpuBytes / 4096);
    require(vendorCpuBudgetFits(ExpandedCpuBytes - 4096, 4096, ExpandedVendorCpuCapability));
    require(!vendorCpuBudgetFits(ExpandedCpuBytes - 4096, 8192, ExpandedVendorCpuCapability));
    require(!vendorCpuBudgetFits(UINT32_MAX, 4096, ExpandedVendorCpuCapability));
    require(vendorGpuBudgetFits(ExpandedGpuBytes / 4096 - 1, 1, ExpandedVendorGpuCapability));
    require(!vendorGpuBudgetFits(ExpandedGpuBytes / 4096, 1, ExpandedVendorGpuCapability));
    require(!vendorGpuBudgetFits(UINT32_MAX, UINT64_MAX, ExpandedVendorGpuCapability));
    require(validNativeTexture({Width,Height,28,1}));
    require(!validNativeTexture({Width + 1,Height,28,1}));
    require(!validNativeTexture({Width,Height + 1,28,1}));
    require(!validNativeTexture({Width,Height,87,1}));
    require(!validNativeTexture({Width,Height,28,0}));
    require(validNativeTexture({3840,2160,28,1}) == HighResolution);
    require(validNativeTexture({130,73,28,1}) != HighResolution);
    require(validVendorCpuReply(0, {MaxMappingBytes,0,1}));
    require(!validVendorCpuReply(0, {MaxMappingBytes + 4096,0,1}));
    GpuVaDesc map{1,0,0,65536,1ull << 40,0,MaxVendorMapPages,1,0};
    require(validGpuVa(map));
    ++map.sizePages; require(!validGpuVa(map));
    map.sizePages = MaxVendorMapPages; map.offsetPages = 1; require(!validGpuVa(map));
    require(MaxMappingBytes == (HighResolution ? 64u : 4u) * 1024 * 1024);
    require(ExpandedCpuBytes == (HighResolution ? 96u : 32u) * 1024 * 1024);
    require(ExpandedGpuBytes == (HighResolution ? 256u : 64u) * 1024 * 1024);
    require(Capability == (HighResolution ? UINT32_C(0x80000000) : 0));
    require(GuestMemoryMiB == (HighResolution ? 1024u : 512u));
    std::cout << "PASS: bounded " << Width << 'x' << Height << " lab profile\n";
}
