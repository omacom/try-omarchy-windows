// SPDX-License-Identifier: MIT
#include "application_memory_profile.h"
#include <iostream>
#include <stdexcept>
using namespace driver_bridge;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("Application memory profile check"); } while(0)
int main() {
    CHECK(validApplicationMemoryProfile(GuestApplicationMemoryProfile));
    CHECK(validApplicationMemoryCapacity(GuestApplicationMemoryCapacity));
    CHECK(!validCoordinatedPresentationCapacity(GuestApplicationMemoryCapacity));
    CHECK(!validApplicationMemoryCapacity(GuestOpenGlMultiappCapacity));
    for(unsigned mutation=0;mutation<3;++mutation) {
        auto p=GuestApplicationMemoryProfile;
        if(mutation==0) ++p.version;
        if(mutation==1) ++p.reserved;
        if(mutation==2) ++p.reportedBytes;
        CHECK(!validApplicationMemoryProfile(p));
    }
    for(unsigned mutation=0;mutation<7;++mutation) {
        auto c=GuestApplicationMemoryCapacity;
        if(mutation==0) --c.mapping.aperture.version;
        if(mutation==1) ++c.mapping.aperture.kernelVersion;
        if(mutation==2) ++c.mapping.aperture.cpuByteLimit;
        if(mutation==3) ++c.mapping.aperture.cpuSlots;
        if(mutation==4) ++c.mapping.aperture.fenceSlots;
        if(mutation==5) ++c.mapping.gpuMappedByteLimit;
        if(mutation==6) ++c.reportedGpuByteLimit;
        CHECK(!validApplicationMemoryCapacity(c));
    }
    const auto p=applicationMemoryPartition();CHECK(p.valid);
    CHECK(p.bytes[0]+p.bytes[1]+ApplicationPresentationReserve==GuestApplicationMemoryProfile.reportedBytes);
    const std::array<std::uint64_t,2> boundary{(p.bytes[0]+ApplicationPresentationReserve)*2,p.bytes[1]*2};
    CHECK(applicationMemoryNativeBudgetAvailable(boundary,{0,0}));
    CHECK(!applicationMemoryNativeBudgetAvailable(boundary,{1,0}));
    CHECK(!applicationMemoryNativeBudgetAvailable(boundary,{0,1}));
    CHECK(!applicationMemoryNativeBudgetAvailable({0,0},{0,0}));
    CHECK(!applicationMemoryNativeBudgetAvailable(boundary,{UINT64_MAX,0}));
    CHECK(applicationMemoryNativeBudgetAvailable({UINT64_MAX,UINT64_MAX},{0,0}));
    CHECK(!applicationMemoryNativeBudgetAvailable({UINT64_MAX,UINT64_MAX},{UINT64_MAX,UINT64_MAX}));
    CHECK(reportedGpuBudgetFits(GuestApplicationMemoryProfile.reportedBytes-1,1,GuestApplicationMemoryProfile.reportedBytes));
    CHECK(!reportedGpuBudgetFits(GuestApplicationMemoryProfile.reportedBytes,1,GuestApplicationMemoryProfile.reportedBytes));
    std::cout<<"APPLICATION_MEMORY_PROFILE explicit=true legacyCapacityRejected=true segmentsShareOnePool=true presentationReserve=true nativeAvailableHeadroomChecked=true overflowSafe=true mock=true gpu=false\n";
}
