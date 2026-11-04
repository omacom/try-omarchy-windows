// SPDX-License-Identifier: MIT
#pragma once
#include "driver_wire.h"
#include <cerrno>
#include <chrono>
#include <exception>
#include <system_error>

namespace driver_bridge {
constexpr unsigned StoppedGroupAdmissionTimeoutMs = 8000;
struct StoppedGroupPollSummary { unsigned attempts=0,destroyed=0; };
// Only the controller's validated RetireStoppedGroup reply may produce this.
// An EBUSY from opening/transferring the native port is a transport failure.
struct StoppedGroupPending : std::exception {
    const char* what() const noexcept override { return "Native presentation drain pending"; }
};

// Busy never supplies progress or releases an object. Transport faults,
// malformed progress, step exhaustion and late completion remain failures.
template<class Request,class Now,class Pause>
StoppedGroupPollSummary pollStoppedGroupRetirement(Request&& request,Now&& now,Pause&& pause,
                                                  std::uint32_t actorLimit) {
    const auto deadline=now()+std::chrono::milliseconds(StoppedGroupAdmissionTimeoutMs);
    StoppedGroupPollSummary summary;
    while(summary.attempts<MaxStoppedGroupSteps && now()<deadline) {
        ++summary.attempts;
        StoppedGroupRetirementProgress progress;
        try { progress=request(); }
        catch(const StoppedGroupPending&) {
            pause();continue;
        }
        if(!validStoppedGroupRetirementProgress(progress,actorLimit))
            throw std::runtime_error("Terminal retirement progress mismatch");
        if(now()>=deadline) break;
        summary.destroyed+=progress.step.destroyed;
        if(!progress.actorsRemaining) return summary;
        pause();
    }
    throw std::runtime_error("Terminal group retirement did not finish within its bounded deadline");
}
}
