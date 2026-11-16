// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include <iostream>
#include <set>
#include <string>
using namespace driver_bridge;
static unsigned checks = 0;
static void requireAt(bool good, int line) { ++checks; if (!good) throw std::runtime_error("Shared sync wire regression at line " + std::to_string(line)); }
#define require(good) requireAt((good), __LINE__)
static Header header(const std::vector<std::uint8_t>& data) {
    require(data.size() >= sizeof(Header)); Header h{}; memcpy(&h, data.data(), sizeof h); return h;
}
struct Fake : Driver {
    std::uint32_t next = 1000;
    unsigned exports = 0, imports = 0, calls = 0;
    bool supported = true, fail = false;
    int badExport = 0, badImport = 0;
    unsigned infoQueries = 0;
    Result infoReply{static_cast<std::int32_t>(0xc0000024u),0,0};
    std::uint32_t capabilityFlags = 7 | SyncCapability;
    unsigned waits = 0, queuedSignals = 0;
    Result waitReply{0,0,0}, signalReply{0,0,0};
    std::vector<HwQueueWaitItem> waited;
    std::set<std::uint32_t> borrowed;
    std::map<std::uint32_t, Kind> live;
    Result create(Kind kind) { ++calls; const auto id = ++next; live.emplace(id, kind); return {0, id, 0}; }
    native_gpu::Capabilities capabilities() const override { return {Version, capabilityFlags, 0x10de, 1}; }
    SyncSharingProfile beginSyncSharing() override { ++calls; return supported ? GuestSyncSharingProfile : SyncSharingProfile{}; }
    OpenGlProfile beginOpenGl(std::uint32_t version) override { return version==3 ? GuestOpenGlCompositorProfile : GuestOpenGlFenceProfile; }
    Result openAdapter() override { return create(Kind::Adapter); }
    Result queryVersion(std::uint32_t) override { return {0, 0, 31}; }
    Result queryAdapter(std::uint32_t, QueryDesc, std::vector<std::uint8_t>&) override { return {-1, 0, 0}; }
    Result createDevice(std::uint32_t) override { return create(Kind::Device); }
    Result createContext(std::uint32_t device, ContextDesc d, std::vector<std::uint8_t>& bytes) override {
        require(live.at(device) == Kind::Device && validContext(d) && bytes.size() == d.privateBytes);
        return create(Kind::Context);
    }
    HwQueueResult createHwQueue(std::uint32_t context, HwQueueDesc d, std::vector<std::uint8_t>& bytes) override {
        require(live.at(context) == Kind::Context && validHwQueue(d) && bytes.size() == d.privateBytes);
        const auto queue = create(Kind::HwQueue); const auto sync = ++next; borrowed.insert(sync);
        return {queue, sync, 8192, 65536};
    }
    Result queueHwQueueWait(std::uint32_t queue, const std::vector<HwQueueWaitItem>& items) override {
        require(live.at(queue) == Kind::HwQueue);
        for (const auto& item : items) require(live.at(item.sync) == Kind::Sync);
        ++waits; waited = items; return waitReply;
    }
    Result queueContextWait(std::uint32_t context, const std::vector<HwQueueWaitItem>& items) override {
        require(live.at(context) == Kind::Context);
        for (const auto& item : items) require(live.at(item.sync) == Kind::Sync);
        ++waits; waited = items; return waitReply;
    }
    Result queueContextSync(std::uint32_t context, std::uint32_t sync, ContextSignalDesc) override {
        require(live.at(context) == Kind::Context && live.at(sync) == Kind::Sync);
        ++queuedSignals; return signalReply;
    }
    SyncResult createSync(std::uint32_t, SyncDesc d) override {
        auto r = create(Kind::Sync); return d.type == 1 ? SyncResult{r,{0,0}} : SyncResult{r,{4096,d.flags & 128 ? 0ull : 65536ull}};
    }
    Result exportSync(std::uint32_t id) override {
        require(live.at(id) == Kind::Sync); ++exports;
        if (fail) return {-123, 0, 0};
        auto r = create(Kind::SharedSync);
        if (badExport == 1) r.ntstatus = 259;
        if (badExport == 2) r.value = 1;
        return r;
    }
    SyncResult importSync(std::uint32_t id, SharedSyncOpenDesc d) override {
        require(live.at(id) == Kind::SharedSync && live.at(d.device) == Kind::Device); ++imports;
        if (fail) return {{-123, 0, 0}, {0, 0}};
        auto r = create(Kind::Sync); SyncReply output{8192, d.flags & 128 ? 0ull : 131072ull};
        if (badImport == 1) r.ntstatus = 259;
        if (badImport == 2) r.value = 1;
        if (badImport == 3) output.offset = FenceApertureBytes;
        if (badImport == 4) output.offset = 1;
        if (badImport == 5) output.gpuAddress = 0;
        if (badImport == 6) output.gpuAddress = MaxGpuAddress;
        return {r, output};
    }
    Result querySharedSyncInfo(std::uint32_t id, std::uint32_t device) override {
        require(live.at(id) == Kind::SharedSync && live.at(device) == Kind::Device);
        ++infoQueries; return infoReply;
    }
    Result createPagingQueue(std::uint32_t) override { return {-1, 0, 0}; }
    Result readPagingFence(std::uint32_t) override { return {-1, 0, 0}; }
    Result createAllocation(std::uint32_t, std::uint32_t) override { return {-1, 0, 0}; }
    Result createSharedAllocation(std::uint32_t, GuestRange) override { return {-1, 0, 0}; }
    Result copySharedAllocation(std::uint32_t, std::uint32_t, CopyRange) override { return {-1, 0, 0}; }
    Result writeAllocation(std::uint32_t, Range, const std::uint8_t*) override { return {-1, 0, 0}; }
    Result readAllocation(std::uint32_t, Range, std::vector<std::uint8_t>&) override { return {-1, 0, 0}; }
    Result makeResident(std::uint32_t, std::uint32_t) override { return {-1, 0, 0}; }
    Result mapAllocation(std::uint32_t, std::uint32_t) override { return {-1, 0, 0}; }
    Result queryResidency(std::uint32_t) override { return {-1, 0, 0}; }
    Result destroy(Kind kind, std::uint32_t id) override {
        if (kind == Kind::HwQueueSync) { require(borrowed.erase(id) == 1); return {0,0,0}; }
        require(live.at(id) == kind); live.erase(id); return {0, 0, 0};
    }
};
static void negotiate(Session& s) { require(header(s.dispatch(request(Op::Hello, 0, Version))).status == 0); }
static void begin(Session& s) {
    const auto out = s.dispatch(request(Op::BeginSyncSharing, 0));
    require(header(out).status == 0 && out.size() == sizeof(Header) + sizeof(SyncSharingProfile));
    SyncSharingProfile p{}; memcpy(&p, out.data() + sizeof(Header), sizeof p); require(validSyncSharingProfile(p));
}
static std::uint32_t add(Session& s, Op op, std::uint32_t parent) {
    const auto h = header(s.dispatch(request(op, parent))); require(h.status == 0 && h.handle); return h.handle;
}
static std::uint32_t sync(Session& s, std::uint32_t device, std::uint32_t flags = 3) {
    const auto h = header(s.dispatch(request(Op::CreateSync, device, SyncDesc{5, flags, 1, 0, 0})));
    require(h.status == 0 && h.handle); return h.handle;
}
static std::vector<std::uint8_t> waitPacket(std::uint32_t queue, std::vector<HwQueueWaitItem> items) {
    auto packet = request(Op::QueueHwQueueWait, queue, HwQueueWaitDesc{static_cast<std::uint32_t>(items.size()),0});
    const auto bytes = reinterpret_cast<const std::uint8_t*>(items.data());
    if (!items.empty()) packet.insert(packet.end(), bytes, bytes + items.size()*sizeof(items[0]));
    return packet;
}
static void queueDependencies() {
    Fake fake;
    fake.capabilityFlags |= ContextCapability | HwQueueCapability | AsyncHwSubmitCapability | ContextSignalCapability;
    {
        Session s(fake);
        require(header(s.dispatch(waitPacket(1,{{1,0,5}}))).status == -71);
        negotiate(s);
        require(header(s.dispatch(waitPacket(1,{{1,0,5}}))).status == -71);
        require(header(s.dispatch(request(Op::QueueContextSync,1,ContextSignalDesc{1,4,5}))).status == -71);
        begin(s);
        const auto adapter = add(s,Op::OpenAdapter,0), device = add(s,Op::CreateDevice,adapter);
        const auto otherDevice = add(s,Op::CreateDevice,adapter);
        auto createContext = request(Op::CreateContext,device,ContextDesc{0,1,16,12,1,0}); createContext.push_back(37);
        const auto context = header(s.dispatch(createContext)).handle; require(context);
        auto createQueue = request(Op::CreateHwQueue,context,HwQueueDesc{0,1,0,0}); createQueue.push_back(37);
        const auto queue = header(s.dispatch(createQueue)).handle; require(queue);
        const auto first = sync(s,device,SharedSyncImportFlags), second = sync(s,device,NoGpuAccessSyncFlag);
        const auto foreign = sync(s,otherDevice,NoGpuAccessSyncFlag);
        const auto mutex = header(s.dispatch(request(Op::CreateSync,device,SyncDesc{1,0,0,0,0}))).handle; require(mutex);
        auto valid = waitPacket(queue,{{first,0,5},{second,0,9}});
        for (const auto missing : {HwQueueCapability,AsyncHwSubmitCapability,SyncCapability}) {
            fake.capabilityFlags ^= missing;
            require(header(s.dispatch(valid)).status == -95); fake.capabilityFlags ^= missing;
        }
        require(header(s.dispatch(request(Op::QueueHwQueueWait,queue))).status == -22);
        for (auto packet : {waitPacket(queue,{}),waitPacket(queue,std::vector<HwQueueWaitItem>(MaxHwQueueWaitObjects+1,{first,0,5})),
                            waitPacket(queue,{{first,1,5}}),waitPacket(queue,{{first,0,UINT64_MAX}}),
                            waitPacket(queue,{{first,0,5},{first,0,6}})}) require(header(s.dispatch(packet)).status == -22);
        auto bad = valid; bad.push_back(0); require(header(s.dispatch(bad)).status == -22);
        bad = valid; bad.pop_back(); require(header(s.dispatch(bad)).status == -22);
        bad = valid; bad[20]=1; require(header(s.dispatch(bad)).status == -22);
        require(header(s.dispatch(waitPacket(device,{{first,0,5}}))).status == -9);
        for (auto id : {foreign,mutex,device,UINT32_MAX}) require(header(s.dispatch(waitPacket(queue,{{id,0,5}}))).status == -9);
        require(!fake.waits);
        for (auto result : {Result{259,0,0},Result{0,1,0},Result{0,0,1},Result{-123,0,1}}) {
            fake.waitReply=result; require(header(s.dispatch(valid)).status == -5);
        }
        fake.waitReply={-123,0,0}; auto out=s.dispatch(valid); Reply reply{}; memcpy(&reply,out.data()+sizeof(Header),sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123 && !reply.value);
        fake.waitReply={0,0,0}; out=s.dispatch(valid);
        require(header(out).status == 0 && header(out).handle == queue && fake.waited.size() == 2);
        require(fake.waited[0].sync > 1000 && fake.waited[0].sync != first && fake.waited[0].fence == 5 && fake.waited[1].fence == 9);
        require(header(s.dispatch(waitPacket(queue,{{first,0,0}}))).status == 0);
        auto contextWait = valid;
        Header contextHeader{static_cast<std::uint32_t>(Op::QueueContextWait),context,0,0};
        memcpy(contextWait.data(),&contextHeader,sizeof contextHeader);
        for (const auto missing : {ContextCapability,AsyncHwSubmitCapability,SyncCapability}) {
            fake.capabilityFlags ^= missing;
            require(header(s.dispatch(contextWait)).status == -95); fake.capabilityFlags ^= missing;
        }
        contextHeader.handle=queue; memcpy(contextWait.data(),&contextHeader,sizeof contextHeader);
        require(header(s.dispatch(contextWait)).status == -9);
        contextHeader.handle=context; memcpy(contextWait.data(),&contextHeader,sizeof contextHeader);
        for (auto result : {Result{259,0,0},Result{0,1,0},Result{0,0,1},Result{-123,0,1}}) {
            fake.waitReply=result; require(header(s.dispatch(contextWait)).status == -5);
        }
        fake.waitReply={-123,0,0}; out=s.dispatch(contextWait); memcpy(&reply,out.data()+sizeof(Header),sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123);
        fake.waitReply={0,0,0}; require(header(s.dispatch(contextWait)).status == 0);
        // Queued signals may return before their target. Invalid replies must
        // not advance the session's per-fence monotonic signal tracking.
        auto signal=request(Op::QueueContextSync,context,ContextSignalDesc{first,4,5});
        fake.capabilityFlags ^= AsyncHwSubmitCapability;
        require(header(s.dispatch(signal)).status == -95); fake.capabilityFlags ^= AsyncHwSubmitCapability;
        fake.capabilityFlags ^= ContextSignalCapability;
        require(header(s.dispatch(signal)).status == -95); fake.capabilityFlags ^= ContextSignalCapability;
        require(header(s.dispatch(request(Op::QueueContextSync,queue,ContextSignalDesc{first,4,5}))).status == -9);
        require(header(s.dispatch(request(Op::QueueContextSync,context,ContextSignalDesc{foreign,4,5}))).status == -9);
        for (auto result : {Result{259,0,0},Result{0,1,0},Result{0,0,UINT64_MAX},Result{-123,0,1}}) {
            fake.signalReply=result; require(header(s.dispatch(signal)).status == -5);
        }
        fake.signalReply={-123,0,0}; out=s.dispatch(signal); memcpy(&reply,out.data()+sizeof(Header),sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123);
        fake.signalReply={0,0,0}; require(header(s.dispatch(signal)).status == 0);
        const auto calls=fake.queuedSignals;
        require(header(s.dispatch(signal)).status == -22 && fake.queuedSignals == calls);
        require(header(s.dispatch(request(Op::QueueContextSync,context,ContextSignalDesc{first,4,6}))).status == 0);
    }
    require(fake.live.empty() && fake.borrowed.empty());
}
static void synchronizationOnlySignals() {
    struct SignalFake : Fake {
        Result signalContextSync(std::uint32_t context,std::uint32_t object,ContextSignalDesc desc) override {
            require(live.at(context)==Kind::Context && live.at(object)==Kind::Sync && validContextSignal(desc));
            ++calls; return {0,0,desc.fence};
        }
    } fake;
    fake.capabilityFlags|=ContextCapability|AsyncHwSubmitCapability|ContextSignalCapability|HwQueueCapability;
    {
        Session session(fake); session.dispatch(hello());
        require(!header(session.dispatch(request(Op::BeginSyncSharing))).status);
        const auto adapter=add(session,Op::OpenAdapter,0),device=add(session,Op::CreateDevice,adapter);
        const auto other=add(session,Op::CreateDevice,adapter);
        const auto context=header(session.dispatch(request(Op::CreateContext,device,ContextDesc{0,0,8,0,0,0}))).handle;
        const auto fence=sync(session,device,128),foreign=sync(session,other,128);
        require(context && fence && foreign);
        require(!header(session.dispatch(request(Op::SignalContextSync,context,ContextSignalDesc{fence,4,1}))).status);
        require(!header(session.dispatch(request(Op::QueueContextSync,context,ContextSignalDesc{fence,4,2}))).status);
        const auto before=fake.calls;
        require(header(session.dispatch(request(Op::QueueContextSync,context,ContextSignalDesc{fence,4,2}))).status==-22);
        require(header(session.dispatch(request(Op::QueueContextSync,context,ContextSignalDesc{foreign,4,3}))).status==-9);
        auto queue=request(Op::CreateHwQueue,context,HwQueueDesc{0,1,0,0}); queue.push_back(1);
        require(header(session.dispatch(queue)).status==-95 && fake.calls==before);
    }
    require(fake.live.empty() && fake.borrowed.empty());
    std::cout<<"SYNC_ONLY_CONTEXT nativeSignalsAuthorized=true foreignDeviceRejected=true duplicateRejected=true commandQueueDenied=true gpu=false\n";
}

static void actorSyncAliases() {
    struct AliasNative : Fake {
        bool equivalent=true;unsigned duplicates=0;
        bool samePhysicalAdapter(std::uint32_t a,std::uint32_t b) const override { return equivalent && live.count(a) && live.count(b); }
        Result duplicateSharedReference(Kind kind,std::uint32_t source) override {
            require(kind==Kind::SharedSync && live.at(source)==Kind::SharedSync);++duplicates;return create(kind);
        }
    } native;
    Session recipient(native);negotiate(recipient);begin(recipient);
    const auto adapter=add(recipient,Op::OpenAdapter,0),device=add(recipient,Op::CreateDevice,adapter);
    std::uint32_t alias=0;
    {
        Session donor(native);negotiate(donor);begin(donor);
        const auto a=add(donor,Op::OpenAdapter,0),d=add(donor,Op::CreateDevice,a);
        const auto original=sync(donor,d),ticket=add(donor,Op::ExportSync,original);
        require(recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter).ntstatus<0);
        native.equivalent=false;require(recipient.aliasSharedReference(donor,Kind::SharedSync,ticket,adapter).ntstatus<0);
        native.equivalent=true;require(!native.duplicates);
        const auto copied=recipient.aliasSharedReference(donor,Kind::SharedSync,ticket,adapter);require(!copied.ntstatus && copied.ticket);alias=copied.ticket;
    }
    const auto opened=recipient.dispatch(request(Op::ImportSync,alias,SharedSyncOpenDesc{device,SharedSyncImportFlags,1,0}));
    require(!header(opened).status && header(opened).handle);
    require(!header(recipient.dispatch(request(Op::CloseSharedSync,alias))).status);
    std::cout<<"ACTOR_SYNC_ALIAS physicalAdapterChecked=true independentNativeReference=true donorSessionDestroyed=true recipientImport=true native=mock gpu=false\n";
}

int main() {
    actorSyncAliases();
    try {
        for(const auto version:{2u,3u}) {
            Fake quota;
            quota.capabilityFlags |= AsyncHwSubmitCapability|GpuReservationCapability|UINT32_C(0x80000000);
            {
                Session s(quota); negotiate(s);
                require(!header(s.dispatch(request(Op::BeginOpenGl,0,version))).status);
                const auto adapter=add(s,Op::OpenAdapter,0), device=add(s,Op::CreateDevice,adapter);
                begin(s); const auto source=sync(s,device);
                const auto ticket=add(s,Op::ExportSync,source);
                const auto open=request(Op::ImportSync,ticket,SharedSyncOpenDesc{device,0,1,0});
                require(s.syncObjectLimit()==(version==3?192u:96u));
                std::uint32_t last=0;
                for(std::size_t n=1;n<s.syncObjectLimit();++n) { last=header(s.dispatch(open)).handle; require(last); }
                const auto before=quota.imports;
                require(header(s.dispatch(open)).status==-24 && quota.imports==before);
                require(!header(s.dispatch(request(Op::DestroySync,last))).status);
                require(header(s.dispatch(open)).handle && quota.imports==before+1);
            }
            require(quota.live.empty());
        }
        Fake fake;
        {
            Session s(fake);
            require(header(s.dispatch(request(Op::BeginSyncSharing, 0))).status == -71);
            negotiate(s);
            fake.supported = false; require(header(s.dispatch(request(Op::BeginSyncSharing, 0))).status == -95);
            fake.supported = true;
            auto bad = request(Op::BeginSyncSharing, 0); bad.push_back(0);
            require(header(s.dispatch(bad)).status == -22);
            require(header(s.dispatch(request(Op::BeginSyncSharing, 1))).status == -22);
            const auto adapter = add(s, Op::OpenAdapter, 0), sourceDevice = add(s, Op::CreateDevice, adapter);
            const auto device = add(s, Op::CreateDevice, adapter);
            require(header(s.dispatch(request(Op::CreateSync, device, SyncDesc{5, 3, 1, 0, 0}))).status == -71);
            begin(s); require(header(s.dispatch(request(Op::BeginSyncSharing, 0))).status == -16);
            const auto source = sync(s, sourceDevice), notShared = sync(s, device, 0);
            require(header(s.dispatch(request(Op::ExportSync, notShared))).status == -22);
            require(header(s.dispatch(request(Op::ExportSync, device))).status == -9);
            for (int n = 1; n <= 2; ++n) {
                fake.badExport = n;
                const auto before = fake.live.size();
                require(header(s.dispatch(request(Op::ExportSync, source))).status == -5 && fake.live.size() == before);
            }
            fake.badExport = 0; fake.fail = true;
            auto failed = s.dispatch(request(Op::ExportSync, source));
            require(header(failed).status == 0 && !header(failed).handle);
            Reply result{}; memcpy(&result, failed.data() + sizeof(Header), sizeof result); require(result.ntstatus == -123);
            fake.fail = false;
            const auto ticket = add(s, Op::ExportSync, source);
            require(header(s.dispatch(request(Op::DestroySync, source))).status == 0);
            require(header(s.dispatch(request(Op::DestroyDevice, sourceDevice))).status == 0);
            require(header(s.dispatch(request(Op::CloseAdapter, adapter))).status == -16);
            const auto otherAdapter = add(s, Op::OpenAdapter, 0), otherDevice = add(s, Op::CreateDevice, otherAdapter);
            require(header(s.dispatch(request(Op::QuerySharedSyncInfo,ticket))).status == -22);
            require(header(s.dispatch(request(Op::QuerySharedSyncInfo,ticket,SharedSyncInfoDesc{device,1}))).status == -22);
            require(header(s.dispatch(request(Op::QuerySharedSyncInfo,adapter,SharedSyncInfoDesc{device,0}))).status == -9);
            require(header(s.dispatch(request(Op::QuerySharedSyncInfo,ticket,SharedSyncInfoDesc{otherDevice,0}))).status == -9);
            require(!fake.infoQueries);
            for (auto badReply : {Result{0,0,0},Result{259,0,0},Result{-123,1,0},Result{-123,0,1}}) {
                fake.infoReply=badReply;
                require(header(s.dispatch(request(Op::QuerySharedSyncInfo,ticket,SharedSyncInfoDesc{device,0}))).status == -5);
            }
            fake.infoReply={static_cast<std::int32_t>(0xc0000024u),0,0};
            const auto info=s.dispatch(request(Op::QuerySharedSyncInfo,ticket,SharedSyncInfoDesc{device,0}));
            require(header(info).status == 0 && header(info).handle == ticket && info.size() == sizeof(Header)+sizeof(Reply));
            Reply infoOutput{}; memcpy(&infoOutput,info.data()+sizeof(Header),sizeof infoOutput);
            require(infoOutput.ntstatus == fake.infoReply.ntstatus && !infoOutput.value && fake.infoQueries == 5);
            const auto beforeCalls = fake.imports;
            for (auto desc : {SharedSyncOpenDesc{0,0,1,0}, SharedSyncOpenDesc{device,1,1,0},
                              SharedSyncOpenDesc{device,0,2,0}, SharedSyncOpenDesc{device,0,1,1}})
                require(header(s.dispatch(request(Op::ImportSync, ticket, desc))).status == -22);
            require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{otherDevice,0,1,0}))).status == -9);
            require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{adapter,0,1,0}))).status == -9);
            require(fake.imports == beforeCalls);
            for (int n = 1; n <= 6; ++n) {
                fake.badImport = n; const auto before = fake.live.size();
                require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{device,0,1,0}))).status == -5 && fake.live.size() == before);
            }
            fake.badImport = 0;
            const auto imported = header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{device,0,1,0}))).handle;
            require(imported);
            const auto reexported = add(s, Op::ExportSync, imported);
            require(header(s.dispatch(request(Op::CloseSharedSync, ticket))).status == 0);
            require(header(s.dispatch(request(Op::ImportSync, ticket, SharedSyncOpenDesc{device,0,1,0}))).status == -9);
            require(header(s.dispatch(request(Op::CloseSharedSync, ticket))).status == -9);
            require(header(s.dispatch(request(Op::DestroySync, imported))).status == 0);
            require(header(s.dispatch(request(Op::CloseSharedSync, reexported))).status == 0);
            const auto remaining = sync(s, device);
            std::vector<std::uint32_t> tickets;
            for (std::size_t n = 0; n < MaxSharedSyncHandles; ++n) tickets.push_back(add(s, Op::ExportSync, remaining));
            const auto exports = fake.exports;
            require(header(s.dispatch(request(Op::ExportSync, remaining))).status == -24 && fake.exports == exports);
            // Disconnect must close every remaining NT reference and KMT object.
        }
        require(fake.live.empty());
        queueDependencies(); synchronizationOnlySignals();
        std::cout << "PASS: " << checks << " shared-fence protocol checks, scoped tickets, cross-device ownership, quotas, malformed replies and cleanup\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
