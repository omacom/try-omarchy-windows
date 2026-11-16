// SPDX-License-Identifier: MIT
// Real Unix FD duplication/last-close; kernel object and native GPU APIs mock.
#include "guest_coordinator.h"
#include <condition_variable>
#include <iostream>
#include <thread>
using namespace guest_broker;
using namespace driver_bridge;
#define CHECK(...) do { if(!(__VA_ARGS__)) throw std::runtime_error("guest coordinator control line "+std::to_string(__LINE__)); } while(0)
struct KernelState {
    struct Record { Fd watch;ObjectRef ref;std::uint64_t identity; };
    std::map<std::uint32_t,Record> records;
    bool corrupt=false;
};
struct KernelMock {
    std::shared_ptr<KernelState> state;
    static std::uint64_t identity(int fd) {
        std::uint64_t value=0;socklen_t bytes=sizeof value;
        if(getsockopt(fd,SOL_SOCKET,SO_COOKIE,&value,&bytes) || bytes!=sizeof value) throw std::system_error(EBADF,std::generic_category());
        return value;
    }
    std::pair<Fd,ObjectRef> create(std::uint32_t ticket,std::uint32_t kind) {
        int sockets[2];CHECK(!socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets));
        Fd recipient(sockets[0]);ObjectRef ref{kind,ticket,1000ull+ticket};
        CHECK(state->records.emplace(ticket,KernelState::Record{Fd(sockets[1]),ref,identity(recipient.get())}).second);
        return {std::move(recipient),ref};
    }
    ObjectRef resolve(int fd,std::uint32_t kind) {
        const auto cookie=identity(fd);
        for(const auto& entry:state->records) if(entry.second.identity==cookie) {
            if(kind!=entry.second.ref.kind) throw std::system_error(EINVAL,std::generic_category());
            auto ref=entry.second.ref;if(state->corrupt) ++ref.cookie;return ref;
        }
        throw std::system_error(EBADF,std::generic_category());
    }
    ObjectRef reap() {
        for(auto i=state->records.begin();i!=state->records.end();++i) {
            pollfd event{i->second.watch.get(),POLLIN,0};CHECK(poll(&event,1,0)>=0);
            if(event.revents&POLLHUP) { const auto ref=i->second.ref;state->records.erase(i);return ref; }
        }
        return {};
    }
};
struct NativeMock {
    std::uint64_t id=0;
    std::uint32_t nextAlias=100;
    int aliasRefusal=0;
    bool applicationReferences=false;
    std::atomic<unsigned> aliases{0},closed{0};
    std::atomic<bool> exited{false},finished{false};
    std::condition_variable ready;
    std::mutex mutex;
    std::uint64_t actor() const { return id; }
    cg::Info info() const {
        cg::Info value{};value.capacity=GuestOpenGlMultiappCapacity;value.objects=GuestCoordinatedMultiappObjects;
        value.gl=GuestOpenGlMultiappProfile;value.sync=GuestSyncSharingProfile;
        value.resource=applicationReferences ? GuestApplicationResourceSharingProfile : GuestResourceSharingProfile;
        value.caps={driver_bridge::Version,0,0x10de,0};
        if(applicationReferences) { value.maxActors=8;value.flags=3; }
        CHECK(cg::validInfo(value));return value;
    }
    std::uint32_t alias(std::uint64_t donor,std::uint32_t ticket,std::uint32_t kind) {
        CHECK(donor && donor!=id && ticket>=3 && ticket<=34 && (kind==WDDM_OBJECT_RESOURCE || kind==WDDM_OBJECT_SYNC));
        if(aliasRefusal) { const auto error=aliasRefusal;aliasRefusal=0;throw std::system_error(error,std::generic_category(),"Mock native recipient capacity refusal"); }
        ++aliases;return nextAlias++;
    }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        CHECK(exited);Header h{};std::memcpy(&h,packet.data(),sizeof h);
        CHECK(h.type==static_cast<std::uint32_t>(Op::CloseSharedResource) || h.type==static_cast<std::uint32_t>(Op::CloseSharedSync));
        ++closed;auto reply=encode(h);const auto status=encode(Reply{});reply.insert(reply.end(),status.begin(),status.end());return reply;
    }
    void processExited() { std::lock_guard<std::mutex> guard(mutex);exited=true;ready.notify_all(); }
    void finish() { finished=true; }
};
static std::vector<std::uint8_t> packet(Op op,std::uint32_t handle) { return encode(Header{static_cast<std::uint32_t>(op),handle,0,0}); }
template<class Store> static void exported(Store& store,Op op,std::uint32_t ticket) {
    auto reply=packet(op,ticket);const auto result=encode(Reply{});reply.insert(reply.end(),result.begin(),result.end());store.observe(packet(op,1),reply);
}
int main() {
    {
        auto state=std::make_shared<KernelState>();ObjectRegistry<KernelMock> registry(KernelMock{state});
        NativeMock first,second,third;first.id=1;second.id=2;third.id=3;
        first.applicationReferences=second.applicationReferences=third.applicationReferences=true;
        ActorObjects source(registry,first),recipient(registry,second),other(registry,third);
        std::vector<Fd> descriptors;
        for(unsigned n=0;n<32;++n) {
            exported(source,Op::ExportResource,n+3);
            auto cap=source.create(ObjectRequest{WDDM_OBJECT_RESOURCE,n+3,0},source);
            const auto ref=recipient.resolve(cap.first.get(),WDDM_OBJECT_RESOURCE,recipient);
            CHECK(ref.ticket==100+n && ref.cookie==cap.second.cookie);
            descriptors.push_back(std::move(cap.first));
        }
        exported(other,Op::ExportResource,3);
        auto extra=other.create(ObjectRequest{WDDM_OBJECT_RESOURCE,3,0},other);
        bool refused=false;
        try { recipient.resolve(extra.first.get(),WDDM_OBJECT_RESOURCE,recipient); }
        catch(const std::system_error& error) { refused=error.code().value()==ENOMEM; }
        CHECK(refused && second.aliases==32 && state->records.size()==33 && fcntl(extra.first.get(),F_GETFD)>=0);
        CHECK(recipient.resolve(descriptors.front().get(),WDDM_OBJECT_RESOURCE,recipient).ticket==100 && second.aliases==32);
        descriptors.clear();extra.first=Fd();source.drain(first,source);other.drain(third,other);recipient.drain(second,recipient);
        CHECK(first.closed==32 && third.closed==1 && second.closed==32 && state->records.empty());
        std::cout<<"GUEST_APPLICATION_REFERENCES authenticatedAgreement=true realFd32=true thirtyThirdRefusedBeforeNative=true liveFdPreserved=true existingAliasReusable=true allReferencesReaped=true nativeAndKernel=mock gpu=false\n";
    }
    {
        const Header h{static_cast<std::uint32_t>(Op::MakeVendorResident),42,0,0};
        auto request=encode(h);const auto desc=encode(ResidentDesc{4,1,1,0});
        request.insert(request.end(),desc.begin(),desc.end());request.resize(request.size()+32);
        auto response=encode(h);
        const Reply status{static_cast<std::int32_t>(0xc0000017u),0,123};
        const auto native=encode(status),resident=encode(ResidentReply{0,0,65536});
        response.insert(response.end(),native.begin(),native.end());response.insert(response.end(),resident.begin(),resident.end());
        const auto beforeRequest=request,beforeResponse=response;
        const auto seen=residencyFailureObservation(request,response);
        CHECK(seen && seen->queue==42 && seen->requested==4 && seen->flags==1 && seen->returned==0 &&
              seen->ntstatus==status.ntstatus && seen->pagingFence==123 && seen->bytesToTrim==65536);
        CHECK(request==beforeRequest && response==beforeResponse);
        for(std::size_t length=0;length<response.size();++length) {
            auto bad=response;bad.resize(length);CHECK(!residencyFailureObservation(request,bad));
        }
        auto extra=response;extra.push_back(0);CHECK(!residencyFailureObservation(request,extra));
        for(unsigned test=0;test<8;++test) {
            auto bad=response;Header header=h;Reply reply=status;ResidentReply output{0,0,65536};
            switch(test) {
                case 0: ++header.type;break;
                case 1: ++header.handle;break;
                case 2: header.status=-1;break;
                case 3: header.padding=1;break;
                case 4: reply.reserved=1;break;
                case 5: reply.ntstatus=0;break;
                case 6: output.reserved=1;break;
                case 7: output.count=5;break;
            }
            std::memcpy(bad.data(),&header,sizeof header);std::memcpy(bad.data()+sizeof header,&reply,sizeof reply);
            std::memcpy(bad.data()+sizeof header+sizeof reply,&output,sizeof output);
            CHECK(!residencyFailureObservation(request,bad));
        }
        for(unsigned test=0;test<5;++test) {
            auto bad=request;Header header=h;ResidentDesc descriptor{4,1,1,0};
            switch(test) {
                case 0: header.status=-1;break;
                case 1: header.handle=0;break;
                case 2: descriptor.count=UINT32_MAX;break;
                case 3: descriptor.reserved=1;break;
                case 4: descriptor.priorities=0;break;
            }
            std::memcpy(bad.data(),&header,sizeof header);std::memcpy(bad.data()+sizeof header,&descriptor,sizeof descriptor);
            CHECK(!residencyFailureObservation(bad,response));
        }
    }
    CHECK(nativeAliasFailureErrno(static_cast<std::int32_t>(0xc0000017u))==ENOMEM);
    CHECK(nativeAliasFailureErrno(static_cast<std::int32_t>(0xc000009au))==ENOMEM);
    for(const auto status:{0xc000000du,0xc0000008u,0xc0000022u,0x887a0005u}) {
        const auto error=nativeAliasFailureErrno(static_cast<std::int32_t>(status));
        CHECK(error==EIO && !recoverableObjectError(Operation::ResolveObject,error));
    }
    CHECK(wddm_aperture_valid_caps({1,0,WDDM_APERTURE_CPU_LIMIT,{0,0}}));
    CHECK(wddm_aperture_valid_caps({1,3,WDDM_APERTURE_DESKTOP_CPU_LIMIT,{0,0}}));
    CHECK(wddm_aperture_valid_caps({1,7,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,3,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,7,WDDM_APERTURE_DESKTOP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,4,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(!wddm_aperture_valid_caps({1,15,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,0,256,0}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,0,512,0}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,1,768,0}));
    CHECK(wddm_aperture_valid_coordinator_caps({1,1,1536,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,0,1536,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,1,1537,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({2,1,768,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,2,768,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,1,512,0}));
    CHECK(!wddm_aperture_valid_coordinator_caps({1,1,768,1}));
    {
        CoordinatorPort unavailable("/no-native-coordinator-for-metadata-test");
        ActorPort metadata(unavailable,false,getpid());
        metadata.processExited();metadata.finish();CHECK(!metadata.actor());
    }
    for(const std::uint32_t kind:{WDDM_OBJECT_SYNC,WDDM_OBJECT_RESOURCE}) {
        auto state=std::make_shared<KernelState>();ObjectRegistry<KernelMock> registry(KernelMock{state});
        NativeMock first,second;first.id=1;second.id=2;
        ActorObjects source(registry,first),recipient(registry,second);
        const auto exporting=kind==WDDM_OBJECT_SYNC ? Op::ExportSync : Op::ExportResource;
        const auto importing=kind==WDDM_OBJECT_SYNC ? Op::ImportSync : Op::ImportResource;
        const auto closing=kind==WDDM_OBJECT_SYNC ? Op::CloseSharedSync : Op::CloseSharedResource;
        exported(source,exporting,3);exported(recipient,exporting,3);
        auto original=source.create(ObjectRequest{kind,3,0},source);
        auto own=recipient.create(ObjectRequest{kind,3,0},recipient);
        CHECK(state->records.size()==2 && original.second.ticket==3 && own.second.ticket==3 && original.second.cookie!=own.second.cookie);
        Fd inherited(fcntl(original.first.get(),F_DUPFD_CLOEXEC,0));CHECK(inherited.get()>=0);
        original.first=Fd();
        bool bad=false;state->corrupt=true;
        try { (void)recipient.resolve(inherited.get(),kind,recipient); } catch(const std::system_error&) { bad=true; }
        CHECK(bad && !second.aliases);state->corrupt=false;
        std::exception_ptr failed;
        std::thread producer([&] { try { source.drain(first,source); } catch(...) { failed=std::current_exception(); } });
        {
            std::unique_lock<std::mutex> guard(first.mutex);CHECK(first.ready.wait_for(guard,std::chrono::seconds(2),[&]{return first.exited.load();}));
        }
        CHECK(!first.closed && !first.finished); // Real recipient duplicate keeps source capability alive.
        recipient.validate(inherited.get(),kind,recipient);CHECK(!second.aliases);
        for(const auto code:{ENOMEM,ENOSPC}) {
            second.aliasRefusal=code;bad=false;
            try { (void)recipient.resolve(inherited.get(),kind,recipient); }
            catch(const std::system_error& error) { bad=error.code().value()==code; }
            CHECK(bad && recoverableObjectError(Operation::ResolveObject,code) && !second.aliases && !second.closed &&
                  !first.closed && !first.finished && state->records.size()==2 && fcntl(inherited.get(),F_GETFD)>=0);
        }
        const auto foreign=recipient.resolve(inherited.get(),kind,recipient);
        CHECK(foreign.ticket==100 && foreign.cookie==original.second.cookie && second.aliases==1);
        CHECK(recipient.resolve(inherited.get(),kind,recipient).ticket==100 && second.aliases==1);
        std::vector<Fd> auth;auth.emplace_back(fcntl(inherited.get(),F_DUPFD_CLOEXEC,0));
        recipient.authorize(packet(importing,foreign.ticket),auth,recipient);
        bad=false;try { recipient.authorize(packet(importing,3),auth,recipient); } catch(const std::system_error&) { bad=true; }CHECK(bad);
        bad=false;try { recipient.authorize(packet(closing,foreign.ticket),{},recipient); } catch(const std::system_error&) { bad=true; }CHECK(bad);
        auth.clear();inherited=Fd();producer.join();if(failed) std::rethrow_exception(failed);
        CHECK(first.closed==1 && first.finished && !second.closed); // Recipient alias owns a separate native reference.
        own.first=Fd();recipient.drain(second,recipient);
        CHECK(second.closed==2 && second.finished && state->records.empty());
    }
    std::cout<<"GUEST_COORDINATOR realUnixFdLifetime=true collidingLocalTickets=true kernelGlobalTicketsUnique=true forgedCookieRejected=true foreignTicketRejected=true refusedAliasLeavesNoCache=true capacityRetry=true aliasCached=true producerExitRetained=true nativeAndKernel=mock gpu=false\n";
}
