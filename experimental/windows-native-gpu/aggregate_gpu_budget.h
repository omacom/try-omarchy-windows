// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <array>
namespace driver_budget {
// floor(part*amount/whole), without an overflowing intermediate product.
// part<=whole ensures the quotient never exceeds amount.
inline std::uint64_t proportional(std::uint64_t part,std::uint64_t amount,std::uint64_t whole) {
    if(!whole || part>whole) return 0;
    std::uint64_t quotient=0,remainder=0;
    for(unsigned bit=64;bit--;) {
        const bool carry=remainder>=whole-remainder;
        remainder=carry ? remainder-(whole-remainder) : remainder+remainder;
        quotient=quotient*2+(carry?1u:0u);
        if((amount>>bit)&1u) {
            if(remainder>=whole-part) { remainder-=whole-part;++quotient; }
            else remainder+=part;
        }
    }
    return quotient;
}
struct Partition { bool valid=false;std::array<std::uint64_t,2> bytes{}; };
inline Partition partition(std::uint64_t localCapacity,std::uint64_t nonlocalCapacity,
                           std::uint64_t aggregateLimit,std::uint64_t presentationReserve) {
    if(presentationReserve>aggregateLimit || localCapacity>UINT64_MAX-nonlocalCapacity ||
       (!localCapacity && !nonlocalCapacity)) return {};
    const auto total=localCapacity+nonlocalCapacity;
    const auto available=aggregateLimit-presentationReserve;
    if(available>=total) return {true,{localCapacity,nonlocalCapacity}};
    const auto local=proportional(localCapacity,available,total);
    return {true,{local,available-local}};
}
}
