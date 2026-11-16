// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
namespace driver_residency {
// An opt-in admission hint, not a physical allocation-size decoder. A mapping
// can cover only part of an allocation, or refer to already shared storage.
// Unknown extents remain unknown; the post-native usage guard is still needed.
struct MappedCandidate {
    std::uint64_t pages=0,nativeReferences=0;
    bool cpuMapped=false;
};
struct MappedAdmission {
    bool valid=true,fits=true;
    std::uint64_t estimateBytes=0,bytesToTrim=0;
    std::size_t estimated=0,alreadyReferenced=0,unknown=0,cpuMapped=0;
};
inline MappedAdmission mappedAdmission(std::uint64_t usage,std::uint64_t limit,
                                      const MappedCandidate* input,std::size_t count) {
    MappedAdmission result;
    if(count && !input) { result.valid=result.fits=false;return result; }
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    for(std::size_t n=0;n<count;++n) {
        const auto& item=input[n];
        // Adding a reference to an already referenced allocation does not
        // represent a first native residency charge. Do not count it twice.
        if(item.nativeReferences) { ++result.alreadyReferenced;continue; }
        // CPU mappings can already charge system memory independently of GPU
        // residency. Never pretend this mapping is a new physical charge.
        if(item.cpuMapped) { ++result.cpuMapped;++result.unknown;continue; }
        if(!item.pages) { ++result.unknown;continue; }
        if(item.pages>maximum/4096 || item.pages*4096>maximum-result.estimateBytes) {
            result.valid=result.fits=false;return result;
        }
        result.estimateBytes+=item.pages*4096;++result.estimated;
    }
    if(usage>limit) {
        const auto excess=usage-limit;
        if(result.estimateBytes>maximum-excess) { result.valid=result.fits=false;return result; }
        result.bytesToTrim=excess+result.estimateBytes;result.fits=false;
    } else if(result.estimateBytes>limit-usage) {
        result.bytesToTrim=result.estimateBytes-(limit-usage);result.fits=false;
    }
    return result;
}
} // namespace driver_residency
