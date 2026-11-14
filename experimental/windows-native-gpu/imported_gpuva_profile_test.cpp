// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace driver_bridge;
static unsigned controls = 0;
static void require(bool value) { if (!value) std::abort(); }
static void reject(GpuVaDesc d) { require(!validImportedGpuVa(d, true)); ++controls; }
int main() {
    const GpuVaDesc opened{1, 0, 0, 67108864, MaxGpuAddress, 0, 16, 1, VendorImportedDriverProtection};
    const auto original = opened;
    require(!validGpuVa(opened) && !validGpuVaOutput(opened, opened.minimum));
    require(!validImportedGpuVa(opened, false) && !validImportedGpuVaOutput(opened, opened.minimum, false));
    require(validImportedGpuVa(opened, true) && validImportedGpuVaOutput(opened, opened.minimum, true));
    require(std::memcmp(&opened, &original, sizeof opened) == 0); // Validation cannot rewrite opaque input.
    auto d = opened; d.driverProtection = VendorDefaultDriverProtection;
    require(validGpuVa(d) && validImportedGpuVa(d, false) && validImportedGpuVa(d, true));
    d.driverProtection = 0; require(validGpuVa(d) && validImportedGpuVa(d, false));
    d = opened; d.queue = 0; reject(d);
    d = opened; d.reserved = 1; reject(d);
    d = opened; d.sizePages = 0; reject(d);
    d = opened; d.sizePages = MaxVendorMapPages + 1ull; reject(d);
    d = opened; d.offsetPages = MaxVendorMapPages; reject(d);
    d = opened; d.protection = 0; reject(d);
    d = opened; d.protection = 2; reject(d);
    d = opened; d.protection = 3; reject(d);
    d = opened; d.protection = 4; reject(d);
    d = opened; d.driverProtection = 0x10000008; reject(d);
    d = opened; d.driverProtection = 0x1000000b; reject(d);
    d = opened; d.driverProtection |= UINT64_C(0x8000000000000000); reject(d);
    d = opened; d.base = 1; reject(d);
    d = opened; d.base = MaxGpuAddress; reject(d);
    d = opened; d.base = MaxGpuAddress - 4096; reject(d);
    d = opened; d.minimum += 1; reject(d);
    d = opened; d.maximum -= 1; reject(d);
    d = opened; d.maximum = opened.minimum + 4096; reject(d);
    for (const auto address : {UINT64_C(0), opened.minimum + 1, opened.minimum - 4096, MaxGpuAddress}) {
        require(!validImportedGpuVaOutput(opened, address, true)); ++controls;
    }
    d = opened; d.base = opened.minimum;
    require(validImportedGpuVaOutput(d, d.base, true));
    require(!validImportedGpuVaOutput(d, d.base + 4096, true)); ++controls;
    std::printf("IMPORTED_GPUVA_PROFILE_PASS rejected=%u ordinaryValidatorUnchanged=true forwardedValueUnchanged=true gpu=false\n", controls);
}
