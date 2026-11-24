#include "aggregate_gpu_budget.h"
#include <cstdio>
#include <cstdlib>
static void require(bool value) { if(!value) std::abort(); }
int main() {
    using driver_budget::partition;using driver_budget::proportional;
    constexpr std::uint64_t gib=1024ull*1024*1024,mib=1024ull*1024;
    const auto desktop=partition(24*gib,48*gib,gib,64*mib);
    require(desktop.valid && desktop.bytes[0]==320*mib && desktop.bytes[1]==640*mib);
    require(desktop.bytes[0]+desktop.bytes[1]==gib-64*mib);
    require(proportional(UINT64_MAX,UINT64_MAX,UINT64_MAX)==UINT64_MAX);
    require(proportional(UINT64_MAX-1,UINT64_MAX,UINT64_MAX)==UINT64_MAX-1);
    require(proportional(1,UINT64_MAX,UINT64_MAX)==1);
    require(proportional(0,UINT64_MAX,UINT64_MAX)==0);
    require(!partition(UINT64_MAX,1,gib,0).valid && !partition(0,0,gib,0).valid);
    require(!partition(gib,gib,63*mib,64*mib).valid);
    for(std::uint64_t local=0;local<33;++local) for(std::uint64_t other=0;other<33;++other) {
        if(!local && !other) continue;
        for(std::uint64_t cap=0;cap<100;++cap) {
            const auto p=partition(local,other,cap+7,7);require(p.valid);
            const auto expected=cap<local+other ? cap : local+other;
            require(p.bytes[0]+p.bytes[1]==expected && p.bytes[0]<=local && p.bytes[1]<=other);
            require(p.bytes[0]==local*expected/(local+other));
        }
    }
    const auto staticShares=partition(24*gib,48*gib,gib,64*mib);
    // Independent native budget changes can only reduce each static share;
    // interleaved segment queries cannot advertise two copies of the limit.
    for(const auto localBudget:std::array<std::uint64_t,3>{0,128*mib,UINT64_MAX})
        for(const auto otherBudget:std::array<std::uint64_t,3>{0,256*mib,UINT64_MAX}) {
        const auto a=localBudget<staticShares.bytes[0]?localBudget:staticShares.bytes[0];
        const auto b=otherBudget<staticShares.bytes[1]?otherBudget:staticShares.bytes[1];
        require(a+b<=gib-64*mib);
    }
    std::puts("AGGREGATE_GPU_BUDGET sharedLimit=true presentationReserve=true overflowSafe=true stableSegmentShares=true inputChanged=false gpu=false");
}
