// SPDX-License-Identifier: MIT
// Trusted controller-only live retirement. No ordinary guest wire operation.
#pragma once
#include <cstdint>
namespace driver_bridge {
struct LiveRetirementProfile {
    std::uint32_t version=1,batch=16,phases=4,reserved=0;
};
struct LiveRetirementProgress {
    std::uint32_t version=1,phase=0,remaining=0,attempted=0,destroyed=0,sharedReferences=0,flags=0;
    std::int32_t ntstatus=0;
};
constexpr std::uint32_t RetirementGpuBusy=1,RetirementSharedReferences=2,RetirementNativeBusy=4;
static_assert(sizeof(LiveRetirementProfile)==16 && sizeof(LiveRetirementProgress)==32,"Fixed live retirement controls");
inline bool validLiveRetirementProfile(LiveRetirementProfile p) {
    return p.version==1 && p.batch==16 && p.phases==4 && !p.reserved;
}
inline bool validLiveRetirementProgress(LiveRetirementProgress p) {
    return p.version==1 && p.phase<=4 && p.remaining<=1024 && p.attempted<=16 && p.destroyed<=p.attempted &&
        p.sharedReferences<=32 && p.sharedReferences<=p.remaining && !(p.flags&~7u) && p.ntstatus<=0 &&
        bool(p.flags&RetirementNativeBusy)==bool(p.ntstatus) &&
        (!(p.flags&RetirementSharedReferences) || p.sharedReferences) &&
        (p.remaining ? p.phase<4 : p.phase==4 && !p.flags && !p.sharedReferences);
}
// Terminal progress describes one bounded actor step and the whole group's
// completion separately. An empty actor does not imply an empty group.
struct StoppedGroupRetirementProgress {
    LiveRetirementProgress step;
    std::uint64_t actor=0;
    std::uint32_t actorsRemaining=0,reserved=0;
};
static_assert(sizeof(StoppedGroupRetirementProgress)==48,"Fixed terminal retirement response");
constexpr unsigned MaxStoppedGroupSteps=8*(1024+4);
inline bool validStoppedGroupRetirementProgress(StoppedGroupRetirementProgress p,unsigned limit=8) {
    return (limit==4 || limit==8) && validLiveRetirementProgress(p.step) && !p.reserved &&
        p.actorsRemaining<=limit && (p.actorsRemaining || !p.step.remaining) &&
        (p.actor || (!p.actorsRemaining && !p.step.remaining && !p.step.attempted && !p.step.destroyed));
}
}
