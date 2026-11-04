// SPDX-License-Identifier: MIT
// The production poll loop with a fake clock and typed native replies; no GPU.
#include "stopped_group_retirement_poll.h"
#include <cassert>
#include <cstdio>
using namespace driver_bridge;
struct Clock {
    std::chrono::steady_clock::time_point current{};
    auto now() const { return current; }
    void advance(unsigned ms) { current+=std::chrono::milliseconds(ms); }
};
static StoppedGroupRetirementProgress complete() {
    StoppedGroupRetirementProgress value;value.step.phase=4;return value;
}
int main() {
    Clock clock;unsigned requests=0;
    auto summary=pollStoppedGroupRetirement([&] {
        if(++requests<=3) throw StoppedGroupPending{};
        return complete();
    },[&] { return clock.now(); },[&] { clock.advance(1); },4);
    assert(summary.attempts==4 && !summary.destroyed && requests==4);
    clock=Clock{};requests=0;
    bool rejected=false;
    try {
        (void)pollStoppedGroupRetirement([&]() -> StoppedGroupRetirementProgress {
            ++requests;throw StoppedGroupPending{};
        },[&] { return clock.now(); },[&] { clock.advance(1000); },4);
    } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && requests==8);
    clock=Clock{};requests=0;rejected=false;
    try {
        (void)pollStoppedGroupRetirement([&]() -> StoppedGroupRetirementProgress {
            ++requests;throw std::system_error(EIO,std::generic_category(),"transport fault");
        },[&] { return clock.now(); },[] {},4);
    } catch(const std::system_error& error) { rejected=error.code().value()==EIO; }
    assert(rejected && requests==1);
    requests=0;rejected=false;
    try {
        (void)pollStoppedGroupRetirement([&]() -> StoppedGroupRetirementProgress {
            ++requests;throw std::system_error(EBUSY,std::generic_category(),"port busy, not a verified reply");
        },[&] { return clock.now(); },[] {},4);
    } catch(const std::system_error& error) { rejected=error.code().value()==EBUSY; }
    assert(rejected && requests==1);
    requests=0;rejected=false;
    try {
        (void)pollStoppedGroupRetirement([&] {
            ++requests;auto value=complete();value.step.phase=99;return value;
        },[&] { return clock.now(); },[] {},4);
    } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && requests==1);
    clock=Clock{};rejected=false;
    try {
        (void)pollStoppedGroupRetirement([&] {
            clock.advance(StoppedGroupAdmissionTimeoutMs);return complete();
        },[&] { return clock.now(); },[] {},4);
    } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected);
    clock=Clock{};requests=0;rejected=false;
    try {
        (void)pollStoppedGroupRetirement([&]() -> StoppedGroupRetirementProgress {
            ++requests;throw StoppedGroupPending{};
        },[&] { return clock.now(); },[] {},4);
    } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && requests==MaxStoppedGroupSteps);
    std::puts("STOPPED_GROUP_POLL_PASS busyRetains=true boundedTime=true boundedAttempts=true transportFaultPropagated=true malformedRejected=true lateCompleteRejected=true gpu=false");
}
