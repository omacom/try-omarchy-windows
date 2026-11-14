// SPDX-License-Identifier: MIT
// Observe an actual reply without changing it or claiming client delivery.
#pragma once
#include "driver_wire.h"
#include <cstring>
#include <optional>
namespace guest_broker {
struct ResidencyFailureObservation {
    std::uint32_t queue,requested,flags,returned;
    std::int32_t ntstatus;
    std::uint64_t pagingFence,bytesToTrim;
};
inline std::optional<ResidencyFailureObservation> residencyFailureObservation(
        const std::vector<std::uint8_t>& request,const std::vector<std::uint8_t>& response) {
    using namespace driver_bridge;
    if(request.size()<sizeof(Header)+sizeof(ResidentDesc) ||
       response.size()!=sizeof(Header)+sizeof(Reply)+sizeof(ResidentReply)) return {};
    Header input{},output{};Reply status{};ResidentDesc description{};ResidentReply resident{};
    std::memcpy(&input,request.data(),sizeof input);
    std::memcpy(&output,response.data(),sizeof output);
    std::memcpy(&status,response.data()+sizeof output,sizeof status);
    if(input.type!=static_cast<std::uint32_t>(Op::MakeVendorResident) || !input.handle ||
       input.status || input.padding || output.type!=input.type || output.handle!=input.handle ||
       output.status || output.padding || status.reserved || status.ntstatus>=0) return {};
    std::memcpy(&description,request.data()+sizeof input,sizeof description);
    if(!validResident(description) || request.size()!=sizeof(Header)+sizeof(ResidentDesc)+
       description.count*sizeof(std::uint32_t)*(1+description.priorities)) return {};
    std::memcpy(&resident,response.data()+sizeof output+sizeof status,sizeof resident);
    if(resident.reserved || resident.count>description.count) return {};
    return ResidencyFailureObservation{input.handle,description.count,description.flags,resident.count,
                                       status.ntstatus,status.value,resident.bytesToTrim};
}
}
