// SPDX-License-Identifier: MIT
// Native receipts and kernel memory are explicit mocks. Real Unix credentials,
// descriptor passing, mmap protection and process lifetime are not mocked.
#include "guest_broker_server.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <grp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
using namespace guest_broker;
using namespace driver_bridge;
static std::atomic<unsigned> checks{0};
#define CHECK(c) do { ++checks; if (!(c)) throw std::runtime_error("Check failed: " #c); } while (0)
template<class F> static void denied(F f,int code) {
    bool rejected=false;
    try { f(); } catch (const std::system_error& e) { rejected=e.code().value()==code; }
    CHECK(rejected);
}
static std::vector<std::uint8_t> packet(Op op,std::uint32_t object=0,const std::vector<std::uint8_t>& body={}) {
    auto data=encode(Header{static_cast<std::uint32_t>(op),object,0,0}); data.insert(data.end(),body.begin(),body.end()); return data;
}
static void cpuCapacityPolicy() {
    struct Authority {
        wddm_aperture_caps cap{1,WDDM_APERTURE_DESKTOP_CPU_ENABLED,WDDM_APERTURE_DESKTOP_CPU_LIMIT,{0,0}};
        wddm_aperture_info layout{1,256,256,0,256*4096,256*1048576,1048576,0,0,0,0};
        unsigned queries=0;
        wddm_aperture_desktop_fences fences{1,1,512,4096,2097152,0};
        wddm_aperture_coordinator_caps leases{1,1,768,0};
        wddm_aperture_desktop_slots slots{1,1,1024,1048576,1073741824,0};
        wddm_aperture_desktop_slots desktopSlotCapacity() { return slots; }
        wddm_aperture_desktop_fences desktopFenceCapacity() { return fences; }
        wddm_aperture_coordinator_caps coordinatedCapacity() { return leases; }
        wddm_aperture_caps capacity() { ++queries;return cap; }
        wddm_aperture_info info() { return layout; }
    } kernel;
    const auto begin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktop4kCapacity));
    verifyCpuCapacityProfile(begin,kernel); CHECK(kernel.queries==1);
    denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(std::uint32_t{7})),kernel);},EPROTO);
    for(unsigned n=0;n<8;++n) {
        auto broken=kernel;
        if(n==0) broken.cap={1,0,WDDM_APERTURE_CPU_LIMIT,{0,0}};
        if(n==1) broken.cap.cpu_byte_limit+=4096;
        if(n==2) broken.cap.flags=3;
        if(n==3) broken.cap.reserved[0]=1;
        if(n==4) broken.cap.reserved[1]=1;
        if(n==5) broken.layout.allocation_slots=128;
        if(n==6) broken.layout.fence_slots=128;
        if(n==7) broken.layout.version=2;
        denied([&]{verifyCpuCapacityProfile(begin,broken);},EPROTO);
    }
    for(unsigned field=0;field<8;++field) {
        auto contract=GuestOpenGlDesktop4kCapacity;
        std::uint32_t words[8]{};std::memcpy(words,&contract,sizeof words);++words[field];
        std::memcpy(&contract,words,sizeof words);
        Views views;
        denied([&]{views.observe(packet(Op::BeginOpenGl,0,encode(contract)),packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktop4kCpuProfile)));},EPROTO);
    }
    verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(std::uint32_t{6})),kernel);
    CHECK(kernel.queries==1); // Older profiles require no new kernel ABI.
    auto sparse=kernel; sparse.cap.flags=3;sparse.layout.allocation_slots=512;sparse.layout.allocation_bytes=512ull*1048576;
    const auto sparseBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopSparseCapacity));
    verifyCpuCapacityProfile(sparseBegin,sparse);
    denied([&]{verifyCpuCapacityProfile(sparseBegin,kernel);},EPROTO);
    denied([&]{verifyCpuCapacityProfile(begin,sparse);},EPROTO);
    denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(std::uint32_t{8})),sparse);},EPROTO);
    for(unsigned field=0;field<8;++field) {
        auto contract=GuestOpenGlDesktopSparseCapacity;
        std::uint32_t words[8]{};std::memcpy(words,&contract,sizeof words);++words[field];std::memcpy(&contract,words,sizeof words);
        denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(contract)),sparse);},EPROTO);
    }
    const auto largeBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopLargeMapCapacity));
    verifyCpuCapacityProfile(largeBegin,sparse);
    denied([&]{verifyCpuCapacityProfile(largeBegin,kernel);},EPROTO);
    auto undersized=GuestOpenGlDesktopSparseCapacity;undersized.version=9;
    denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(undersized)),sparse);},EPROTO);
    for(unsigned field=0;field<10;++field) {
        auto capacity=GuestOpenGlDesktopLargeMapCapacity;
        std::uint32_t words[10]{};std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(capacity)),sparse);},EPROTO);
    }
    const auto presentationBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopPresentationCapacity));
    verifyCpuCapacityProfile(presentationBegin,sparse);
    denied([&]{verifyCpuCapacityProfile(presentationBegin,kernel);},EPROTO);
    denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(std::uint32_t{10})),sparse);},EPROTO);
    auto oldMap=GuestOpenGlDesktopLargeMapCapacity;oldMap.aperture.version=10;
    denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(oldMap)),sparse);},EPROTO);
    for(unsigned field=0;field<12;++field) {
        auto capacity=GuestOpenGlDesktopPresentationCapacity;
        std::uint32_t words[12]{};std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        const auto malformed=packet(Op::BeginOpenGl,0,encode(capacity));
        denied([&]{verifyCpuCapacityProfile(malformed,sparse);},EPROTO);
        Views views;denied([&]{views.observe(malformed,packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopPresentationProfile)));},EPROTO);
    }
    std::cout<<"BROKER_PRESENTATION_BUDGET fixed48ByteReceipt=true exact1GiBUsage=true malformedRejected=true kernel=mock gpu=false\n";
    auto wide=sparse;wide.layout.fence_slots=512;wide.layout.fence_bytes=2097152;
    const auto wideBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopFenceCapacity));
    verifyCpuCapacityProfile(wideBegin,wide);
    denied([&]{verifyCpuCapacityProfile(wideBegin,sparse);},EPROTO);
    denied([&]{verifyCpuCapacityProfile(presentationBegin,wide);},EPROTO);
    for(unsigned n=0;n<9;++n) {
        auto bad=wide;
        if(n==0) bad.fences.enabled=0;
        if(n==1) bad.fences.slots=256;
        if(n==2) bad.fences.bytes=1048576;
        if(n==3) bad.fences.stride=8192;
        if(n==4) bad.fences.version=2;
        if(n==5) bad.fences.reserved=1;
        if(n==6) bad.leases.enabled=0;
        if(n==7) bad.leases.lease_limit=512;
        if(n==8) bad.leases.reserved=1;
        denied([&]{verifyCpuCapacityProfile(wideBegin,bad);},EPROTO);
    }
    for(unsigned field=0;field<12;++field) {
        auto capacity=GuestOpenGlDesktopFenceCapacity;std::uint32_t words[12]{};
        std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        denied([&]{verifyCpuCapacityProfile(packet(Op::BeginOpenGl,0,encode(capacity)),wide);},EPROTO);
    }
    std::cout<<"BROKER_DESKTOP_FENCES exact512Pages=true actualControllerReceiptRequired=true v10LayoutUnchanged=true kernel=mock gpu=false\n";
    auto expanded=wide;expanded.layout.allocation_slots=1024;expanded.layout.allocation_bytes=1073741824;
    const auto expandedBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopSlotCapacity));
    verifyCpuCapacityProfile(expandedBegin,expanded);
    denied([&]{verifyCpuCapacityProfile(expandedBegin,wide);},EPROTO);
    denied([&]{verifyCpuCapacityProfile(wideBegin,expanded);},EPROTO);
    for(unsigned n=0;n<10;++n) {
        auto bad=expanded;
        if(n==0) bad.slots.enabled=0;
        if(n==1) bad.slots.slots=512;
        if(n==2) bad.slots.bytes=536870912;
        if(n==3) bad.slots.stride=4096;
        if(n==4) bad.slots.version=2;
        if(n==5) bad.slots.reserved=1;
        if(n==6) bad.layout.allocation_bytes=536870912;
        if(n==7) bad.fences.enabled=0;
        if(n==8) bad.leases.enabled=0;
        if(n==9) bad.cap.cpu_byte_limit+=4096;
        denied([&]{verifyCpuCapacityProfile(expandedBegin,bad);},EPROTO);
    }
    Views expandedViews;
    expandedViews.observe(expandedBegin,packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopSlotProfile)));
    for(unsigned field=0;field<12;++field) {
        auto capacity=GuestOpenGlDesktopSlotCapacity;std::uint32_t words[12]{};
        std::memcpy(words,&capacity,sizeof words);
        if(field==0) words[field]=UINT32_MAX;else ++words[field];
        std::memcpy(&capacity,words,sizeof words);
        const auto malformed=packet(Op::BeginOpenGl,0,encode(capacity));
        denied([&]{verifyCpuCapacityProfile(malformed,expanded);},EPROTO);
        Views views;denied([&]{views.observe(malformed,packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopSlotProfile)));},EPROTO);
    }
    for(const auto profile:{GuestOpenGlDesktopPresentationProfile,GuestOpenGlDesktopFenceProfile}) {
        Views views;denied([&]{views.observe(expandedBegin,packet(Op::BeginOpenGl,0,encode(profile)));},EPROTO);
    }
    std::cout<<"BROKER_DESKTOP_SLOTS exact1024Slots=true actualControllerReceiptRequired=true cpu192MiBUnchanged=true v11LayoutUnchanged=true kernel=mock gpu=false\n";
    const auto referenceBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopReferenceCapacity));
    verifyCpuCapacityProfile(referenceBegin,expanded);
    denied([&]{verifyCpuCapacityProfile(referenceBegin,wide);},EPROTO);
    Views referenceViews;
    referenceViews.observe(referenceBegin,packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopReferenceProfile)));
    for(unsigned field=0;field<12;++field) {
        auto capacity=GuestOpenGlDesktopReferenceCapacity;std::uint32_t words[12]{};
        std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        const auto malformed=packet(Op::BeginOpenGl,0,encode(capacity));
        denied([&]{verifyCpuCapacityProfile(malformed,expanded);},EPROTO);
        Views views;denied([&]{views.observe(malformed,packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopReferenceProfile)));},EPROTO);
    }
    for(const auto profile:{GuestOpenGlDesktopSlotProfile,GuestOpenGlDesktopFenceProfile}) {
        Views views;denied([&]{views.observe(referenceBegin,packet(Op::BeginOpenGl,0,encode(profile)));},EPROTO);
    }
    auto multiapp=expanded;multiapp.cap.flags=7;multiapp.cap.cpu_byte_limit=WDDM_APERTURE_MULTIAPP_CPU_LIMIT;
    multiapp.leases.lease_limit=WDDM_APERTURE_DESKTOP_LEASES;
    const auto multiappBegin=packet(Op::BeginOpenGl,0,encode(GuestOpenGlMultiappCapacity));
    verifyCpuCapacityProfile(multiappBegin,multiapp);
    Views multiappViews;multiappViews.observe(multiappBegin,packet(Op::BeginOpenGl,0,encode(GuestOpenGlMultiappProfile)));
    denied([&]{verifyCpuCapacityProfile(multiappBegin,expanded);},EPROTO);
    denied([&]{verifyCpuCapacityProfile(referenceBegin,multiapp);},EPROTO);
    auto oldLeases=multiapp;oldLeases.leases.lease_limit=WDDM_APERTURE_COORDINATED_LEASES;
    denied([&]{verifyCpuCapacityProfile(multiappBegin,oldLeases);},EPROTO);
    for(unsigned field=0;field<12;++field) {
        auto capacity=GuestOpenGlMultiappCapacity;std::uint32_t words[12]{};
        std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
        const auto malformed=packet(Op::BeginOpenGl,0,encode(capacity));
        denied([&]{verifyCpuCapacityProfile(malformed,multiapp);},EPROTO);
        Views views;denied([&]{views.observe(malformed,packet(Op::BeginOpenGl,0,encode(GuestOpenGlMultiappProfile)));},EPROTO);
    }
    for(const auto profile:{GuestOpenGlDesktopReferenceProfile,GuestOpenGlDesktopSlotProfile}) {
        Views views;denied([&]{views.observe(multiappBegin,packet(Op::BeginOpenGl,0,encode(profile)));},EPROTO);
    }
    const auto memoryBegin=packet(Op::BeginOpenGl,0,encode(GuestApplicationMemoryCapacity));
    const auto memoryReply=packet(Op::BeginOpenGl,0,encode(GuestOpenGlMultiappProfile));
    denied([&]{verifyCpuCapacityProfile(memoryBegin,multiapp);},EPROTO);
    { Views views;denied([&]{views.observe(memoryBegin,memoryReply);},EPROTO); }
    coordinator::Info memory{};memory.capacity=GuestApplicationMemoryCapacity;
    memory.objects=GuestCoordinatedMultiappObjects;memory.gl=GuestOpenGlMultiappProfile;
    memory.sync=GuestSyncSharingProfile;memory.resource=GuestApplicationResourceSharingProfile;
    memory.caps={driver_bridge::Version,0,0x10de,0};memory.maxActors=8;memory.flags=7;
    CHECK(coordinator::matchesApplicationMemoryCapacity(GuestApplicationMemoryCapacity,memory));
    verifyCpuCapacityProfile(memoryBegin,multiapp,&memory);
    { Views views;views.observe(memoryBegin,memoryReply,&memory); }
    denied([&]{verifyCpuCapacityProfile(memoryBegin,expanded,&memory);},EPROTO);
    for(unsigned mutation=0;mutation<5;++mutation) {
        auto forged=memory;
        if(mutation==0) forged.flags=3;
        if(mutation==1) forged.capacity.reportedGpuByteLimit=OpenGlDesktopPresentationReportedBytes;
        if(mutation==2) forged.maxActors=4;
        if(mutation==3) ++forged.reserved;
        if(mutation==4) forged.resource=GuestResourceSharingProfile;
        CHECK(!coordinator::matchesApplicationMemoryCapacity(GuestApplicationMemoryCapacity,forged));
        denied([&]{verifyCpuCapacityProfile(memoryBegin,multiapp,&forged);},EPROTO);
        Views views;denied([&]{views.observe(memoryBegin,memoryReply,&forged);},EPROTO);
    }
    std::cout<<"BROKER_APPLICATION_MEMORY authenticatedAgreementRequired=true clientBytesInsufficient=true cpuKernelReceiptStillRequired=true malformedAndMismatchedRejected=true mock=true gpu=false\n";
    std::cout<<"BROKER_MULTIAPP_PROFILE exact14=true cpu384MiB=true legacy192MiBUnchanged=true malformedExtentRejected=true kernel=mock gpu=false\n";
    std::cout<<"BROKER_REFERENCE_PROFILE exact13=true actualCpuAndFenceReceiptsRequired=true olderProfilesUnchanged=true kernel=mock gpu=false\n";
    std::cout<<"BROKER_LARGE_MAP exact1GiB=true fixed40ByteReceipt=true malformedRejected=true oldProfilesUnchanged=true kernel=mock gpu=false\n";
    std::cout<<"BROKER_SPARSE_CAPACITY exact512Slots=true oldProfileRejected=true oldKernelRejected=true kernel=mock gpu=false\n";
    std::cout<<"BROKER_CPU_CAPACITY exact192MiB=true ownKernelQuery=true missingAndMalformedRejected=true legacyUnchanged=true kernel=mock gpu=false\n";
}
static void profileObjectBudgets() {
    for(const auto profile:{GuestOpenGlFenceProfile,GuestOpenGlCompositorProfile,
                            GuestOpenGlCompositorRenderProfile,GuestOpenGlDesktopProfile,GuestOpenGlDesktop4kProfile,GuestOpenGlDesktop4kCpuProfile,GuestOpenGlDesktopSparseProfile,GuestOpenGlDesktopLargeMapProfile,GuestOpenGlDesktopPresentationProfile}) {
        Views views;
        const auto request=packet(Op::BeginOpenGl,0,profile.version==10 ? encode(GuestOpenGlDesktopPresentationCapacity) : profile.version==9 ? encode(GuestOpenGlDesktopLargeMapCapacity) : profile.version==8 ? encode(GuestOpenGlDesktopSparseCapacity) : profile.version==7 ? encode(GuestOpenGlDesktop4kCapacity) : encode(profile.version));
        auto malformed=profile; ++malformed.objectLimit;
        denied([&]{views.observe(request,packet(Op::BeginOpenGl,0,encode(malformed)));},EPROTO);
        views.observe(request,packet(Op::BeginOpenGl,0,encode(profile)));
        denied([&]{views.observe(request,packet(Op::BeginOpenGl,0,encode(profile)));},EPROTO);
        // The same exact quota accepted by the host applies to authenticated
        // native creation receipts, including budgets above the old 512 cap.
        views.observe(packet(Op::OpenAdapter),packet(Op::OpenAdapter,1,encode(Reply{})));
        for(std::uint32_t id=2;id<=profile.objectLimit;++id)
            views.observe(packet(Op::CreateDevice,1),packet(Op::CreateDevice,id,encode(Reply{})));
        denied([&]{views.observe(packet(Op::CreateDevice,1),packet(Op::CreateDevice,profile.objectLimit+1,encode(Reply{})));},EPROTO);
        denied([&]{views.observe(packet(Op::CreateDevice,1),packet(Op::CreateDevice,2,encode(Reply{})));},EPROTO);
        views.observe(packet(Op::DestroyDevice,2),packet(Op::DestroyDevice,2,encode(Reply{})));
        views.observe(packet(Op::CreateDevice,1),packet(Op::CreateDevice,profile.objectLimit+1,encode(Reply{})));
        CHECK(views.active()==0);
    }
}
static std::vector<std::uint8_t> receipt(Op op,std::uint32_t object,std::uint64_t value=0,
                                       const std::vector<std::uint8_t>& body={},std::int32_t status=0) {
    auto data=packet(op,object,encode(Reply{status,0,value})); data.insert(data.end(),body.begin(),body.end()); return data;
}
static Views base() {
    Views views;
    views.observe(packet(Op::OpenAdapter),receipt(Op::OpenAdapter,1));
    views.observe(packet(Op::CreateDevice,1),receipt(Op::CreateDevice,2));
    return views;
}
static void paging(Views& views) {
    views.observe(packet(Op::CreateGuestPagingQueue,2),receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,4104})));
}
static void viewPolicy() {
    {
        Views wide;
        wide.observe(packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopFenceCapacity)),packet(Op::BeginOpenGl,0,encode(GuestOpenGlDesktopFenceProfile)));
        wide.observe(packet(Op::OpenAdapter),receipt(Op::OpenAdapter,1));wide.observe(packet(Op::CreateDevice,1),receipt(Op::CreateDevice,2));
        wide.observe(packet(Op::CreateGuestPagingQueue,2),receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,DesktopFenceApertureBytes-8})));
        CHECK(wide.acquire({WDDM_APERTURE_FENCE,0,511*4096ull,4096,0}).cookie==0);
        auto old=base();denied([&]{old.observe(packet(Op::CreateGuestPagingQueue,2),receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,DesktopFenceApertureBytes-8})));},EPROTO);
        auto beyond=base();denied([&]{beyond.observe(packet(Op::CreateGuestPagingQueue,2),receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,DesktopFenceApertureBytes})));},EPROTO);
    }
    const ViewRequest fence{WDDM_APERTURE_FENCE,0,4096,4096,0};
    auto views=base(); denied([&]{views.acquire(fence);},EPERM); paging(views);
    views.observe(packet(Op::MapVendorAllocation,8),receipt(Op::MapVendorAllocation,8,0,{},0x103));
    CHECK(views.acquire(fence).cookie==0);
    for (unsigned field=0;field<5;++field) {
        auto invalid=fence;
        if (field==0) invalid.kind=WDDM_APERTURE_ALLOCATION;
        if (field==1) invalid.offset=8192;
        if (field==2) invalid.bytes=8192;
        if (field==3) invalid.generation=9;
        if (field==4) invalid.reserved=1;
        denied([&]{views.acquire(invalid);},field==4 ? EPROTO : EPERM);
    }
    views.acquire(fence).cookie=8; CHECK(views.active()==1);
    views.syncFile({8,8,57});
    CHECK(views.syncFile({8,8,0},true)==3);
    denied([&]{views.syncFile({8,0,0},true);},EPERM);
    for (const auto invalid: {SyncFileRequest{8,0,57},SyncFileRequest{8,16,57},SyncFileRequest{8,8,0},SyncFileRequest{8,8,UINT64_MAX}})
        denied([&]{views.syncFile(invalid);},EPERM);
    denied([&]{views.syncFile({9,8,57});},EBADF);
    denied([&]{views.acquire(fence);},EBUSY);
    denied([&]{views.cookie(0);},EBADF); denied([&]{views.cookie(9);},EBADF);
    for (auto op : {Op::DestroyPagingQueue,Op::DestroyDevice,Op::CloseAdapter})
        denied([&]{views.approve(packet(op,op==Op::DestroyPagingQueue ? 3 : op==Op::DestroyDevice ? 2 : 1));},EBUSY);
    views.cookie(8).cookie=0; CHECK(views.active()==0); views.approve(packet(Op::DestroyPagingQueue,3));
    views.observe(packet(Op::DestroyPagingQueue,3),receipt(Op::DestroyPagingQueue,3,0,{},static_cast<std::int32_t>(0xc0000001u)));
    CHECK(views.acquire(fence).cookie==0);
    views.observe(packet(Op::DestroyPagingQueue,3),receipt(Op::DestroyPagingQueue,3));
    denied([&]{views.acquire(fence);},EPERM);
    {
        auto queued=base(); queued.observe(packet(Op::CreateContext,2),receipt(Op::CreateContext,3));
        auto desc=encode(HwQueueDesc{0,4,0,0}); desc.insert(desc.end(),{1,2,3,4});
        auto body=encode(HwQueueReply{5,0,8192,65536}); body.insert(body.end(),{9,8,7,6});
        const auto request=packet(Op::CreateHwQueue,3,desc), reply=receipt(Op::CreateHwQueue,4,0,body);
        auto truncated=reply; truncated.pop_back();
        denied([&]{queued.observe(request,truncated);},EPROTO);
        queued.observe(request,reply);
        CHECK(queued.acquire({WDDM_APERTURE_FENCE,0,8192,4096,0}).cookie==0);
    }
    for (unsigned corruption=0;corruption<6;++corruption) {
        auto invalid=base(); auto reply=receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,4096}));
        if (corruption==0) reply[0]^=1;
        if (corruption==1) reply.resize(sizeof(Header));
        if (corruption==2) reply=receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,1,4096}));
        if (corruption==3) reply=receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,FenceApertureBytes}));
        if (corruption==4) reply=receipt(Op::CreateGuestPagingQueue,2,0,encode(PagingReply{4,0,4096}));
        if (corruption==5) reply=receipt(Op::CreateGuestPagingQueue,3,0,encode(PagingReply{4,0,4096}),0x103);
        denied([&]{invalid.observe(packet(Op::CreateGuestPagingQueue,2),reply);},EPROTO);
    }
    for (const auto creating : {Op::CreateVendorResourceAllocation, Op::CreateNativeSharedGpuBufferAllocation}) {
        auto allocation=base();
        allocation.observe(packet(creating,2),receipt(creating,5,0,encode(VendorResourceReply{6,0})));
        const auto lock=packet(Op::LockVendorAllocation,5,encode(VendorCpuDesc{2,0}));
        const ViewRequest cpu{WDDM_APERTURE_ALLOCATION,0,1048576,4096,77};
        allocation.observe(lock,receipt(Op::LockVendorAllocation,5,0,encode(VendorCpuReply{}),static_cast<std::int32_t>(0xc0000017u)));
        denied([&]{allocation.acquire(cpu);},EPERM);
        allocation.observe(lock,receipt(Op::LockVendorAllocation,5,cpu.offset,encode(VendorCpuReply{4096,0,77})));
        allocation.acquire(cpu).cookie=9;
        denied([&]{allocation.syncFile({9,0,1});},EPERM);
        denied([&]{allocation.approve(packet(Op::UnlockVendorAllocation,5,encode(VendorCpuDesc{2,0})));},EBUSY);
        denied([&]{allocation.approve(packet(Op::DestroyVendorResource,6,encode(std::uint32_t{2})));},EBUSY);
        auto destroy=encode(DestroyVendorDesc{1,0}); const auto id=encode(std::uint32_t{5}); destroy.insert(destroy.end(),id.begin(),id.end());
        denied([&]{allocation.approve(packet(Op::DestroyVendorAllocations,2,destroy));},EBUSY);
        allocation.cookie(9).cookie=0;
        allocation.observe(packet(Op::UnlockVendorAllocation,5,encode(VendorCpuDesc{2,0})),receipt(Op::UnlockVendorAllocation,5));
        denied([&]{allocation.acquire(cpu);},EPERM);
    }
    std::cout<<"BROKER_VIEW_POLICY checks="<<checks.load()<<" nativeReceipts=mock kernel=false gpu=false\n";
}
struct NativeMock {
    unsigned calls=0;
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& bytes) {
        ++calls; Header h{}; std::memcpy(&h,bytes.data(),sizeof(h));
        const auto op=static_cast<Op>(h.type);
        if (op==Op::Hello) return packet(op,0,encode(native_gpu::Capabilities{driver_bridge::Version,ContextCapability|QueryCapability|GuestPagingCapability,0x10de,11352}));
        if (op==Op::OpenAdapter) return receipt(op,1);
        if (op==Op::CreateDevice && h.handle==1) return receipt(op,2);
        if (op==Op::CreateGuestPagingQueue && h.handle==2) return receipt(op,3,0,encode(PagingReply{4,0,4104}));
        if (op==Op::DestroyPagingQueue && h.handle==3) return receipt(op,3);
        throw std::runtime_error("Unexpected mock native operation");
    }
};
struct KernelMock {
    Fd nativeSyncFile(SyncFileRequest,int) { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No native kernel in portable test"); }
    Fd syncFile(SyncFileRequest) { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No kernel in portable test"); }
    wddm_sync_file_stats syncFileInfo() { return {1,0,0,0,0,0,0,0}; }
    unsigned created=0,released=0;
    void close() {}
    wddm_aperture_desktop_slots desktopSlotCapacity() { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No desktop slot kernel in portable test"); }
    wddm_aperture_desktop_fences desktopFenceCapacity() { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No desktop fence kernel in portable test"); }
    wddm_aperture_coordinator_caps coordinatedCapacity() { throw std::system_error(EOPNOTSUPP,std::generic_category(),"No coordinator kernel in portable test"); }
    wddm_aperture_caps capacity() { return {1,0,WDDM_APERTURE_CPU_LIMIT,{0,0}}; }
    wddm_aperture_info info() { return {1,128,256,0,128*4096,256*1048576,1048576,0,0,0,0}; }
    std::pair<Fd,std::uint64_t> acquire(ViewRequest view) {
        CHECK(view.kind==WDDM_APERTURE_FENCE && view.offset==4096 && view.bytes==4096 && !view.generation);
        Fd file(static_cast<int>(syscall(SYS_memfd_create,"broker-mock-view",MFD_CLOEXEC)));
        CHECK(file.get()>=0 && !ftruncate(file.get(),4096));
        const std::uint64_t marker=57; CHECK(pwrite(file.get(),&marker,sizeof(marker),8)==sizeof(marker));
        const auto name="/proc/self/fd/"+std::to_string(file.get()); Fd readOnly(open(name.c_str(),O_RDONLY|O_CLOEXEC));
        CHECK(readOnly.get()>=0); ++created; return {std::move(readOnly),19};
    }
    void release(std::uint64_t cookie) { CHECK(cookie==19); ++released; }
};
static void realRpc(bool attack,const char* probe=nullptr,const char* shim=nullptr,bool expectTimeout=false) {
    char directory[]="/tmp/wddm-broker-routing-XXXXXX"; CHECK(mkdtemp(directory)); CHECK(!chmod(directory,0755));
    const auto path=std::string(directory)+"/broker.sock";
    sockaddr_un address{}; address.sun_family=AF_UNIX; std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0)); const int enabled=1;
    CHECK(listener.get()>=0 && !setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof(enabled)));
    CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)));
    CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),1));
    int commands[2],notices[2]; CHECK(!pipe2(commands,O_CLOEXEC) && !pipe2(notices,O_CLOEXEC));
    const auto child=fork(); CHECK(child>=0);
    if (!child) {
        try {
            close(listener.get()); close(commands[1]); close(notices[0]);
            CHECK(!setgroups(0,nullptr) && !setgid(65534) && !setuid(65534));
            setenv("WDDM_BRIDGE_BROKER_SOCKET",path.c_str(),1);
            if (probe) {
                unsetenv("WDDM_BRIDGE_WINDOWS_WORKER"); unsetenv("WDDM_BRIDGE_PORT");
                setenv("LD_PRELOAD",shim,1); setenv("WDDM_BRIDGE_KERNEL_APERTURE","1",1);
                execl(probe,probe,(char*)nullptr); throw std::runtime_error("Shim probe exec failed");
            }
            Client client;
            CHECK(client.info().allocation_slots==256);
            CHECK(client.capacity().cpu_byte_limit==WDDM_APERTURE_CPU_LIMIT);
            client.driver(packet(Op::OpenAdapter)); client.driver(packet(Op::CreateDevice,1)); client.driver(packet(Op::CreateGuestPagingQueue,2));
            auto reply=client.acquire({WDDM_APERTURE_FENCE,0,4096,4096,0});
            CHECK(reply.fds.size()==1 && (fcntl(reply.fds[0].get(),F_GETFD)&FD_CLOEXEC));
            CHECK(mmap(nullptr,4096,PROT_READ|PROT_WRITE,MAP_SHARED,reply.fds[0].get(),0)==MAP_FAILED && errno==EACCES);
            auto* page=static_cast<std::uint64_t*>(mmap(nullptr,4096,PROT_READ,MAP_SHARED,reply.fds[0].get(),0));
            CHECK(page!=MAP_FAILED && page[1]==57);
            denied([&]{client.syncFile({decode<ViewReply>(reply.payload).cookie,8,58});},EOPNOTSUPP);
            CHECK(client.syncFileInfo().held==0 && client.info().fence_slots==128);
            if (attack) denied([&]{client.acquire({WDDM_APERTURE_ALLOCATION,0,0,4096,1});},EPERM);
            client.disconnect(); const char marker='C'; CHECK(write(notices[1],&marker,1)==1);
            char command=0; CHECK(read(commands[0],&command,1)==1 && command=='E');
            CHECK(page[1]==57); CHECK(!munmap(page,4096)); _exit(0);
        } catch (const std::exception& e) { dprintf(2,"RPC client failed: %s\n",e.what()); _exit(1); }
    }
    close(commands[0]); close(notices[1]);
    Fd accepted(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK)); CHECK(accepted.get()>=0);
    Peer peer(std::move(accepted),65534,65534); NativeMock native; KernelMock kernel;
    std::atomic<bool> done{false}; int result=-1;
    std::thread server([&]{result=serve(peer,native,kernel);done.store(true);});
    int status=0; bool timeout=false;
    if (probe) {
        pid_t waited=0;
        for (unsigned n=0;n<100 && !waited;++n) { waited=waitpid(child,&status,WNOHANG); if (!waited) poll(nullptr,0,50); }
        if (!waited) { timeout=true; CHECK(!kill(child,SIGKILL)); waited=waitpid(child,&status,0); }
        CHECK(waited==child);
    } else {
        char notice=0; CHECK(read(notices[0],&notice,1)==1 && notice=='C');
        CHECK(!done.load() && !peer.processExited()); const char exit='E'; CHECK(write(commands[1],&exit,1)==1);
        CHECK(waitpid(child,&status,0)==child);
    }
    server.join();
    if (expectTimeout) {
        CHECK(timeout && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL && result==0 && native.calls==4 && kernel.created==1 && kernel.released==0);
    } else {
        CHECK(!timeout && WIFEXITED(status) && !WEXITSTATUS(status));
        CHECK(result==(attack ? 1 : 0) && native.calls==(probe ? 5u : 3u) && kernel.created==1 && kernel.released==(probe ? 1u : 0u));
    }
    CHECK(peer.processExited());
    close(commands[1]); close(notices[0]); CHECK(!unlink(path.c_str()) && !rmdir(directory));
    if (probe) std::cout<<"BROKER_SHIM_CONTRACT clientUid=65534 oldDeadlockControl="<<(expectTimeout?"true":"false")<<" native=mock kernel=mock gpu=false\n";
    else std::cout<<"BROKER_RPC_CONTRACT clientUid=65534 readonlyFd=true retainedAfterEof=true attack="<<(attack?"rejected":"none")<<" native=mock kernel=mock gpu=false\n";
}
static void replyDisconnect() {
    char directory[]="/tmp/wddm-broker-reply-XXXXXX"; CHECK(mkdtemp(directory)); CHECK(!chmod(directory,0755));
    const auto path=std::string(directory)+"/broker.sock";
    sockaddr_un address{}; address.sun_family=AF_UNIX; std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0)); const int enabled=1;
    CHECK(listener.get()>=0 && !setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled));
    CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof address));
    CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),1));
    int notices[2],commands[2]; CHECK(!pipe2(notices,O_CLOEXEC) && !pipe2(commands,O_CLOEXEC));
    const auto child=fork(); CHECK(child>=0);
    if(!child) {
        try {
            close(listener.get()); close(notices[0]); close(commands[1]);
            CHECK(!setgroups(0,nullptr) && !setgid(65534) && !setuid(65534));
            Fd connection(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));
            CHECK(connection.get()>=0 && !connect(connection.get(),reinterpret_cast<sockaddr*>(&address),sizeof address));
            const auto request=packet(Op::OpenAdapter);
            Frame frame; frame.operation=Operation::DriverCall; frame.bytes=static_cast<std::uint32_t>(request.size());
            std::vector<std::uint8_t> data(sizeof frame+request.size());
            std::memcpy(data.data(),&frame,sizeof frame); std::memcpy(data.data()+sizeof frame,request.data(),request.size());
            CHECK(send(connection.get(),data.data(),data.size(),MSG_NOSIGNAL)==static_cast<ssize_t>(data.size()));
            connection=Fd(); const char closed='C'; CHECK(write(notices[1],&closed,1)==1);
            char finish=0; CHECK(read(commands[0],&finish,1)==1 && finish=='F'); _exit(0);
        } catch(const std::exception& e) { dprintf(2,"Reply disconnect client: %s\n",e.what()); _exit(1); }
    }
    close(notices[1]); close(commands[0]);
    Peer peer(Fd(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK)),65534,65534);
    struct DelayedNative : NativeMock {
        int notice; std::atomic<bool> completed{false};
        explicit DelayedNative(int fd):notice(fd) {}
        std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& bytes) {
            char closed=0; CHECK(read(notice,&closed,1)==1 && closed=='C');
            auto result=NativeMock::exchange(bytes); completed=true; return result;
        }
    } native(notices[0]);
    KernelMock kernel; std::atomic<bool> done{false}; int result=-1;
    std::thread server([&]{result=serve(peer,native,kernel); done=true;});
    for(unsigned n=0;n<100 && !native.completed;++n) poll(nullptr,0,10);
    CHECK(native.completed && !done && !peer.processExited());
    const char finish='F'; CHECK(write(commands[1],&finish,1)==1);
    int status=0; CHECK(waitpid(child,&status,0)==child); server.join();
    CHECK(WIFEXITED(status) && !WEXITSTATUS(status) && result==0 && native.calls==1 && peer.processExited());
    close(commands[1]); close(notices[0]); CHECK(!unlink(path.c_str()) && !rmdir(directory));
    std::cout<<"BROKER_REPLY_DISCONNECT validRequest=true replyLost=true pidfdExitRequired=true native=mock kernel=mock gpu=false\n";
}
int main(int argc,char** argv) {
    try {
        if (argc!=1 && argc!=3 && (argc!=4 || std::strcmp(argv[3],"--expect-close-deadlock"))) throw std::runtime_error("Use optional PROBE SHIM [--expect-close-deadlock]");
        viewPolicy();
        profileObjectBudgets();
        cpuCapacityPolicy();
        if (geteuid()) { std::cout<<"BROKER_RPC_CONTRACT skipped=root-required\n"; return 0; }
        alarm(25); realRpc(false); realRpc(true); replyDisconnect();
        if (argc>=3) realRpc(false,argv[1],argv[2],argc==4);
        alarm(0); return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
