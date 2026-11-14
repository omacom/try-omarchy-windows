// SPDX-License-Identifier: MIT
#include "gpu_mapping_accounting.h"
#include <stdexcept>
#include <iostream>
static void require(bool value) { if(!value) throw std::runtime_error("GPU mapping accounting regression"); }
int main() {
    constexpr std::uint32_t standalone=262144;
    constexpr std::uint64_t reserved=64ull*1024*1024*1024/4096;
    using driver_mapping::fits;
    // Recorded failing request: separate owned-reservation pages from the
    // standalone quota. The same request without ownership remains bounded.
    require(!fits(258869,0,8160,standalone,reserved,false));
    require(fits(258869,200000,8160,standalone,reserved,true));
    require(fits(standalone+1000000,1000000,1,standalone,reserved,true));
    require(!fits(standalone+1000000,1000000,1,standalone,reserved,false));
    require(!fits(1,2,1,standalone,reserved,true));
    require(!fits(10,10,1,standalone,9,true));
    require(fits(10,10,reserved-10,standalone,reserved,true));
    require(!fits(10,10,reserved-9,standalone,reserved,true));
    require(!fits(UINT32_MAX,0,1,UINT32_MAX,reserved,false));
    require(!fits(0,0,0,standalone,reserved,true));
    require(!fits(standalone+1,0,1,standalone,reserved,true));
    std::cout<<"PASS: owned reservation capacity separated from standalone mappings, overflow and legacy quotas\n";
}
