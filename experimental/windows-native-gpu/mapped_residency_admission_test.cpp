// SPDX-License-Identifier: MIT
#include "mapped_residency_admission.h"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace driver_residency;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("mapped admission line "+std::to_string(__LINE__)); } while(0)
int main() {
    constexpr std::uint64_t limit=1073741824;
    const std::array<MappedCandidate,1> observed{{{8128,0,false}}};
    auto p=mappedAdmission(1062191104,limit,observed.data(),observed.size());
    CHECK(p.valid && !p.fits && p.estimateBytes==33292288 && p.bytesToTrim==21741568 && p.estimated==1 && !p.unknown);
    // The plan is a read-only snapshot: no logical/native references moved.
    CHECK(observed[0].nativeReferences==0 && observed[0].pages==8128);
    p=mappedAdmission(limit-33292288,limit,observed.data(),1);
    CHECK(p.valid && p.fits && !p.bytesToTrim);
    p=mappedAdmission(limit-33292288+1,limit,observed.data(),1);
    CHECK(p.valid && !p.fits && p.bytesToTrim==1);
    const std::array<MappedCandidate,4> mixed{{{8128,1,false},{0,0,false},{8128,0,true},{1,0,false}}};
    p=mappedAdmission(limit-4096,limit,mixed.data(),mixed.size());
    CHECK(p.valid && p.fits && p.estimateBytes==4096 && p.estimated==1 && p.alreadyReferenced==1 && p.unknown==2 && p.cpuMapped==1);
    // Unknown bytes are never turned into a claimed physical-size estimate.
    const MappedCandidate unknown{};p=mappedAdmission(limit,limit,&unknown,1);
    CHECK(p.valid && p.fits && p.unknown==1 && !p.estimateBytes);
    p=mappedAdmission(limit+1,limit,nullptr,0);CHECK(p.valid && !p.fits && p.bytesToTrim==1);
    p=mappedAdmission(0,limit,nullptr,1);CHECK(!p.valid && !p.fits);
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    const MappedCandidate tooLarge{maximum/4096+1,0,false};
    p=mappedAdmission(0,maximum,&tooLarge,1);CHECK(!p.valid && !p.fits);
    const std::array<MappedCandidate,2> overflowing{{{maximum/4096,0,false},{1,0,false}}};
    p=mappedAdmission(0,maximum,overflowing.data(),2);CHECK(!p.valid && !p.fits);
    const MappedCandidate page{1,0,false};p=mappedAdmission(maximum,0,&page,1);CHECK(!p.valid && !p.fits);
    p=mappedAdmission(maximum,maximum,&page,1);CHECK(p.valid && !p.fits && p.bytesToTrim==4096);
    // A mixed batch is assessed in full; never admit an inexpensive prefix.
    const std::array<MappedCandidate,2> batch{{{1,0,false},{8128,0,false}}};
    p=mappedAdmission(1062191104,limit,batch.data(),2);CHECK(p.valid && !p.fits && p.estimated==2 && p.bytesToTrim==21745664);
    std::cout<<"MAPPED_RESIDENCY_ADMISSION observedOverrunRefused=true exactHeadroom=true noReferenceMutation=true repeatedReferencesNotDoubleCounted=true unknownExtentsExplicit=true cpuMappingNotPhysicalSize=true overflowRejected=true wholeBatch=true mock=true gpu=false\n";
}
