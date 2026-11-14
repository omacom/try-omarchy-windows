// SPDX-License-Identifier: MIT
#include "bounded_gpu_retirement_wait.h"
#include <iostream>
#include <stdexcept>
static void require(bool value) { if(!value) throw std::runtime_error("Bounded retirement wait control"); }
int main() {
    using driver_bridge::boundedGpuRetirementWait;
    std::uint64_t time=0;unsigned polls=0,yields=0;
    require(boundedGpuRetirementWait([&]{++polls;return true;},[&]{return time;},[&]{++yields;++time;}));
    require(polls==1 && !yields && !time);
    polls=yields=0;
    require(boundedGpuRetirementWait([&]{++polls;return time>=7;},[&]{return time;},[&]{++yields;++time;}));
    require(time==7 && yields==7 && polls==8);
    time=0;polls=yields=0;
    require(!boundedGpuRetirementWait([&]{++polls;return false;},[&]{return time;},[&]{++yields;++time;}));
    require(time==250 && yields==250 && polls==251);
    time=0;polls=yields=0;
    require(!boundedGpuRetirementWait([&]{++polls;return false;},[&]{return time;},[&]{++yields;}));
    require(yields==256 && polls==257 && !time);
    time=UINT64_MAX-3;polls=yields=0;
    require(!boundedGpuRetirementWait([&]{++polls;return false;},[&]{return time;},[&]{++yields;time+=100;}));
    require(yields==3 && polls==4);
    std::cout<<"GPU_RETIREMENT_WAIT_CONTROL immediateNoSleep=true actualPredicateRequired=true timeout250ms=true attemptsBounded=true clockWrap=true gpu=false\n";
}
