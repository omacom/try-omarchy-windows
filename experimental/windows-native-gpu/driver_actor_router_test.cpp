// SPDX-License-Identifier: MIT
// Actual Session dispatch with a mock Driver; no GPU/coordinator claim.
#include "driver_actor_router.h"
#include <iostream>
#include <set>
#include <string>
#include <type_traits>
using namespace driver_bridge;
#define CHECK(...) do { if(!(__VA_ARGS__)) throw std::runtime_error("actor router control line "+std::to_string(__LINE__)); } while(0)
struct NativeMock : Driver {
    unsigned calls=0,destroyed=0;std::uint32_t next=100;std::set<std::uint32_t> live;std::uint32_t queried=0;
    bool vmStopped=false,requireVmStop=false;
    native_gpu::Capabilities capabilities() const override { return {1,0,0x10de,0x1234}; }
    Result openAdapter() override { ++calls;live.insert(++next);return {0,next,0}; }
    Result queryVersion(std::uint32_t adapter) override { ++calls;CHECK(live.count(adapter));queried=adapter;return {0,0,31}; }
    Result createDevice(std::uint32_t adapter) override { CHECK(live.count(adapter));return openAdapter(); }
    Result queryAdapter(std::uint32_t,QueryDesc,std::vector<std::uint8_t>&) override { return {-95,0,0}; }
    Result createContext(std::uint32_t,ContextDesc,std::vector<std::uint8_t>&) override { return {-95,0,0}; }
    Result createPagingQueue(std::uint32_t) override { return {-95,0,0}; }
    Result readPagingFence(std::uint32_t) override { return {-95,0,0}; }
    Result createAllocation(std::uint32_t,std::uint32_t) override { return {-95,0,0}; }
    Result createSharedAllocation(std::uint32_t,GuestRange) override { return {-95,0,0}; }
    Result copySharedAllocation(std::uint32_t,std::uint32_t,CopyRange) override { return {-95,0,0}; }
    Result writeAllocation(std::uint32_t,Range,const std::uint8_t*) override { return {-95,0,0}; }
    Result readAllocation(std::uint32_t,Range,std::vector<std::uint8_t>&) override { return {-95,0,0}; }
    Result makeResident(std::uint32_t,std::uint32_t) override { return {-95,0,0}; }
    Result mapAllocation(std::uint32_t,std::uint32_t) override { return {-95,0,0}; }
    Result queryResidency(std::uint32_t) override { return {-95,0,0}; }
    Result destroy(Kind,std::uint32_t id) override { CHECK(!requireVmStop || vmStopped);CHECK(live.erase(id)==1);++destroyed;return {0,0,0}; }
};
static std::vector<std::uint8_t> inner(Op op,std::uint32_t handle=0) {
    Header h{static_cast<unsigned>(op),handle,0,0};std::vector<std::uint8_t> out(sizeof h);std::memcpy(out.data(),&h,sizeof h);return out;
}
static std::vector<std::uint8_t> envelope(std::uint64_t actor,std::uint64_t sequence,const std::vector<std::uint8_t>& payload={},actors::Operation op=actors::Operation::Call) {
    actors::Envelope h{};h.actor=actor;h.sequence=sequence;h.operation=op;h.bytes=static_cast<std::uint32_t>(payload.size());
    std::vector<std::uint8_t> out(sizeof h+payload.size());std::memcpy(out.data(),&h,sizeof h);if(!payload.empty()) std::memcpy(out.data()+sizeof h,payload.data(),payload.size());return out;
}
template<class T> static T at(const std::vector<std::uint8_t>& value,std::size_t offset=0) { CHECK(value.size()>=offset+sizeof(T));T out{};std::memcpy(&out,value.data()+offset,sizeof out);return out; }
int main() {
    static_assert(!std::is_copy_constructible_v<actors::Router> && !std::is_move_constructible_v<actors::Router>,"Actor identities cannot be duplicated");
    NativeMock native;
    {
        Session first(native),second(native),third(native),fourth(native),fifth(native);
        {
        actors::Router router;
        const auto a=router.attach(first),b=router.attach(second);CHECK(a==1 && b==2 && !router.attach(first));
        CHECK(!router.configureActorLimit(8));
        std::uint64_t sequence=1;
        auto call=[&](std::uint64_t actor,const std::vector<std::uint8_t>& body) { return router.dispatch(envelope(actor,sequence++,body)); };
        CHECK(!at<actors::Envelope>(call(a,hello())).status && !at<actors::Envelope>(call(b,hello())).status);
        CHECK(at<Header>(call(a,inner(Op::OpenAdapter)),40).handle==1);
        CHECK(at<Header>(call(b,inner(Op::OpenAdapter)),40).handle==1 && native.live.size()==2);
        call(a,inner(Op::QueryDriverVersion,1));CHECK(native.queried==101);
        call(b,inner(Op::QueryDriverVersion,1));CHECK(native.queried==102);
        CHECK(at<Header>(call(a,inner(Op::CreateDevice,1)),40).handle==2);
        const auto before=native.calls;
        CHECK(at<Header>(call(b,inner(Op::CreateDevice,2)),40).status<0 && native.calls==before);
        CHECK(at<actors::Envelope>(router.dispatch(envelope(a,sequence++,{},actors::Operation::Close))).status==-16);
        CHECK(!at<actors::Envelope>(router.dispatch(envelope(a,sequence++,{},actors::Operation::Disconnect))).status);
        CHECK(router.disconnected(a) && first.objectCount()==2 && native.live.size()==3 && !native.destroyed);
        CHECK(at<actors::Envelope>(call(a,inner(Op::QueryDriverVersion,1))).status==-32 && native.calls==before);
        CHECK(!at<Header>(call(b,inner(Op::QueryDriverVersion,1)),40).status && native.queried==102);
        CHECK(router.attach(third)==3 && router.attach(fourth)==4 && !router.attach(fifth));
        CHECK(!at<actors::Envelope>(router.dispatch(envelope(3,sequence++,{},actors::Operation::Close))).status);
        CHECK(router.attach(fifth)==5); // Retired actor IDs are never reused.
        CHECK(!router.configureActorLimit(8));
        CHECK(at<actors::Envelope>(call(3,hello())).status==-9);
        // Router destruction leaves both actual Sessions untouched. Their
        // owner must reap the VM before allowing nonempty Session destruction.
        native.requireVmStop=true;
        }
        CHECK(native.live.size()==3 && !native.destroyed);
        native.vmStopped=true; // Explicit mock owner proof, not real VM/GPU work.
    }
    CHECK(native.live.empty() && native.destroyed==3);
    {
        Session first(native),second(native);actors::Router router;const auto a=router.attach(first),b=router.attach(second);
        std::uint64_t sequence=1;
        for(unsigned n=0;n<10000;++n) CHECK(!at<actors::Envelope>(router.dispatch(envelope(a,sequence++,hello()))).status);
        CHECK(at<actors::Envelope>(router.dispatch(envelope(a,sequence++,hello()))).status==-24);
        CHECK(!at<actors::Envelope>(router.dispatch(envelope(b,sequence++,hello()))).status);
        auto maximum=inner(static_cast<Op>(0xdead));maximum.resize(native_gpu::MaxPacket);
        const auto result=router.dispatch(envelope(b,sequence++,maximum));
        CHECK(!at<actors::Envelope>(result).status && at<Header>(result,40).status<0 && !router.failed());
    }
    for(unsigned mutation=0;mutation<13;++mutation) {
        Session session(native);actors::Router router;const auto id=router.attach(session);
        auto packet=envelope(id,1,hello());auto h=at<actors::Envelope>(packet);
        if(mutation==0) ++h.magic;
        if(mutation==1) ++h.version;
        if(mutation==2) h.operation=static_cast<actors::Operation>(99);
        if(mutation==3) h.actor=0;
        if(mutation==4) h.sequence=0;
        if(mutation==5) h.sequence=2;
        if(mutation==6) ++h.bytes;
        if(mutation==7) h.reserved=1;
        if(mutation==8) h.status=-1;
        if(mutation==9) h.padding=1;
        if(mutation==10) h.operation=actors::Operation::Disconnect;
        std::memcpy(packet.data(),&h,sizeof h);if(mutation==11) packet.resize(39);if(mutation==12) packet.resize(actors::MaxEnvelopeBytes+1);
        const auto before=native.calls;router.dispatch(packet);CHECK(router.failed() && native.calls==before);
        CHECK(router.dispatch(envelope(id,1,hello())).empty() && !router.attach(session));
    }
    std::cout<<"ACTOR_ROUTER fourActorsBounded=true collidingWireIdsIsolated=true foreignParentsRejected=true disconnectRetainsObjects=true idsNeverReused=true malformedPoisoned=true native=mock gpu=false runtimeIntegration=false\n";
}
