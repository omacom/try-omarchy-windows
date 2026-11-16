// SPDX-License-Identifier: MIT
#include "staging_reclaim.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace driver_shared;
static void require(bool value) { if(!value) throw std::runtime_error("Staging reclamation control failed"); }
int main() {
    constexpr std::uint64_t limit=1073741824,texture=33423360;
    require(!stagingPressureTarget(limit-ResidencyStagingReserve,limit));
    require(stagingPressureTarget(limit-1,limit)==ResidencyStagingReserve-1);
    require(!stagingPressureTarget(limit+1,limit) && !stagingPressureTarget(0,0));
    std::array<StagingReclaimState,3> slots{{{true,false,texture,7},{true,false,texture,8},{true,false,texture,9}}};
    std::vector<unsigned> released;
    auto drop=[&](unsigned index,std::uint64_t bytes) { require(index && slots[index].present && bytes==texture);released.push_back(index);return true; };
    auto r=reclaimIdleStaging(slots,9,9,texture,drop);
    require(!r.failed && r.textures==1 && r.bytes==texture && released==std::vector<unsigned>{2});
    released.clear();r=reclaimIdleStaging(slots,9,9,3*texture,drop);
    require(!r.failed && r.textures==2 && r.bytes==2*texture && released==std::vector<unsigned>({2,1}));
    released.clear();slots[2].occupied=true;
    r=reclaimIdleStaging(slots,9,9,2*texture,drop);
    require(r.textures==1 && released==std::vector<unsigned>{1});
    released.clear();slots[1].reusedAfter=10;
    r=reclaimIdleStaging(slots,9,9,2*texture,drop);require(!r.textures && released.empty());
    slots[2].occupied=false;slots[1].reusedAfter=8;
    r=reclaimIdleStaging(slots,7,9,2*texture,drop);require(!r.textures && released.empty());
    r=reclaimIdleStaging(slots,9,7,2*texture,drop);require(!r.textures && released.empty());
    r=reclaimIdleStaging(slots,UINT64_MAX,9,texture,drop);require(r.failed && released.empty());
    r=reclaimIdleStaging(slots,9,UINT64_MAX,texture,drop);require(r.failed && released.empty());
    r=reclaimIdleStaging(slots,9,9,texture,[](unsigned,std::uint64_t){return false;});require(r.failed && !r.bytes && !r.textures);
    slots[2].reusedAfter=0;slots[1].present=false;
    r=reclaimIdleStaging(slots,9,9,texture,drop);require(!r.textures && released.empty());
    slots[2]={true,false,texture,11};
    r=reclaimIdleStaging(slots,11,10,texture,drop);require(!r.textures && released.empty());
    r=reclaimIdleStaging(slots,11,11,texture,drop);require(r.textures==1 && released==std::vector<unsigned>{2});
    released.clear();slots[2].bytes=0;
    r=reclaimIdleStaging(slots,11,11,texture,drop);require(r.failed && released.empty());
    r=reclaimIdleStaging(slots,UINT64_MAX,UINT64_MAX,0,drop);require(!r.failed && released.empty());
    std::puts("STAGING_RECLAIM_CONTROL threeSlotCapacity=true primaryPreserved=true occupiedRetained=true bothFencesRequired=true deviceLossRejected=true failedReleaseCharged=true reuse=true gpu=false");
}
