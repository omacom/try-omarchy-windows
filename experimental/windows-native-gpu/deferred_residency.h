// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace driver_residency {
// guest + retained is the number of real native residency references owned.
// Transferring ownership never calls Evict or changes the native total.
struct References {
    std::uint32_t guest=0,retained=0;
    bool known=true;
    std::uint64_t native() const { return std::uint64_t{guest}+retained; }
    bool valid() const { return known && native()<=UINT32_MAX; }
    bool canAdmit() const { return valid() && guest<UINT32_MAX && native()<UINT32_MAX; }
    bool admit() { if(!canAdmit()) return false; ++guest;return true; }
    bool canDefer() const { return valid() && guest && retained<UINT32_MAX; }
    bool defer() { if(!canDefer()) return false; --guest;++retained;return true; }
    bool canReuse() const { return valid() && retained && guest<UINT32_MAX; }
    bool reuse() { if(!canReuse()) return false; --retained;++guest;return true; }
    bool canRelease(bool cpuMapped,bool gpuRetired) const {
        return valid() && retained && !cpuMapped && gpuRetired;
    }
    bool release(bool cpuMapped,bool gpuRetired) {
        if(!canRelease(cpuMapped,gpuRetired)) return false;
        --retained;return true;
    }
    // A partially failing native list has no per-allocation receipt. Keep
    // ownership until VM teardown instead of guessing which references moved.
    void uncertain() { known=false; }
};
}
