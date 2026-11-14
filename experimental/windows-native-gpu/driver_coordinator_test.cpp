// SPDX-License-Identifier: MIT
#include "driver_coordinator.h"
#include <iostream>
#include <set>
#include <sstream>
#include <string>
using namespace driver_bridge;
namespace cg=driver_bridge::coordinator;
#define CHECK(...) do { if(!(__VA_ARGS__)) throw std::runtime_error("coordinator control line "+std::to_string(__LINE__)); } while(0)
struct NativeMock : Driver {
    unsigned glCalls=0,syncCalls=0,resourceCalls=0,objectCalls=0,calls=0,destroyed=0,badStage=0;
    std::uint32_t next=100;std::set<std::uint32_t> live,devices;
    std::uint64_t fenceOffset=0;
    bool vmStopped=false,requireVmStop=false;
    bool managedGpuMappings=false;
    bool allowsReservationBackedMappings() const override { return managedGpuMappings; }
    bool retainsMappedResidencyOnEvict() const override { return managedGpuMappings; }
    bool globalDevicePhase=false;
    bool liveSupported=true,liveReady=true;
    std::uint32_t blockedHandle=0;
    unsigned livePrepares=0;
    unsigned presentationStops=0;
    unsigned pendingPresentationPolls=0;
    bool presentationReady=true,presentationStopped=false,requirePresentationStop=false;
    std::map<std::uint32_t,std::uint32_t> paging,hardware;
    std::set<std::uint32_t> pagingSyncs,hardwareSyncs;
    unsigned pagingWaits=0;
    std::uint32_t waitedPagingSync=0;
    native_gpu::Capabilities caps{1,AsyncHwSubmitCapability|SyncCapability|GpuReservationCapability|VendorMemoryManagementCapability|
        ExpandedVendorGpuCapability|ExpandedVendorCpuCapability|VendorAllocationCapability|SharedVendorResourceCapability|NativeSharedTextureCapability|ContextCapability|GuestPagingCapability|HwQueueCapability,0x10de,0x1234};
    native_gpu::Capabilities capabilities() const override { return caps; }
    bool supportsLiveRetirement() const override { return liveSupported && !vmStopped; }
    bool prepareLiveRetirement() override { ++livePrepares;return supportsLiveRetirement() && liveReady; }
    bool stopCoordinatedPresentation() override {
        ++presentationStops;presentationStopped=presentationReady;return presentationStopped;
    }
    PresentationStopProgress pollCoordinatedPresentationStop() override {
        if(pendingPresentationPolls) { --pendingPresentationPolls;return PresentationStopProgress::Pending; }
        return stopCoordinatedPresentation() ? PresentationStopProgress::Complete : PresentationStopProgress::Failed;
    }
    SyncResult createSync(std::uint32_t device,SyncDesc desc) override {
        CHECK(devices.count(device) && validSync(desc));return {openAdapter(),{desc.type==5 ? 4096ull : 0ull,desc.type==5 ? 8192ull : 0ull}};
    }
    Result exportSync(std::uint32_t sync) override { CHECK(live.count(sync));return openAdapter(); }
    bool applicationMemoryReady=true,applicationMemorySelected=false;
    unsigned applicationMemoryCalls=0;
    bool beginApplicationMemory(ApplicationMemoryProfile profile) override {
        CHECK(++applicationMemoryCalls==1 && validApplicationMemoryProfile(profile));
        applicationMemorySelected=applicationMemoryReady;return applicationMemoryReady;
    }
    bool beginCoordinatedObjects(CoordinatedObjectCapacity value) override { CHECK(++objectCalls==1 && validCoordinatedObjectCapacity(value));return badStage!=4; }
    OpenGlProfile beginOpenGl(std::uint32_t version) override { CHECK(++glCalls==1 && (version==10 || version==11 || version==12 || version==13 || version==14));auto p=version==14 ? GuestOpenGlMultiappProfile : version==13 ? GuestOpenGlDesktopReferenceProfile : version==12 ? GuestOpenGlDesktopSlotProfile : version==11 ? GuestOpenGlDesktopFenceProfile : GuestOpenGlDesktopPresentationProfile;if(badStage==1) p.objectLimit=0;return p; }
    SyncSharingProfile beginSyncSharing() override { CHECK(++syncCalls==1);auto p=GuestSyncSharingProfile;if(badStage==2) p.version=1;return p; }
    ResourceSharingProfile beginResourceSharing() override { CHECK(++resourceCalls==1);auto p=GuestResourceSharingProfile;if(badStage==3) p.handleLimit=0;return p; }
    Result openAdapter() override { ++calls;live.insert(++next);return {0,next,0}; }
    Result queryVersion(std::uint32_t adapter) override { ++calls;CHECK(live.count(adapter));return {0,0,31}; }
    Result createDevice(std::uint32_t adapter) override { CHECK(live.count(adapter));const auto result=openAdapter();devices.insert(result.nativeHandle);return result; }
    Result queryAdapter(std::uint32_t,QueryDesc,std::vector<std::uint8_t>&) override { return {-95,0,0}; }
    Result createContext(std::uint32_t device,ContextDesc desc,std::vector<std::uint8_t>& data) override { CHECK(devices.count(device) && validContext(desc) && data.size()==desc.privateBytes);return openAdapter(); }
    GuestPagingResult createGuestPagingQueue(std::uint32_t device) override {
        CHECK(devices.count(device));const auto queue=openAdapter();const auto sync=++next;pagingSyncs.insert(sync);paging.emplace(queue.nativeHandle,sync);return {queue,sync,fenceOffset};
    }
    HwQueueResult createHwQueue(std::uint32_t context,HwQueueDesc desc,std::vector<std::uint8_t>& data) override {
        CHECK(live.count(context) && validHwQueue(desc) && data.size()==desc.privateBytes);
        const auto queue=openAdapter();const auto sync=++next;hardwareSyncs.insert(sync);hardware.emplace(queue.nativeHandle,sync);return {queue,sync,0,4096};
    }
    Result queueContextWait(std::uint32_t context,const std::vector<HwQueueWaitItem>& items) override {
        CHECK(live.count(context) && items.size()==1 && (pagingSyncs.count(items[0].sync) || hardwareSyncs.count(items[0].sync)));++pagingWaits;waitedPagingSync=items[0].sync;return {0,0,0};
    }
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
    Result reserveGpuAddress(std::uint32_t adapter,GpuReservationDesc desc) override {
        CHECK(live.count(adapter) && validGpuReservation(desc));const auto result=openAdapter();
        return {0,result.nativeHandle,0x10000000ull+result.nativeHandle*65536ull};
    }
    Result destroy(Kind kind,std::uint32_t id) override {
        CHECK(!requirePresentationStop || presentationStopped);
        if(id==blockedHandle) return {-16,0,0};
        if(kind==Kind::PagingSync || kind==Kind::HwQueueSync) return {0,0,0};
        CHECK(!requireVmStop || vmStopped);
        if(globalDevicePhase && kind==Kind::Adapter && !devices.empty()) return {-16,0,0};
        if(kind==Kind::Device) CHECK(devices.erase(id)==1);
        if(kind==Kind::PagingQueue) { CHECK(pagingSyncs.erase(paging.at(id))==1);paging.erase(id); }
        if(kind==Kind::HwQueue) { CHECK(hardwareSyncs.erase(hardware.at(id))==1);hardware.erase(id); }
        CHECK(live.erase(id)==1);++destroyed;return {0,0,0};
    }
};
template<class T> static std::vector<std::uint8_t> encoded(const T& value) { std::vector<std::uint8_t> out(sizeof value);std::memcpy(out.data(),&value,sizeof value);return out; }
template<class T> static T at(const std::vector<std::uint8_t>& value,std::size_t offset=0) { CHECK(value.size()>=offset+sizeof(T));T out{};std::memcpy(&out,value.data()+offset,sizeof out);return out; }
static std::vector<std::uint8_t> inner(Op op,std::uint32_t handle=0,const std::vector<std::uint8_t>& body={}) {
    auto out=encoded(Header{static_cast<std::uint32_t>(op),handle,0,0});out.insert(out.end(),body.begin(),body.end());return out;
}
struct Control {
    cg::Runtime& owner;std::uint64_t sequence=1,callSequence=1;
    std::vector<std::uint8_t> control(cg::Operation op,const std::vector<std::uint8_t>& body={}) {
        cg::Frame h;h.operation=op;h.sequence=sequence++;h.bytes=static_cast<std::uint32_t>(body.size());auto out=encoded(h);out.insert(out.end(),body.begin(),body.end());return owner.dispatch(out);
    }
    std::vector<std::uint8_t> call(std::uint64_t actor,const std::vector<std::uint8_t>& body) {
        actors::Envelope h;h.actor=actor;h.sequence=callSequence++;h.bytes=static_cast<std::uint32_t>(body.size());auto out=encoded(h);out.insert(out.end(),body.begin(),body.end());return owner.dispatch(out);
    }
    std::uint64_t reg(cg::Role role=cg::Role::Application) {
        const auto reply=control(cg::Operation::Register,encoded(cg::Registration{role,0}));CHECK(!at<cg::Frame>(reply).status);return at<std::uint64_t>(reply,32);
    }
};
static void liveRetirementControls() {
    {
        NativeMock driver;cg::Runtime owner(driver);Control c{owner};
        c.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects}));
        const auto actor=c.reg();c.call(actor,hello());c.call(actor,inner(Op::OpenAdapter));
        c.control(cg::Operation::Disconnect,encoded(actor));
        CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(actor))).status==-95 && !driver.destroyed);
        CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-16);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status==-16 && !driver.destroyed);
        CHECK(owner.teardownAfterVmExit(true));
    }
    for(unsigned field=0;field<4;++field) {
        auto profile=LiveRetirementProfile{};std::uint32_t words[4]{};std::memcpy(words,&profile,sizeof words);
        ++words[field];std::memcpy(&profile,words,sizeof words);CHECK(!validLiveRetirementProfile(profile));
    }
    for(unsigned field=0;field<8;++field) {
        LiveRetirementProgress progress{1,1,20,16,16,0,0,0};CHECK(validLiveRetirementProgress(progress));
        if(field==0) progress.version=2;
        if(field==1) progress.phase=5;
        if(field==2) progress.remaining=0;
        if(field==3) progress.attempted=17;
        if(field==4) progress.destroyed=17;
        if(field==5) progress.sharedReferences=21;
        if(field==6) progress.flags=8;
        if(field==7) progress.ntstatus=1;
        CHECK(!validLiveRetirementProgress(progress));
    }
    for(const auto& capacity:{GuestOpenGlDesktopPresentationCapacity,GuestOpenGlDesktopFenceCapacity,GuestOpenGlDesktopSlotCapacity}) {
        NativeMock driver;cg::Runtime owner(driver);Control c{owner};
        const auto objects=capacity.mapping.aperture.version==10 ? CoordinatedObjectCapacity{} : GuestCoordinatedDesktopObjects;
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(cg::Initialization{capacity,objects}))).status);
        CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-95 && !driver.livePrepares);
    }
    NativeMock driver;cg::Runtime owner(driver);Control c{owner};
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects}))).status);
    driver.liveSupported=false;
    CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-95);
    driver.liveSupported=true;
    CHECK(validLiveRetirementProfile(at<LiveRetirementProfile>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{})),32)));
    CHECK(at<cg::Frame>(c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}))).status==-16);
    const auto primary=c.reg(cg::Role::Compositor),app=c.reg(),spare1=c.reg(),spare2=c.reg();
    (void)spare1;(void)spare2;
    for(const auto actor:{primary,app}) {
        CHECK(!at<Header>(c.call(actor,hello()),40).status);
        CHECK(!at<Header>(c.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopReferenceCapacity))),40).status);
        CHECK(at<Header>(c.call(actor,inner(Op::OpenAdapter)),40).handle==1);
    }
    CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(app))).status==-16 && !driver.destroyed);
    CHECK(at<Header>(c.call(app,inner(Op::CreateDevice,1)),40).handle==2);
    auto context=encoded(ContextDesc{0,1,16,16,4,0});context.resize(context.size()+4);
    CHECK(at<Header>(c.call(app,inner(Op::CreateContext,2,context)),40).handle==3);
    auto queue=encoded(HwQueueDesc{0,4,0,0});queue.resize(queue.size()+4);
    CHECK(at<Header>(c.call(app,inner(Op::CreateHwQueue,3,queue)),40).handle==4);
    driver.blockedHandle=driver.next-1;
    for(unsigned n=0;n<25;++n) CHECK(!at<Header>(c.call(app,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(app))).status);
    const auto before=owner.objectCount();driver.liveReady=false;
    auto progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(app)),32);
    CHECK(validLiveRetirementProgress(progress) && progress.flags==RetirementGpuBusy && !progress.attempted && !driver.destroyed && owner.objectCount()==before);
    driver.liveReady=true;
    progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(app)),32);
    CHECK(progress.phase==0 && progress.flags==RetirementNativeBusy && progress.remaining && driver.hardware.size()==1 && driver.devices.size()==1);
    CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(app))).status==-16);
    CHECK(at<cg::Frame>(c.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-24);
    driver.blockedHandle=0;unsigned batches=0;
    do {
        progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(app)),32);++batches;
        CHECK(validLiveRetirementProgress(progress) && progress.attempted<=16 && !progress.flags && batches<=8);
        CHECK(!at<Header>(c.call(primary,inner(Op::QueryDriverVersion,1)),40).status);
    } while(progress.remaining);
    CHECK(batches>=2 && progress.phase==4 && !driver.vmStopped && driver.devices.empty() && driver.hardware.empty());
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(app))).status && owner.liveRetiredActorCount()==1);
    CHECK(at<actors::Envelope>(c.call(app,hello())).status==-9);
    for(unsigned cycle=0;cycle<5;++cycle) {
        const auto actor=c.reg();CHECK(actor>4);
        c.call(actor,hello());c.call(actor,inner(Op::OpenAdapter));c.call(actor,inner(Op::CreateDevice,1));
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(actor))).status);
        progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(actor)),32);
        CHECK(!progress.remaining && progress.destroyed==2 && !driver.vmStopped);
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status);
    }
    CHECK(owner.liveRetiredActorCount()==6 && owner.liveRetiredObjectCount()>=40);
    const auto shared=c.reg();c.call(shared,hello());c.call(shared,inner(Op::BeginSyncSharing));
    c.call(shared,inner(Op::OpenAdapter));c.call(shared,inner(Op::CreateDevice,1));
    const auto sync=at<Header>(c.call(shared,inner(Op::CreateSync,2,encoded(SyncDesc{5,NtSharedGpuSyncFlags,1,0,0}))),40);
    CHECK(!sync.status && sync.handle);
    const auto exported=at<Header>(c.call(shared,inner(Op::ExportSync,sync.handle)),40);CHECK(!exported.status && exported.handle);
    c.control(cg::Operation::Disconnect,encoded(shared));const auto prepares=driver.livePrepares;
    progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(shared)),32);
    CHECK(progress.flags==RetirementSharedReferences && progress.sharedReferences==1 && driver.livePrepares==prepares && !progress.attempted);
    CHECK(!at<Header>(c.control(cg::Operation::ReapReference,encoded(cg::Reference{shared,Kind::SharedSync,exported.handle})),32).status);
    progress=at<LiveRetirementProgress>(c.control(cg::Operation::RetireExitedActor,encoded(shared)),32);
    CHECK(!progress.remaining && !progress.flags);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(shared))).status);
    c.control(cg::Operation::Disconnect,encoded(primary));
    CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(primary))).status==-13);
    CHECK(owner.teardownAfterVmExit(true) && driver.live.empty());
    std::cout<<"LIVE_RETIREMENT_CONTROL negotiatedOnly=true exitedApplicationOnly=true pendingGpuWorkRetained=true sharedReferencesRetained=true failedQueueRetainsParents=true batch16=true actorSlotsReused=true native=mock gpu=false\n";
}
static void terminalRetirementControls() {
    {
        StoppedGroupRetirementProgress value;value.step.phase=4;value.actor=1;value.actorsRemaining=1;
        CHECK(validStoppedGroupRetirementProgress(value)); // This actor is empty; group is not.
        for(unsigned mutation=0;mutation<7;++mutation) {
            auto bad=value;
            if(mutation==0) bad.actorsRemaining=9;
            if(mutation==1) bad.reserved=1;
            if(mutation==2) bad.actor=0;
            if(mutation==3) { bad.actorsRemaining=0;bad.step.remaining=1;bad.step.phase=0; }
            if(mutation==4) bad.step.attempted=17;
            if(mutation==5) bad.step.version=2;
            if(mutation==6) bad.step.remaining=1025;
            CHECK(!validStoppedGroupRetirementProgress(bad));
        }
        value.actorsRemaining=8;CHECK(!validStoppedGroupRetirementProgress(value,4));
    }
    for(unsigned scenario=0;scenario<4;++scenario) {
        NativeMock driver;cg::Runtime owner(driver);Control c{owner};
        c.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects}));
        CHECK(at<cg::Frame>(c.control(cg::Operation::RetireStoppedGroup)).status==-95);
        c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}));
        const auto primary=c.reg(cg::Role::Compositor);
        c.call(primary,hello());c.call(primary,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopReferenceCapacity)));
        c.call(primary,inner(Op::OpenAdapter));
        CHECK(at<cg::Frame>(c.control(cg::Operation::RetireStoppedGroup)).status==-16 && !driver.presentationStops);
        if(scenario==0) {
            const auto app=c.reg();c.control(cg::Operation::Disconnect,encoded(primary));
            CHECK(at<cg::Frame>(c.control(cg::Operation::RetireStoppedGroup)).status==-16 && !driver.presentationStops);
            c.call(app,hello());c.call(app,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopReferenceCapacity)));
            c.call(app,inner(Op::OpenAdapter));c.call(app,inner(Op::CreateDevice,1));
            c.control(cg::Operation::Disconnect,encoded(app));
            driver.globalDevicePhase=true; // Primary adapter waits for the app device.

        } else if(scenario==1) {
            c.call(primary,inner(Op::BeginSyncSharing));c.call(primary,inner(Op::CreateDevice,1));
            const auto sync=at<Header>(c.call(primary,inner(Op::CreateSync,2,encoded(SyncDesc{5,NtSharedGpuSyncFlags,1,0,0}))),40);
            CHECK(sync.handle);
            const auto reference=at<Header>(c.call(primary,inner(Op::ExportSync,sync.handle)),40);CHECK(reference.handle);
            c.control(cg::Operation::Disconnect,encoded(primary));
            CHECK(at<cg::Frame>(c.control(cg::Operation::RetireStoppedGroup)).status==-16 && !driver.presentationStops);
            CHECK(!at<Header>(c.control(cg::Operation::ReapReference,encoded(cg::Reference{primary,Kind::SharedSync,reference.handle})),32).status);
        } else {
            if(scenario==3) for(unsigned n=0;n<25;++n)
                CHECK(!at<Header>(c.call(primary,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status);
            c.control(cg::Operation::Disconnect,encoded(primary));
        }
        CHECK(at<cg::Frame>(c.control(cg::Operation::RetireExitedActor,encoded(primary))).status==-13);
        if(scenario==2) {
            driver.presentationReady=false;
            bool failed=false;try { (void)c.control(cg::Operation::RetireStoppedGroup); } catch(const std::runtime_error&) { failed=true; }
            CHECK(failed && owner.failed() && !driver.destroyed && driver.presentationStops==1);
            continue;
        }
        const auto destroyedBefore=driver.destroyed;
        if(scenario==0) {
            driver.pendingPresentationPolls=3;
            const auto actorsBefore=owner.actorCount(),objectsBefore=owner.objectCount();
            for(unsigned n=0;n<3;++n) {
                CHECK(at<cg::Frame>(c.control(cg::Operation::RetireStoppedGroup)).status==-16);
                CHECK(!owner.failed() && driver.destroyed==destroyedBefore && !driver.presentationStops);
                CHECK(owner.actorCount()==actorsBefore && owner.objectCount()==objectsBefore);
                CHECK(at<cg::Frame>(c.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-32);
                CHECK(!at<cg::Frame>(c.control(cg::Operation::Info)).status);
            }
        }
        driver.requirePresentationStop=true;driver.liveReady=false;
        auto progress=at<StoppedGroupRetirementProgress>(c.control(cg::Operation::RetireStoppedGroup),32);
        CHECK(progress.step.flags==RetirementGpuBusy && !progress.step.attempted && driver.destroyed==destroyedBefore && driver.presentationStops==1);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-32);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Alias,encoded(cg::Alias{}))).status==-32);
        driver.liveReady=true;driver.blockedHandle=*driver.live.rbegin();
        progress=at<StoppedGroupRetirementProgress>(c.control(cg::Operation::RetireStoppedGroup),32);
        CHECK(progress.step.remaining && progress.step.flags==RetirementNativeBusy && !driver.vmStopped);
        driver.blockedHandle=0;
        unsigned batches=0;
        do {
            progress=at<StoppedGroupRetirementProgress>(c.control(cg::Operation::RetireStoppedGroup),32);++batches;
            CHECK(validStoppedGroupRetirementProgress(progress) && progress.step.attempted<=16 && batches<=8);
        } while(progress.actorsRemaining);
        CHECK(owner.actorCount()==0 && owner.objectCount()==0 && driver.live.empty() && driver.presentationStops==1 && !driver.vmStopped);
        progress=at<StoppedGroupRetirementProgress>(c.control(cg::Operation::RetireStoppedGroup),32);
        CHECK(!progress.actorsRemaining && !progress.step.remaining && !progress.step.attempted && driver.presentationStops==1);
    }
    {
        NativeMock driver;driver.managedGpuMappings=true;
        cg::Runtime owner(driver,8);Control c{owner};
        const cg::ApplicationInitialization wanted{{GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects},8,0};
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status);
        c.control(cg::Operation::BeginLiveRetirement,encoded(LiveRetirementProfile{}));
        std::vector<std::uint64_t> actors{c.reg(cg::Role::Compositor)};
        for(unsigned n=1;n<8;++n) actors.push_back(c.reg());
        for(unsigned n=0;n<actors.size();++n) {
            const auto id=actors[n];
            if(n!=1) { // An empty secondary must not report the whole group finished.
                c.call(id,hello());c.call(id,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlMultiappCapacity)));
                c.call(id,inner(Op::OpenAdapter));c.call(id,inner(Op::CreateDevice,1));
                for(unsigned j=0;j<25;++j)
                    CHECK(!at<Header>(c.call(id,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status);
            }
            c.control(cg::Operation::Disconnect,encoded(id));
        }
        driver.requirePresentationStop=true;driver.globalDevicePhase=true;driver.liveReady=false;
        std::set<std::uint64_t> visited;bool partialEmpty=false;unsigned steps=0;
        for(unsigned n=0;n<8;++n) {
            const auto result=at<StoppedGroupRetirementProgress>(c.control(cg::Operation::RetireStoppedGroup),32);
            CHECK(validStoppedGroupRetirementProgress(result) && result.actorsRemaining==owner.actorCount());
            CHECK(result.actorsRemaining && !result.step.attempted && !driver.destroyed);
            visited.insert(result.actor);partialEmpty|=!result.step.remaining;
        }
        CHECK(visited.size()==8 && partialEmpty && owner.actorCount()==7 && driver.presentationStops==1);
        driver.liveReady=true;
        StoppedGroupRetirementProgress result;
        do {
            result=at<StoppedGroupRetirementProgress>(c.control(cg::Operation::RetireStoppedGroup),32);
            CHECK(validStoppedGroupRetirementProgress(result) && result.actorsRemaining==owner.actorCount());
            CHECK(++steps<100 && result.step.attempted<=16 && driver.presentationStops==1);
        } while(result.actorsRemaining);
        CHECK(!owner.objectCount() && driver.live.empty() && driver.devices.empty() && !driver.vmStopped);
    }
    std::cout<<"TERMINAL_GROUP_RETIREMENT stoppedActorsOnly=true referencesDrained=true presentationBeforeObjects=true failedDrainFatal=true sealed=true roundRobinActors=8 emptyActorNotGroupCompletion=true crossActorDependencies=true native=mock gpu=false\n";
}
static void applicationActorControls() {
    for(const auto limit:{0u,1u,5u,9u,UINT32_MAX}) {
        NativeMock driver;bool rejected=false;
        try { cg::Runtime owner(driver,limit); } catch(const std::invalid_argument&) { rejected=true; }
        CHECK(rejected && !driver.objectCalls && !driver.glCalls);
    }
    const cg::ApplicationInitialization wanted{{GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects},8,0};
    {
        NativeMock driver;driver.managedGpuMappings=true;cg::Runtime owner(driver);Control c{owner};
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status==-22);
        CHECK(!driver.objectCalls && !driver.glCalls && !owner.failed());
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted.base))).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),sizeof(cg::Frame));
        CHECK(cg::validInfo(info) && info.maxActors==4 && !info.flags);
    }
    {
        NativeMock driver;driver.managedGpuMappings=true;cg::Runtime owner(driver,8);Control c{owner};
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted.base))).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),sizeof(cg::Frame));
        CHECK(cg::validInfo(info) && info.maxActors==4 && !info.flags);
        for(unsigned n=0;n<4;++n) CHECK(c.reg()>0);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-24);
        CHECK(owner.actorCount()==4 && !owner.failed() && owner.teardownAfterVmExit(true));
    }
    for(unsigned mutation=0;mutation<5;++mutation) {
        NativeMock driver;driver.managedGpuMappings=true;cg::Runtime owner(driver,8);Control c{owner};
        auto bad=wanted;
        if(mutation==0) bad.actorSlots=4;
        if(mutation==1) bad.actorSlots=9;
        if(mutation==2) bad.reserved=1;
        if(mutation==3) bad.base.capacity=GuestOpenGlDesktopReferenceCapacity;
        auto bytes=encoded(bad);if(mutation==4) bytes.pop_back();
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,bytes)).status==-22);
        CHECK(!driver.glCalls && !driver.objectCalls && !owner.failed());
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status);
    }
    {
        NativeMock driver;driver.managedGpuMappings=true;cg::Runtime owner(driver,8);Control c{owner};
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status);
        auto info=at<cg::Info>(c.control(cg::Operation::Info),sizeof(cg::Frame));
        CHECK(cg::validInfo(info) && info.maxActors==8 && info.flags==cg::ApplicationActorsFlag);
        CHECK(!std::memcmp(&info.objects,&wanted.base.objects,sizeof info.objects));
        CHECK(info.objects.hwQueues==64 && info.objects.syncObjects==768 && info.objects.leaseLimit==1536);
        for(unsigned mutation=0;mutation<4;++mutation) {
            auto bad=info;
            if(mutation==0) bad.flags=0;
            if(mutation==1) bad.maxActors=4;
            if(mutation==2) bad.flags=2;
            if(mutation==3) bad.reserved=1;
            CHECK(!cg::validInfo(bad));
        }
        std::vector<std::uint64_t> ids;
        for(unsigned n=0;n<8;++n) {
            ids.push_back(c.reg());CHECK(ids.back()==n+1);
            CHECK(!at<Header>(c.call(ids.back(),hello()),40).status);
        }
        CHECK(at<cg::Frame>(c.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-24);
        CHECK(owner.actorCount()==8 && !owner.failed() && driver.glCalls==1 && driver.objectCalls==1);
        for(const auto id:{ids.front(),ids.back()}) {
            CHECK(at<Header>(c.call(id,inner(Op::OpenAdapter)),40).handle==1);
            CHECK(at<Header>(c.call(id,inner(Op::CreateDevice,1)),40).handle==2);
        }
        const auto before=driver.calls;
        CHECK(at<Header>(c.call(ids.back(),inner(Op::QueryDriverVersion,3)),40).status<0 && driver.calls==before);
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(ids.front()))).status);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(ids.front()))).status==-16);
        CHECK(owner.actorCount()==8 && driver.live.size()==4);
        CHECK(owner.teardownAfterVmExit(true) && driver.live.empty());
    }
    std::cout<<"APPLICATION_ACTOR_CONTROL explicitOwnerOptIn=true legacyFourPreserved=true eightNamespaces=true aggregateObjectBudgetUnchanged=true malformedRejected=true mock=true gpu=false\n";
}
static void applicationReferenceControls() {
    const cg::ApplicationReferenceInitialization wanted{{{GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects},8,0},32,0};
    for(const auto actors:{4u,8u}) {
        NativeMock driver;cg::Runtime owner(driver,actors);Control c{owner};
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status==-22);
        CHECK(!driver.glCalls && !driver.objectCalls && !owner.failed());
    }
    for(const auto slots:{0u,17u,31u,33u,UINT32_MAX}) {
        NativeMock driver;bool refused=false;
        try { cg::Runtime owner(driver,8,slots); } catch(const std::invalid_argument&) { refused=true; }
        CHECK(refused && !driver.glCalls);
    }
    {
        NativeMock driver;bool refused=false;
        try { cg::Runtime owner(driver,4,32); } catch(const std::invalid_argument&) { refused=true; }
        CHECK(refused);
    }
    for(unsigned mutation=0;mutation<7;++mutation) {
        NativeMock driver;driver.managedGpuMappings=true;cg::Runtime owner(driver,8,32);Control c{owner};
        auto bad=wanted;
        if(mutation==0) bad.resourceReferences=16;
        if(mutation==1) bad.resourceReferences=33;
        if(mutation==2) bad.reserved=1;
        if(mutation==3) bad.base.reserved=1;
        if(mutation==4) bad.base.actorSlots=4;
        if(mutation==5) bad.base.base.objects.version=8;
        auto bytes=encoded(bad);if(mutation==6) bytes.pop_back();
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,bytes)).status==-22 && !owner.failed());
        CHECK(!driver.glCalls && !driver.objectCalls);
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.maxActors==8 && info.flags==3 && info.resource.handleLimit==32);
        CHECK(info.sync.handleLimit==16 && info.objects.hwQueues==64 && info.objects.syncObjects==768);
        auto wrong=info;wrong.flags=1;CHECK(!cg::validInfo(wrong));
        wrong=info;wrong.resource.version=1;CHECK(!cg::validInfo(wrong));
        wrong=info;wrong.resource.handleLimit=33;CHECK(!cg::validInfo(wrong));
        CHECK(owner.teardownAfterVmExit(true));
    }
    for(const bool legacy:{false,true}) {
        NativeMock driver;driver.managedGpuMappings=true;cg::Runtime owner(driver,8,32);Control c{owner};
        const auto bytes=legacy ? encoded(wanted.base.base) : encoded(wanted.base);
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,bytes)).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.resource.handleLimit==16 && info.resource.version==1);
    }
    std::cout<<"APPLICATION_REFERENCE_CONTROL ownerAndControllerOptIn=true legacySixteenPreserved=true syncLimitUnchanged=true malformedRejected=true mock=true gpu=false\n";
}
static void applicationMemoryControls() {
    const cg::ApplicationMemoryInitialization wanted{{{{GuestApplicationMemoryCapacity,GuestCoordinatedMultiappObjects},8,0},32,0},GuestApplicationMemoryProfile};
    {
        NativeMock d;d.managedGpuMappings=true;cg::Runtime owner(d,8,32);Control c{owner};
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status==-22);
        CHECK(!d.applicationMemoryCalls && !d.glCalls && !owner.failed());
    }
    for(unsigned older=0;older<3;++older) {
        NativeMock d;d.managedGpuMappings=true;cg::Runtime owner(d,8,32,true);Control c{owner};
        const cg::ApplicationReferenceInitialization old{{{GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects},8,0},32,0};
        const auto request=older==0 ? encoded(old.base.base) : older==1 ? encoded(old.base) : encoded(old);
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,request)).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && !(info.flags&cg::ApplicationMemoryFlag));
        CHECK(info.capacity.reportedGpuByteLimit==OpenGlDesktopPresentationReportedBytes && !d.applicationMemoryCalls);
        CHECK(owner.teardownAfterVmExit(true));
    }
    for(unsigned badOwner=0;badOwner<2;++badOwner) {
        NativeMock d;bool refused=false;
        try { cg::Runtime owner(d,badOwner ? 8u : 4u,16,true); }
        catch(const std::invalid_argument&) { refused=true; }
        CHECK(refused && !d.applicationMemoryCalls && !d.glCalls);
    }
    for(unsigned mutation=0;mutation<10;++mutation) {
        NativeMock d;d.managedGpuMappings=true;cg::Runtime owner(d,8,32,true);Control c{owner};auto bad=wanted;
        if(mutation==0) bad.memory.version=2;
        if(mutation==1) bad.memory.reserved=1;
        if(mutation==2) bad.memory.reportedBytes=OpenGlDesktopPresentationReportedBytes;
        if(mutation==3) bad.base.base.base.capacity.reportedGpuByteLimit=OpenGlDesktopPresentationReportedBytes;
        if(mutation==4) bad.base.base.base.capacity.mapping.gpuMappedByteLimit=0;
        if(mutation==5) bad.base.base.actorSlots=4;
        if(mutation==6) bad.base.resourceReferences=16;
        if(mutation==7) bad.base.base.base.objects.version=8;
        if(mutation==8) bad.base.reserved=1;
        auto request=encoded(bad);if(mutation==9) request.pop_back();
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,request)).status==-22);
        CHECK(!d.applicationMemoryCalls && !d.glCalls && !d.objectCalls && !owner.failed());
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status);
        CHECK(d.applicationMemoryCalls==1 && d.applicationMemorySelected && d.glCalls==1);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.flags==7 && info.capacity.reportedGpuByteLimit==GuestApplicationMemoryProfile.reportedBytes);
        CHECK(sizeof(info)==176 && info.objects.vendorAllocations==1024 && info.resource.handleLimit==32 && info.sync.handleLimit==16);
        auto forged=info;forged.flags=3;CHECK(!cg::validInfo(forged));
        forged=info;forged.capacity.reportedGpuByteLimit=OpenGlDesktopPresentationReportedBytes;CHECK(!cg::validInfo(forged));
        for(unsigned n=0;n<2;++n) {
            const auto actor=c.reg();CHECK(!at<Header>(c.call(actor,hello()),40).status);
            CHECK(at<Header>(c.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlMultiappCapacity))),40).status==-22);
            CHECK(!at<Header>(c.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestApplicationMemoryCapacity))),40).status);
        }
        CHECK(d.glCalls==1 && d.applicationMemoryCalls==1);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status==-16);
        CHECK(owner.teardownAfterVmExit(true));
    }
    for(unsigned older=0;older<2;++older) {
        NativeMock d;cg::Runtime owner(d,8,32,true);Control c{owner};
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,older ? encoded(wanted.base) : encoded(wanted.base.base.base))).status==-22);
        CHECK(!d.applicationMemoryCalls && !d.glCalls);
    }
    {
        NativeMock d;d.applicationMemoryReady=false;d.managedGpuMappings=true;cg::Runtime owner(d,8,32,true);Control c{owner};bool refused=false;
        try { c.control(cg::Operation::Initialize,encoded(wanted)); } catch(const std::runtime_error&) { refused=true; }
        CHECK(refused && owner.failed() && d.applicationMemoryCalls==1 && !d.glCalls && !d.objectCalls);
    }
    std::cout<<"APPLICATION_MEMORY_CONTROL explicit112Bytes=true ownerAndControllerRequired=true oldPayloadBudgetsPreserved=true infoLayoutPreserved=true malformedRejectedBeforeNative=true nativeHeadroomFailureBeforeGl=true clientMustMatchAgreement=true sharedPool=true mock=true gpu=false\n";
}
static void applicationObjectControls() {
    const cg::ApplicationMemoryInitialization wanted{{{{GuestApplicationMemoryCapacity,GuestCoordinatedApplicationObjects},8,0},32,0},GuestApplicationMemoryProfile};
    {
        NativeMock d;bool refused=false;
        try { cg::Runtime owner(d,8,32,false,true); }
        catch(const std::invalid_argument&) { refused=true; }
        CHECK(refused && !d.glCalls && !d.objectCalls);
    }
    {
        NativeMock d;d.managedGpuMappings=true;cg::Runtime owner(d,8,32,true);Control c{owner};
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status==-22);
        CHECK(!d.applicationMemoryCalls && !d.glCalls && !d.objectCalls && !owner.failed());
    }
    for(unsigned mutation=0;mutation<10;++mutation) {
        NativeMock d;d.managedGpuMappings=true;cg::Runtime owner(d,8,32,true,true);Control c{owner};auto bad=wanted;
        auto& objects=bad.base.base.base.objects;
        if(mutation==0) objects.vendorAllocations=2049;
        if(mutation==1) objects.vendorAllocations=1024;
        if(mutation==2) objects.version=9;
        if(mutation==3) objects.leaseLimit=2048;
        if(mutation==4) objects.hwQueues=128;
        if(mutation==5) objects.syncObjects=1024;
        if(mutation==6) bad.base.base.base.capacity.mapping.aperture.cpuByteLimit*=2;
        if(mutation==7) bad.memory.reportedBytes*=2;
        auto request=mutation==8 ? encoded(bad.base) : encoded(bad);
        if(mutation==9) request.push_back(0);
        CHECK(at<cg::Frame>(c.control(cg::Operation::Initialize,request)).status==-22);
        CHECK(!d.glCalls && !d.applicationMemoryCalls && !d.objectCalls && !owner.failed());
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(wanted))).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.flags==cg::ApplicationObjectsFlags && info.objects.vendorAllocations==2048);
        CHECK(info.objects.version==10 && info.objects.hwQueues==64 && info.objects.syncObjects==768 && info.objects.leaseLimit==1536);
        CHECK(!std::memcmp(&info.capacity,&GuestApplicationMemoryCapacity,sizeof info.capacity));
        CHECK(cg::matchesApplicationMemoryCapacity(GuestApplicationMemoryCapacity,info));
        auto forged=info;forged.flags=cg::ApplicationMemoryFlags;CHECK(!cg::validInfo(forged));
        forged=info;forged.objects=GuestCoordinatedMultiappObjects;CHECK(!cg::validInfo(forged));
        forged=info;forged.flags=cg::ApplicationActorsFlag|cg::ApplicationObjectsFlag;CHECK(!cg::validInfo(forged));
        const auto actor=c.reg();CHECK(!at<Header>(c.call(actor,hello()),40).status);
        CHECK(!at<Header>(c.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestApplicationMemoryCapacity))),40).status);
        CHECK(d.glCalls==1 && d.objectCalls==1 && d.applicationMemoryCalls==1);
        CHECK(owner.teardownAfterVmExit(true));
    }
    {
        NativeMock d;d.managedGpuMappings=true;cg::Runtime owner(d,8,32,true,true);Control c{owner};auto old=wanted;
        old.base.base.base.objects=GuestCoordinatedMultiappObjects;
        CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(old))).status);
        const auto info=at<cg::Info>(c.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.flags==cg::ApplicationMemoryFlags && info.objects.vendorAllocations==1024);
        CHECK(owner.teardownAfterVmExit(true));
    }
    std::cout<<"APPLICATION_OBJECT_CONTROL ownerAndControllerRequired=true legacy1024Preserved=true expanded2048=true byteBudgetsUnchanged=true malformedRejectedBeforeNative=true mock=true gpu=false\n";
}
static void submissionSummaryControls() {
    NativeMock driver;cg::Runtime owner(driver);Control c{owner};
    std::ostringstream output;
    struct Restore {
        std::streambuf* original;
        ~Restore() { std::cerr.rdbuf(original); }
    } restore{std::cerr.rdbuf(output.rdbuf())};
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}))).status);
    const auto actor=c.reg();c.call(actor,hello());c.call(actor,inner(Op::OpenAdapter));
    CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status==-16);
    CHECK(output.str().find("nativeActorSubmissionSummary")==std::string::npos);
    CHECK(!at<Header>(c.call(actor,inner(Op::CloseAdapter,1)),40).status);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(actor))).status);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status);
    const auto once=output.str();
    CHECK(once.find("\"nativeActorSubmissionSummary\":true,\"actor\":1,")!=std::string::npos);
    CHECK(once.find("\"acceptedCommands\":0,\"asynchronousCommands\":0,\"acceptedCommandBytes\":0,")!=std::string::npos);
    CHECK(once.find("\"countsExact\":true,\"validatedNativeRepliesOnly\":true,\"queueAcceptanceOnly\":true,\"wireObjectsRemaining\":0,\"beforeGlobalTeardown\":true")!=std::string::npos);
    CHECK(at<cg::Frame>(c.control(cg::Operation::Release,encoded(actor))).status==-9 && output.str()==once);
    const auto next=c.reg();CHECK(next!=actor);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Disconnect,encoded(next))).status);
    CHECK(!at<cg::Frame>(c.control(cg::Operation::Release,encoded(next))).status);
    const auto all=output.str();const auto position=all.find("nativeActorSubmissionSummary");
    const auto second=all.find("nativeActorSubmissionSummary",position+1);
    CHECK(second!=std::string::npos && all.find("nativeActorSubmissionSummary",second+1)==std::string::npos);
}
int main() {
    submissionSummaryControls();
    applicationObjectControls();
    applicationMemoryControls();
    applicationReferenceControls();
    applicationActorControls();
    terminalRetirementControls();
    liveRetirementControls();
    for(const auto capacity:{GuestOpenGlDesktopPresentationCapacity,GuestOpenGlDesktopFenceCapacity,GuestOpenGlDesktopSlotCapacity,GuestOpenGlDesktopReferenceCapacity}) {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        const auto objects=capacity.mapping.aperture.version==13 ? GuestCoordinatedReferenceObjects : capacity.mapping.aperture.version>=11 ? GuestCoordinatedDesktopObjects : CoordinatedObjectCapacity{};
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{capacity,objects}))).status);
        auto info=at<cg::Info>(client.control(cg::Operation::Info),32);
        CHECK(cg::validInfo(info) && info.gl.version==capacity.mapping.aperture.version);
        info.gl.version=info.gl.version==10 ? 11 : 10;CHECK(!cg::validInfo(info));
        const auto actor=client.reg();CHECK(!at<Header>(client.call(actor,hello()),40).status);
        const auto other=capacity.mapping.aperture.version==10 ? GuestOpenGlDesktopFenceCapacity : GuestOpenGlDesktopPresentationCapacity;
        CHECK(at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(other))),40).status==-95);
        CHECK(!at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(capacity))),40).status);
        CHECK(at<Header>(client.call(actor,inner(Op::OpenAdapter)),40).handle==1);
        CHECK(at<Header>(client.call(actor,inner(Op::CreateDevice,1)),40).handle==2);
        const unsigned reservationLimit=capacity.mapping.aperture.version>=11 ? 128u : 64u;
        for(unsigned n=0;n<reservationLimit;++n)
            CHECK(!at<Header>(client.call(actor,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status);
        CHECK(at<Header>(client.call(actor,inner(Op::ReserveGpuAddress,1,encoded(GpuReservationDesc{0,0,0,65536}))),40).status==-24);
        driver.fenceOffset=511*4096ull+4088;
        const auto paging=client.call(actor,inner(Op::CreateGuestPagingQueue,2));
        CHECK(at<Header>(paging,40).status==(capacity.mapping.aperture.version>=11 ? 0 : -5));
        if(capacity.mapping.aperture.version>=11) CHECK(at<PagingReply>(paging,72).offset==driver.fenceOffset);
        CHECK(owner.teardownAfterVmExit(true) && driver.live.empty());
    }
    for(const auto original:{GuestOpenGlDesktopFenceCapacity,GuestOpenGlDesktopSlotCapacity,GuestOpenGlDesktopReferenceCapacity}) for(unsigned field=0;field<12;++field) {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        auto capacity=original;std::uint32_t words[12]{};
        std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{capacity,GuestCoordinatedDesktopObjects}))).status==-22 && !driver.glCalls);
    }
    for(unsigned field=0;field<8;++field) {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        auto objects=GuestCoordinatedDesktopObjects;std::uint32_t words[8]{};
        std::memcpy(words,&objects,sizeof words);++words[field];std::memcpy(&objects,words,sizeof words);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopFenceCapacity,objects}))).status==-22 && !driver.glCalls);
    }
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopFenceCapacity,{}));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopPresentationCapacity,GuestCoordinatedDesktopObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopFenceCapacity,GuestCoordinatedDesktopObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedDesktopObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedReferenceObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedManagedObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedManagedObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedLeaseObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedLeaseObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedActorObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedActorObjects));
    CHECK(validCoordinatedAgreement(GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects));
    for(const auto old:{GuestCoordinatedReferenceObjects,GuestCoordinatedManagedObjects,GuestCoordinatedLeaseObjects,GuestCoordinatedActorObjects})
        CHECK(!validCoordinatedAgreement(GuestOpenGlMultiappCapacity,old));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedMultiappObjects));
    for(const bool supported:{false,true}) {
        NativeMock managed;managed.managedGpuMappings=supported;
        cg::Runtime owner(managed);Control client{owner};bool rejected=false;
        try {
            const auto reply=client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects}));
            CHECK(!at<cg::Frame>(reply).status);
            const auto info=at<cg::Info>(client.control(cg::Operation::Info),sizeof(cg::Frame));
            CHECK(cg::validInfo(info) && info.capacity.mapping.aperture.cpuByteLimit==OpenGlMultiappCpuBytes && info.objects.version==9);
        } catch(const std::runtime_error&) { rejected=true; }
        CHECK(rejected==!supported && owner.failed()==!supported);
    }
    for(const auto objects:{GuestCoordinatedManagedObjects,GuestCoordinatedLeaseObjects,GuestCoordinatedActorObjects})
    for(const bool supported:{false,true}) {
        NativeMock managed;managed.managedGpuMappings=supported;
        cg::Runtime owner(managed);Control client{owner};bool rejected=false;
        try {
            const auto reply=client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopReferenceCapacity,objects}));
            CHECK(!at<cg::Frame>(reply).status);
            const auto info=at<cg::Info>(client.control(cg::Operation::Info),sizeof(cg::Frame));
            CHECK(info.objects.version==objects.version && info.objects.leaseLimit==objects.leaseLimit && cg::validInfo(info));
        } catch(const std::runtime_error&) { rejected=true; }
        CHECK(rejected==!supported);
        CHECK(owner.failed()==!supported);
    }
    {
        auto old=GuestCoordinatedManagedObjects;old.leaseLimit=1536;CHECK(!validCoordinatedObjectCapacity(old));
        auto current=GuestCoordinatedLeaseObjects;current.leaseLimit=768;CHECK(!validCoordinatedObjectCapacity(current));
        current=GuestCoordinatedLeaseObjects;current.version=8;CHECK(!validCoordinatedObjectCapacity(current));
        current=GuestCoordinatedActorObjects;current.version=10;CHECK(!validCoordinatedObjectCapacity(current));
        current=GuestCoordinatedActorObjects;current.hwQueues=65;CHECK(!validCoordinatedObjectCapacity(current));
        current=GuestCoordinatedActorObjects;current.syncObjects=769;CHECK(!validCoordinatedObjectCapacity(current));
        current=GuestCoordinatedLeaseObjects;current.syncObjects=768;CHECK(!validCoordinatedObjectCapacity(current));
    }
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopReferenceCapacity,GuestCoordinatedDesktopObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,GuestCoordinatedReferenceObjects));
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopFenceCapacity,GuestCoordinatedReferenceObjects));
    for(const auto capacity:{GuestOpenGlDesktopReferenceCapacity,GuestOpenGlDesktopSlotCapacity,GuestOpenGlDesktopFenceCapacity})
        for(const auto objects:{GuestCoordinatedReferenceObjects,GuestCoordinatedDesktopObjects}) {
            const bool matching=capacity.mapping.aperture.version==13 ? objects.version==5 : objects.version==4;
            CHECK(validCoordinatedAgreement(capacity,objects)==matching);
        }
    CHECK(!validCoordinatedAgreement(GuestOpenGlDesktopSlotCapacity,{}));
    CHECK(CoordinatedObjectCapacity{}.version==3 && !CoordinatedObjectCapacity{}.vendorAllocations);
    {
        NativeMock driver;Session legacy(driver);CHECK(!at<Header>(legacy.dispatch(hello())).status);
        CHECK(at<Header>(legacy.dispatch(inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopFenceCapacity)))).status==-93 && !driver.glCalls);
        CHECK(at<Header>(legacy.dispatch(inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopSlotCapacity)))).status==-93 && !driver.glCalls);
        CHECK(at<Header>(legacy.dispatch(inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopReferenceCapacity)))).status==-93 && !driver.glCalls);
    }
    std::cout<<"COORDINATOR_FENCE_CONTROL profile11Exact512=true legacyLastPageRejected=true profileMismatchRejected=true byteBudgetsUnchanged=true native=mock gpu=false\n";
    NativeMock native;
    {
        cg::Runtime owner(native);Control client{owner};
        CHECK(at<cg::Frame>(client.control(cg::Operation::Info)).status==-71 && !native.glCalls);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(GuestOpenGlDesktopPresentationCapacity))).status==-22 && !native.glCalls);
        auto bad=GuestOpenGlDesktopPresentationCapacity;++bad.mapping.aperture.cpuSlots;
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{bad,{}}))).status==-22 && !native.glCalls);
        for(unsigned mutation=0;mutation<7;++mutation) {
            cg::Initialization request{GuestOpenGlDesktopPresentationCapacity,{}};
            if(mutation==0) request.objects.version=1;
            if(mutation==1) request.objects.hwQueues=33;
            if(mutation==2) request.objects.syncObjects=385;
            if(mutation==3) request.objects.leaseLimit=769;
            if(mutation==4) request.objects.reservedGpuBytes++;
            if(mutation==5) request.objects.reservations++;
            if(mutation==6) request.objects.vendorAllocations=1;
            CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(request))).status==-22 && !native.glCalls);
        }
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}))).status);
        const auto info=client.control(cg::Operation::Info);CHECK(!at<cg::Frame>(info).status && cg::validInfo(at<cg::Info>(info,32)));
        CHECK(at<cg::Frame>(client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}))).status==-16);
        const auto a=client.reg(cg::Role::Compositor),b=client.reg();CHECK(a==1 && b==2);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Register,encoded(cg::Registration{cg::Role::Compositor,0}))).status==-24);
        for(const auto actor:{a,b}) {
            CHECK(!at<Header>(client.call(actor,hello()),40).status);
            CHECK(!at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopPresentationCapacity))),40).status);
            CHECK(at<Header>(client.call(actor,inner(Op::BeginOpenGl,0,encoded(GuestOpenGlDesktopPresentationCapacity))),40).status==-16);
            CHECK(!at<Header>(client.call(actor,inner(Op::BeginSyncSharing)),40).status);
            CHECK(!at<Header>(client.call(actor,inner(Op::BeginResourceSharing)),40).status);
            CHECK(at<Header>(client.call(actor,inner(Op::OpenAdapter)),40).handle==1);
        }
        CHECK(native.glCalls==1 && native.objectCalls==1 && native.syncCalls==1 && native.resourceCalls==1 && owner.objectCount()==2);
        CHECK(at<Header>(client.call(b,inner(Op::QueueCompositorFrame)),40).status==-13);
        CHECK(at<Header>(client.call(a,inner(Op::QueueCompositorFrame)),40).status!=-13);
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Disconnect,encoded(a))).status);
        CHECK(at<actors::Envelope>(client.call(a,inner(Op::OpenAdapter))).status==-32);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Release,encoded(a))).status==-16 && owner.objectCount()==2 && !native.destroyed);
        CHECK(!at<Header>(client.call(b,inner(Op::QueryDriverVersion,1)),40).status);
        CHECK(at<cg::Frame>(client.control(cg::Operation::ReapReference,encoded(cg::Reference{a,Kind::Adapter,1}))).status==-22);
        const auto c=client.reg(),d=client.reg();CHECK(c==3 && d==4);
        CHECK(at<cg::Frame>(client.control(cg::Operation::Register,encoded(cg::Registration{}))).status==-24);
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Disconnect,encoded(c))).status);
        CHECK(!at<cg::Frame>(client.control(cg::Operation::Release,encoded(c))).status && client.reg()==5);
        native.requireVmStop=true;native.vmStopped=true; // Owner reaps VM before nonempty Session destruction.
    }
    CHECK(native.live.empty() && native.destroyed==2);
    for(const bool grouped:{false,true}) {
        NativeMock driver;std::unique_ptr<CoordinatedProfiles> group;
        if(grouped) group=std::make_unique<CoordinatedProfiles>(driver,GuestOpenGlDesktopPresentationCapacity);
        auto session=grouped ? std::make_unique<Session>(driver,*group) : std::make_unique<Session>(driver);
        session->dispatch(hello());session->dispatch(inner(Op::BeginSyncSharing));session->dispatch(inner(Op::OpenAdapter));session->dispatch(inner(Op::CreateDevice,1));
        auto context=encoded(ContextDesc{0,1,16,16,4,0});context.resize(context.size()+4);
        CHECK(at<Header>(session->dispatch(inner(Op::CreateContext,2,context))).handle==3);
        const auto result=session->dispatch(inner(Op::CreateGuestPagingQueue,2));const auto sync=at<PagingReply>(result,32).sync;
        std::uint32_t waitContext=3;
        auto wait=[&](std::uint32_t source) { auto payload=encoded(HwQueueWaitDesc{1,0});const auto item=encoded(HwQueueWaitItem{source,0,7});payload.insert(payload.end(),item.begin(),item.end());return session->dispatch(inner(Op::QueueContextWait,waitContext,payload)); };
        CHECK(at<Header>(wait(sync)).status==(grouped ? 0 : -9));
        CHECK(driver.pagingWaits==(grouped ? 1u : 0u));
        if(grouped) CHECK(driver.pagingSyncs.count(driver.waitedPagingSync));
        const auto foreignDevice=at<Header>(session->dispatch(inner(Op::CreateDevice,1))).handle;
        const auto foreign=session->dispatch(inner(Op::CreateGuestPagingQueue,foreignDevice));
        const auto before=driver.pagingWaits;
        CHECK(at<Header>(wait(at<PagingReply>(foreign,32).sync)).status==-9 && driver.pagingWaits==before);
        auto hardware=encoded(HwQueueDesc{0,4,0,0});hardware.resize(hardware.size()+4);
        const auto ownQueue=session->dispatch(inner(Op::CreateHwQueue,3,hardware));
        CHECK(!at<Header>(ownQueue).status);
        const auto progress=at<HwQueueReply>(ownQueue,32).sync;
        CHECK(at<Header>(wait(progress)).status==(grouped ? 0 : -9));
        if(grouped) CHECK(driver.hardwareSyncs.count(driver.waitedPagingSync));
        const auto foreignContext=at<Header>(session->dispatch(inner(Op::CreateContext,foreignDevice,context))).handle;
        const auto foreignQueue=session->dispatch(inner(Op::CreateHwQueue,foreignContext,hardware));
        const auto count=driver.pagingWaits;
        CHECK(at<Header>(wait(at<HwQueueReply>(foreignQueue,32).sync)).status==-9 && driver.pagingWaits==count);
        CHECK(at<Header>(wait(UINT32_MAX)).status==-9 && driver.pagingWaits==count);
        CHECK(!at<Header>(session->dispatch(inner(Op::DestroyHwQueue,at<Header>(ownQueue).handle))).status);
        CHECK(at<Header>(wait(progress)).status==-9 && driver.pagingWaits==count);
        waitContext=at<Header>(session->dispatch(inner(Op::CreateContext,2,encoded(ContextDesc{0,0,8,0,0,0})))).handle;
        CHECK(waitContext);
        CHECK(at<Header>(wait(sync)).status==(grouped ? 0 : -9));
        const auto after=driver.pagingWaits;
        CHECK(at<Header>(wait(at<PagingReply>(foreign,32).sync)).status==-9 && driver.pagingWaits==after);
        CHECK(at<Header>(session->dispatch(inner(Op::CreateHwQueue,waitContext,hardware))).status==-95);
        auto render=encoded(ContextDesc{0,1,0,12,4,0});render.resize(render.size()+4);
        waitContext=at<Header>(session->dispatch(inner(Op::CreateContext,2,render))).handle;
        CHECK(at<Header>(wait(sync)).status==-9 && driver.pagingWaits==after);
    }
    {
        NativeMock driver;cg::Runtime owner(driver);Control client{owner};
        client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}}));
        for(unsigned n=0;n<2;++n) {
            const auto id=client.reg();client.call(id,hello());client.call(id,inner(Op::OpenAdapter));client.call(id,inner(Op::CreateDevice,1));
        }
        CHECK(owner.objectCount()==4 && driver.devices.size()==2);
        driver.requireVmStop=driver.globalDevicePhase=true;
        bool early=false;try { owner.teardownAfterVmExit(false); } catch(const std::exception&) { early=true; }
        CHECK(early && !driver.destroyed && owner.objectCount()==4);
        driver.vmStopped=true;
        CHECK(owner.teardownAfterVmExit(true) && driver.live.empty() && driver.destroyed==4 && !owner.objectCount());
    }
    for(unsigned stage=1;stage<=3;++stage) {
        NativeMock broken;broken.badStage=stage;cg::Runtime owner(broken);Control client{owner};bool threw=false;
        try { (void)client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}})); } catch(const std::exception&) { threw=true; }
        CHECK(threw && owner.failed() && owner.dispatch({}).empty() && broken.glCalls==1 && broken.syncCalls==(stage>=2 ? 1u : 0u) && broken.resourceCalls==(stage>=3 ? 1u : 0u));
    }
    for(unsigned mutation=0;mutation<3;++mutation) {
        NativeMock driver;
        if(mutation==0) driver.caps.vendor=0x8086;
        if(mutation==1) driver.caps.version=2;
        if(mutation==2) driver.caps.flags&=~ExpandedVendorCpuCapability;
        bool threw=false;
        try { CoordinatedProfiles invalid(driver,GuestOpenGlDesktopPresentationCapacity); } catch(const std::exception&) { threw=true; }
        CHECK(threw && !driver.glCalls && !driver.syncCalls && !driver.resourceCalls);
    }
    {
        NativeMock broken;broken.badStage=4;cg::Runtime owner(broken);Control client{owner};bool threw=false;
        try { client.control(cg::Operation::Initialize,encoded(cg::Initialization{GuestOpenGlDesktopPresentationCapacity,{}})); } catch(const std::exception&) { threw=true; }
        CHECK(threw && owner.failed() && broken.objectCalls==1 && !broken.syncCalls && !broken.resourceCalls);
    }
    {
        NativeMock driver,other;CoordinatedProfiles group(driver,GuestOpenGlDesktopPresentationCapacity);bool wrong=false;
        try { Session invalid(other,group); } catch(const std::exception&) { wrong=true; }CHECK(wrong && !other.calls);
        Session session(driver,group);CHECK(!at<Header>(session.dispatch(hello())).status);
        CHECK(at<Header>(session.dispatch(inner(Op::BeginStreaming))).status==-95);
        CHECK(at<Header>(session.dispatch(inner(Op::BeginOpenGl,0,encoded(std::uint32_t{5})))).status==-95);
        ++driver.caps.device;CHECK(at<Header>(session.dispatch(hello())).status==-95);
    }
    for(unsigned mutation=0;mutation<7;++mutation) {
        NativeMock driver;cg::Runtime owner(driver);cg::Frame h;h.operation=cg::Operation::Info;h.sequence=1;
        if(mutation==0) ++h.magic;
        if(mutation==1) ++h.version;
        if(mutation==2) ++h.sequence;
        if(mutation==3) ++h.bytes;
        if(mutation==4) h.status=-1;
        if(mutation==5) h.reserved=1;
        auto packet=encoded(h);if(mutation==6) packet.resize(31);(void)owner.dispatch(packet);
        CHECK(owner.failed() && !driver.glCalls && owner.dispatch(packet).empty());
    }
    std::cout<<"COORDINATOR_CONTROL exactCapacity=true oneNativeNegotiation=true independentSessions=true singlePresentationOwner=true drainingCharged=true partialInitializationFatal=true native=mock gpu=false\n";
}
