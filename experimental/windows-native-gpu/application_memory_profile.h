// SPDX-License-Identifier: MIT
#pragma once
#include "opengl_profile.h"
#include "aggregate_gpu_budget.h"
#include <cstddef>
namespace driver_bridge {
// A separate trusted-controller payload and Info flag select this agreement.
// Profile 14's kernel apertures/object quotas stay fixed. This is reported
// process usage, not a physical GPU partition or an allocation-size quota.
struct ApplicationMemoryProfile {
    std::uint32_t version=1,reserved=0;
    std::uint64_t reportedBytes=4ull*1024*1024*1024;
};
constexpr ApplicationMemoryProfile GuestApplicationMemoryProfile{};
constexpr std::uint64_t ApplicationPresentationReserve=64ull*1024*1024;
static_assert(sizeof(ApplicationMemoryProfile)==16,"Explicit application memory agreement");
inline bool validApplicationMemoryProfile(ApplicationMemoryProfile p) {
    return p.version==1 && !p.reserved && p.reportedBytes==GuestApplicationMemoryProfile.reportedBytes;
}
inline bool validApplicationMemoryCapacity(OpenGlDesktopPresentationCapacity c) {
    if(c.reportedGpuByteLimit!=GuestApplicationMemoryProfile.reportedBytes) return false;
    c.reportedGpuByteLimit=OpenGlDesktopPresentationReportedBytes;
    return validOpenGlMultiappCapacity(c);
}
constexpr OpenGlDesktopPresentationCapacity GuestApplicationMemoryCapacity{
    GuestOpenGlMultiappCapacity.mapping,GuestApplicationMemoryProfile.reportedBytes};
inline driver_budget::Partition applicationMemoryPartition() {
    // Prefer local GPU memory for applications. The aggregate pool is shared
    // by all actors; presentation headroom is not advertised to applications.
    return {true,{3ull*1024*1024*1024,1024ull*1024*1024-ApplicationPresentationReserve}};
}
inline bool applicationMemoryNativeBudgetAvailable(const std::array<std::uint64_t,2>& budgets,
                                                 const std::array<std::uint64_t,2>& usage) {
    auto shares=applicationMemoryPartition();
    // Headroom admission covers the entire pool, including the local reserve
    // withheld from application budget queries.
    shares.bytes[0]+=ApplicationPresentationReserve;
    for(std::size_t n=0;n<shares.bytes.size();++n)
        if(usage[n]>budgets[n] || shares.bytes[n]>(budgets[n]-usage[n])/2) return false;
    return true;
}
}
