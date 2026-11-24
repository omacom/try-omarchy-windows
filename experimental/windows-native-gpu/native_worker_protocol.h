// SPDX-License-Identifier: MIT
// Host-private coordinator protocol. NT HANDLE values never cross guest wire.
#pragma once
#include <cstdint>
#include <type_traits>
namespace driver_worker {
constexpr std::uint32_t Magic=0x57575047; // GPWW
constexpr std::uint16_t Version=1;
enum class Operation : std::uint16_t { Initialize=1,Consume,Stop };
struct Request {
    std::uint32_t magic=Magic;
    std::uint16_t version=Version;
    Operation operation=Operation::Initialize;
    std::uint64_t sequence=0,ownedNtHandle=0;
    std::uint32_t luidLow=0;
    std::int32_t luidHigh=0;
    std::uint32_t flags=0,reserved=0;
};
struct Response {
    std::uint32_t magic=Magic;
    std::uint16_t version=Version;
    Operation operation=Operation::Initialize;
    std::uint64_t sequence=0,hash=0;
    std::int32_t hresult=0;
    std::uint32_t workerPid=0,reserved=0,padding=0;
};
static_assert(sizeof(Request)==40 && sizeof(Response)==40 && std::is_trivially_copyable<Request>::value,"Host worker ABI");
constexpr std::uint32_t Present=1,Streaming=2;
inline bool valid(const Request& r,std::uint64_t next,bool initialized) {
    if (r.magic!=Magic || r.version!=Version || r.reserved || !next || r.sequence!=next) return false;
    if (r.operation==Operation::Initialize)
        return !initialized && r.sequence==1 && r.ownedNtHandle && r.ownedNtHandle!=UINT64_MAX && !(r.flags&~Present);
    if (r.ownedNtHandle || r.luidLow || r.luidHigh) return false;
    if (r.operation==Operation::Consume) return initialized && !(r.flags&~Streaming);
    return r.operation==Operation::Stop && initialized && !r.flags;
}
inline bool valid(const Response& r,const Request& request,std::uint32_t pid) {
    return r.magic==Magic && r.version==Version && r.operation==request.operation && r.sequence==request.sequence &&
           pid && r.workerPid==pid && !r.reserved && !r.padding && r.hresult<=0 &&
           (r.hresult==0 || !r.hash) && (r.operation==Operation::Consume || !r.hash);
}
}
