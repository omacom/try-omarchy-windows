// SPDX-License-Identifier: MIT
#include "driver_wire.h"
#include "adapter_query_client.h"
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
using namespace driver_bridge;
#define require(...) do { if (!(__VA_ARGS__)) throw std::runtime_error("driver wire regression at line " + std::to_string(__LINE__)); } while (0)
static Header header(const std::vector<std::uint8_t>& p) {
    require(p.size() >= sizeof(Header)); Header h{}; std::memcpy(&h, p.data(), sizeof h); return h;
}
struct Fake : Driver {
    int calls = 0; std::uint32_t next = 500; bool fail = false, shortRead = false, badContextReply = false;
    Result executionState{0,0,1};std::uint32_t lastExecutionDevice=0;
    Result queryDeviceExecutionState(std::uint32_t device) override {
        require(device>500);++calls;lastExecutionDevice=device;return executionState;
    }
    bool contextsEnabled = true;
    int badContextPriorityReply = 0;
    Result setContextInProcessPriority(std::uint32_t context, std::int32_t priority) override {
        require(context > 500 && validContextPriority(priority)); ++calls;
        return {fail ? -123 : badContextPriorityReply == 1 ? 259 : 0,
                badContextPriorityReply == 2 ? 501u : 0u, badContextPriorityReply == 3 ? 1ull : 0ull};
    }
    bool queriesEnabled = true, badQueryReply = false;
    bool guestPagingEnabled = false;
    int badPagingReply = 0;
    bool vendorEnabled = false;
    bool expandedVendorObjectsEnabled = false;
    int badVendorDestroyReply = 0;
    bool gpuVaEnabled = false;
    bool residencyEnabled = false;
    bool memoryManagementEnabled = false, evictAttempted = true;
    bool mappedResidencyRetained = false;
    bool retainsMappedResidencyOnEvict() const override { return mappedResidencyRetained; }
    int badMemoryBudgetReply = 0, badEvictReply = 0;
    unsigned memoryBudgetCalls = 0, evictionCalls = 0;
    MemoryBudgetResult queryMemoryBudget(std::uint32_t adapter, MemoryBudgetDesc desc) override {
        require(adapter>500 && validMemoryBudget(desc)); ++calls; ++memoryBudgetCalls;
        const MemoryBudgetReply info{268435456,65536,0,268435456};
        return {{badMemoryBudgetReply==1?259:badMemoryBudgetReply==4?-123:0,
                 badMemoryBudgetReply==2?777u:0u,badMemoryBudgetReply==3?1ull:0ull},info};
    }
    EvictResult evictVendorAllocations(std::uint32_t device, EvictDesc desc, const std::vector<std::uint32_t>& handles) override {
        require(device>500 && validEvict(desc) && handles.size()==desc.count);
        for (const auto handle:handles) require(vendorOwners.at(handle)==device);
        ++calls; ++evictionCalls;
        return {{!evictAttempted?static_cast<std::int32_t>(0x80000011u):fail?-123:badEvictReply==1?259:0,
                 badEvictReply==2?777u:0u,fail?(badEvictReply==3?1ull:0ull):evictAttempted?65536ull:0ull},evictAttempted};
    }
    bool cpuEnabled = false;
    bool translationEnabled = false;
    int badTranslationReply = 0;
    std::uint32_t translationOverride = 0;
    std::vector<std::uint32_t> lastTranslationHandles;
    bool hwQueuesEnabled = false;
    bool noBroadcastHwQueuesEnabled = false;
    bool noBroadcastWaitHwQueuesEnabled = false;
    HwQueueDesc lastHwQueueDesc{};
    bool retirementEnabled = false;
    bool reservationEnabled = false;
    bool gpuStateEnabled = false;
    int badGpuStateReply = 0;
    GpuVaDesc lastGpuStateDesc{};
    int badReservationReply = 0, badReservationDestroyReply = 0;
    std::map<std::uint32_t, std::uint32_t> reservationParents;
    int badHwQueueReply = 0;
    std::map<std::uint32_t, std::uint32_t> hwQueueParents;
    bool syncEnabled = false;
    int badSyncReply = 0;
    SyncDesc lastSyncDesc{};
    std::map<std::uint32_t, std::uint32_t> syncParents;
    bool submitEnabled = false;
    bool asyncSubmitEnabled = false;
    int badSubmitReply = 0;
    unsigned submitCalls = 0;
    unsigned cpuLocks = 0, cpuUnlocks = 0;
    int badCpuReply = 0;
    int badResidentReply = 0;
    unsigned residentCalls = 0;
    std::vector<std::uint32_t> lastResidentPriorities, lastResidentHandles;
    int badGpuVaReply = 0;
    int badVendorReply = 0;
    std::map<std::uint32_t, std::uint32_t> vendorOwners;
    bool resourceEnabled = false;
    bool sharedResourceEnabled = false;
    bool consumeSharedEnabled = false;
    int badConsumeReply = 0;
    std::vector<std::uint8_t> lastSharedRuntime;
    bool nativeTextureEnabled = false;
    unsigned nativeTextureCalls = 0;
    bool nativeInteropEnabled=false;
    int badNativeInteropCaps=0;
    unsigned nativeInteropCalls=0;
    bool nativeGpuBufferEnabled = false;
    unsigned nativeGpuBufferCalls = 0;
    unsigned nativeGpuBufferAdmissionCalls = 0;
    NativeGpuBufferDesc lastGpuBufferAdmission{};
    Result nativeGpuBufferAdmissionResult{static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    Result queryNativeGpuBufferAdmission(NativeGpuBufferDesc desc) override {
        require(nativeGpuBufferEnabled && validNativeGpuBuffer(desc));
        ++nativeGpuBufferAdmissionCalls; lastGpuBufferAdmission = desc; return nativeGpuBufferAdmissionResult;
    }
    int badNativeGpuBufferCaps = 0;
    NativeGpuBufferCaps nativeGpuBufferCaps() const override {
        auto c = nativeGpuBufferEnabled ? NativeGpuBufferProfile : NativeGpuBufferCaps{};
        if (badNativeGpuBufferCaps == 1) ++c.version;
        if (badNativeGpuBufferCaps == 2) ++c.flags;
        if (badNativeGpuBufferCaps == 3) ++c.maxBytes;
        if (badNativeGpuBufferCaps == 4) ++c.resourceFlags;
        if (badNativeGpuBufferCaps == 5) ++c.reserved;
        return c;
    }
    VendorResourceResult createNativeSharedGpuBufferAllocation(std::uint32_t device, NativeSharedGpuBufferAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        require(nativeGpuBufferEnabled && validNativeSharedGpuBufferAllocation(desc)); ++nativeGpuBufferCalls;
        return createSharedVendorResourceAllocation(device, desc.shared, data);
    }
    NativeInteropCaps nativeInteropCaps() const override {
        auto c=nativeInteropEnabled ? NativeInteropProfile : NativeInteropCaps{};
        if(badNativeInteropCaps==1) c.reserved=1;
        if(badNativeInteropCaps==2) c.formats=1;
        if(badNativeInteropCaps==3) ++c.maxWidth;
        if(badNativeInteropCaps==4) ++c.maxLogicalBytes;
        if(badNativeInteropCaps==5) c.flags=3;
        if(badNativeInteropCaps==6) ++c.version;
        if(badNativeInteropCaps==7) c.maxHeight=0;
        return c;
    }
    VendorResourceResult createNativeInteropResource(std::uint32_t device,NativeSharedVendorAllocationDesc desc,std::vector<std::uint8_t>& data) override {
        require(nativeInteropEnabled && validNativeInteropAllocation(desc)); ++nativeInteropCalls;
        return createSharedVendorResourceAllocation(device,desc.shared,data);
    }
    bool resourceSharingSupported = true;
    int badResourceExport = 0;
    unsigned resourceExports = 0;
    int badResourceInfo = 0;
    int badResourceImport = 0;
    std::set<std::uint32_t> resourceTickets;
    ResourceSharingProfile beginResourceSharing() override {
        return resourceSharingSupported ? GuestResourceSharingProfile : ResourceSharingProfile{};
    }
    Result exportResource(std::uint32_t resource) override {
        require(std::any_of(vendorResources.begin(), vendorResources.end(),
            [resource](const auto& item) { return item.second == resource; }));
        ++resourceExports;
        if (fail) return {-123, 0, 0};
        const auto ticket = ++next; resourceTickets.insert(ticket);
        return {badResourceExport == 1 ? 259 : 0, ticket, badResourceExport == 2 ? 1ull : 0ull};
    }
    Result querySharedResourceInfo(std::uint32_t ticket, std::uint32_t device, SharedResourceInfo& info, std::vector<std::uint8_t>& runtime) override {
        require(resourceTickets.count(ticket) && device > 500);
        if (fail) return {-123, 0, 0};
        info = {4, 8, 0, 1}; runtime = {5,6,7,8};
        if (badResourceInfo == 1) info.allocations = 2;
        if (badResourceInfo == 2) info.runtimeBytes = MaxSharedResourceRuntimeBytes + 1;
        if (badResourceInfo == 3) runtime.pop_back();
        return {badResourceInfo == 4 ? 259 : 0, badResourceInfo == 5 ? 777u : 0u, badResourceInfo == 6 ? 1ull : 0ull};
    }
    SharedResourceOpenResult importResource(std::uint32_t ticket, SharedResourceOpenDesc desc, std::vector<std::uint8_t>& data) override {
        require(resourceTickets.count(ticket) && desc.device > 500 && desc.runtimeBytes == 4 && desc.driverBytes == 8);
        if (fail) return {{-123,0,0},{}};
        const auto allocation = ++next, resource = ++next;
        vendorOwners.emplace(allocation, desc.device); vendorResources.emplace(allocation, resource);
        std::fill(data.begin(), data.end(), std::uint8_t{42});
        SharedResourceOpenReply info{resource,8,0,0};
        if (badResourceImport == 2) info.resource = 0;
        if (badResourceImport == 3) info.resource = allocation;
        if (badResourceImport == 4) info.allocationDriverOffset = 9;
        if (badResourceImport == 5) info.allocationDriverBytes = 0;
        if (badResourceImport == 6) info.reserved = 1;
        if (badResourceImport == 7) data.pop_back();
        return {{badResourceImport == 1 ? 259 : 0, allocation, badResourceImport == 8 ? 1ull : 0ull}, info};
    }
    int badResourceReply = 0;
    bool contextSignalEnabled = false;
    int badContextSignalReply = 0;
    unsigned contextSignalCalls = 0;
    bool hwQueueSignalEnabled = false;
    int badHwQueueSignalReply = 0;
    unsigned hwQueueSignalCalls = 0;
    std::vector<std::uint32_t> lastSignalQueues;
    std::map<std::uint32_t, std::uint32_t> vendorResources;
    std::map<std::uint32_t, std::vector<std::uint8_t>> buffers;
    std::vector<Kind> destroyed;
    native_gpu::Capabilities capabilities() const override {
        return {1, 31u | (contextsEnabled ? ContextCapability : 0u) | (queriesEnabled ? QueryCapability : 0u) |
                (guestPagingEnabled ? GuestPagingCapability : 0u) | (vendorEnabled ? VendorAllocationCapability : 0u) |
                (resourceEnabled ? VendorResourceCapability : 0u) |
                (sharedResourceEnabled ? SharedVendorResourceCapability : 0u) |
                (nativeTextureEnabled ? NativeSharedTextureCapability : 0u) |
                (consumeSharedEnabled ? SharedTextureConsumeCapability : 0u) |
                (expandedVendorObjectsEnabled ? ExpandedVendorObjectsCapability : 0u) |
                (contextSignalEnabled ? ContextSignalCapability : 0u) |
                (hwQueueSignalEnabled ? HwQueueSignalCapability : 0u) |
                (gpuVaEnabled ? VendorGpuVaCapability : 0u) | (residencyEnabled ? VendorResidencyCapability : 0u) |
                (memoryManagementEnabled ? VendorMemoryManagementCapability : 0u) |
                (cpuEnabled ? VendorCpuCapability : 0u) | (translationEnabled ? VendorTranslationCapability : 0u) |
                (hwQueuesEnabled ? HwQueueCapability : 0u) | (syncEnabled ? SyncCapability : 0u) |
                (noBroadcastHwQueuesEnabled ? NoBroadcastSignalHwQueueCapability : 0u) |
                (noBroadcastWaitHwQueuesEnabled ? NoBroadcastWaitHwQueueCapability : 0u) |
                (submitEnabled ? HwSubmitCapability : 0u) | (retirementEnabled ? VendorRetirementCapability : 0u) |
                (asyncSubmitEnabled ? AsyncHwSubmitCapability : 0u) |
                (reservationEnabled ? GpuReservationCapability : 0u) | (gpuStateEnabled ? GpuStateCapability : 0u), 0x10de, 123};
    }
    GpuVaResult mapGpuState(std::uint32_t reservation, std::uint32_t queue, GpuVaDesc desc) override {
        require(reservationParents.count(reservation) && queue > 500 && validGpuState(desc)); ++calls;
        lastGpuStateDesc = desc;
        if (fail) return {{-123, 0, badGpuStateReply == 5 ? desc.base : 0}, badGpuStateReply == 6 ? 42ull : 0ull};
        return {{badGpuStateReply == 1 ? 1 : 259, badGpuStateReply == 2 ? 777u : 0u,
                 badGpuStateReply == 3 ? 0ull : badGpuStateReply == 4 ? desc.base + 4096 : desc.base}, 7019};
    }
    Result created() { ++calls; return {fail ? -123 : 0, ++next, 0}; }
    Result consumeSharedTexture(std::uint32_t resource, std::uint32_t sync, SharedTextureConsumeDesc desc) override {
        require(std::any_of(vendorResources.begin(),vendorResources.end(),[&](const auto& item) { return item.second == resource; }) &&
                syncParents.count(sync) && desc.sync == sync && validSharedTextureConsume(desc));
        ++calls;
        return {fail ? -123 : badConsumeReply == 1 ? 259 : 0,
                badConsumeReply == 2 ? 700u : 0u, fail ? (badConsumeReply == 3 ? 42ull : 0ull) : 42ull};
    }
    Result reserveGpuAddress(std::uint32_t adapter, GpuReservationDesc desc) override {
        require(adapter > 500 && validGpuReservation(desc)); ++calls;
        if (fail && badReservationReply != 10) return {-123, 0, badReservationReply == 9 ? 65536ull : 0ull};
        const auto id = badReservationReply == 2 ? 0u : ++next;
        if (id) reservationParents.emplace(id, adapter);
        const auto address = desc.base ? desc.base : desc.minimum + (next - 500) * MaxGpuReservationBytes;
        return {fail ? -123 : badReservationReply == 1 ? 259 : 0, id,
                badReservationReply == 3 ? 0ull : badReservationReply == 4 ? address + 1 :
                badReservationReply == 5 ? MaxGpuAddress : badReservationReply == 6 ? desc.minimum - 65536 :
                badReservationReply == 7 ? desc.maximum : badReservationReply == 8 ? address + 65536 : address};
    }
    Result openAdapter() override { return created(); }
    Result queryVersion(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 3200}; }
    Result queryAdapter(std::uint32_t h, QueryDesc desc, std::vector<std::uint8_t>& data) override {
        require(h > 500 && validQuery(desc) && data.size() == desc.bytes); ++calls;
        for (auto& byte : data) byte ^= 255;
        if (badQueryReply) data.pop_back();
        return {fail ? -123 : 0, 0, desc.bytes};
    }
    Result createDevice(std::uint32_t h) override { require(h > 500); return created(); }
    Result createContext(std::uint32_t h, ContextDesc desc, std::vector<std::uint8_t>& data) override {
        require(h > 500 && validContext(desc) && data.size() == desc.privateBytes);
        if (!data.empty()) data[0] ^= 255;
        if (badContextReply) data.push_back(0);
        return created();
    }
    Result createPagingQueue(std::uint32_t h) override { require(h > 500); return created(); }
    Result createVendorAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        require(device > 500 && validVendorAllocation(desc) && data.size() == desc.privateBytes);
        ++calls; data[0] ^= 255;
        if (badVendorReply == 1) data.pop_back();
        if (fail) return {-123, 0, 0};
        const auto handle = ++next; vendorOwners.emplace(handle, device);
        return {0, badVendorReply == 2 ? 0u : handle, badVendorReply == 3 ? 1ull : 0ull};
    }
    Result destroyVendorAllocations(std::uint32_t device, const std::vector<std::uint32_t>& handles) override {
        require(!handles.empty()); ++calls;
        for (const auto handle : handles) require(vendorOwners.at(handle) == device);
        if (fail) return {-123, 0, 0};
        if (badVendorDestroyReply) return {badVendorDestroyReply == 1 ? 259 : 0,
                                          badVendorDestroyReply == 2 ? 777u : 0u,
                                          badVendorDestroyReply == 3 ? 1ull : 0ull};
        for (const auto handle : handles) { vendorResources.erase(handle); vendorOwners.erase(handle); destroyed.push_back(Kind::VendorAllocation); }
        return {0, 0, 0};
    }
    VendorResourceResult createVendorResourceAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        auto result = createVendorAllocation(device, desc, data);
        if (result.ntstatus < 0) return {result, badResourceReply == 3 ? 777u : 0u};
        const auto resource = ++next;
        vendorResources.emplace(result.nativeHandle, resource);
        return {result, badResourceReply == 1 ? 0u : badResourceReply == 2 ? result.nativeHandle : resource};
    }
    VendorResourceResult createSharedVendorResourceAllocation(std::uint32_t device, SharedVendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        require(sharedResourceEnabled && validSharedVendorAllocation(desc) && data.size() == desc.allocation.privateBytes + desc.runtimeBytes);
        lastSharedRuntime.assign(data.begin() + desc.allocation.privateBytes, data.end());
        std::vector<std::uint8_t> allocation(data.begin(), data.begin() + desc.allocation.privateBytes);
        const auto result = createVendorResourceAllocation(device, desc.allocation, allocation);
        data = allocation; data.insert(data.end(), lastSharedRuntime.begin(), lastSharedRuntime.end());
        return result;
    }
    VendorResourceResult createNativeSharedVendorResourceAllocation(std::uint32_t device, NativeSharedVendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        require(nativeTextureEnabled && validNativeSharedVendorAllocation(desc)); ++nativeTextureCalls;
        return createSharedVendorResourceAllocation(device, desc.shared, data);
    }
    GpuVaResult mapVendorAllocation(std::uint32_t allocation, std::uint32_t queue, GpuVaDesc desc) override {
        require(vendorOwners.count(allocation) && queue > 500 && validGpuVa(desc)); ++calls;
        if (fail) return {{-123, 0, 0}, 0};
        const auto address = desc.base ? desc.base : (desc.minimum ? desc.minimum : 65536ull) + (allocation - 500) * 1048576ull;
        return {{badGpuVaReply == 4 ? 1 : 0x103, badGpuVaReply == 5 ? 777u : 0u,
                 badGpuVaReply == 1 ? 0ull : badGpuVaReply == 2 ? address + 1 : badGpuVaReply == 3 ? MaxGpuAddress : address}, 7019};
    }
    ResidentResult makeVendorResident(std::uint32_t queue, ResidentDesc desc, const std::vector<std::uint32_t>& handles,
                                     const std::vector<std::uint32_t>& priorities) override {
        require(queue > 500 && validResident(desc) && handles.size() == desc.count && priorities.size() == (desc.priorities ? desc.count : 0));
        for (const auto handle : handles) require(vendorOwners.count(handle));
        ++calls; ++residentCalls; lastResidentPriorities = priorities; lastResidentHandles = handles;
        if (fail) return {{static_cast<std::int32_t>(0xc0000017u), 0, 7009}, {desc.count - 1, 0, 65536}};
        return {{badResidentReply == 1 ? 1 : 259, badResidentReply == 2 ? 777u : 0u, 7011},
                {badResidentReply == 3 ? desc.count + 1 : desc.count, badResidentReply == 4 ? 1u : 0u, 0}};
    }
    VendorCpuResult lockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        require(allocation > 500 && device > 500 && vendorOwners.at(allocation) == device); ++calls; ++cpuLocks;
        if (fail) return {{-123, 0, 0}, {0, 0, 0}};
        return {{badCpuReply == 1 ? 259 : 0, badCpuReply == 2 ? 777u : 0u,
                 badCpuReply == 3 ? 1ull : badCpuReply == 4 ? VendorCpuApertureBytes : 0ull},
                {badCpuReply == 5 ? 0u : badCpuReply == 6 ? 4097u : badCpuReply == 7 ? MaxVendorCpuMappingBytes + 4096u :
                 badCpuReply == 8 ? 8192u : 65536u, badCpuReply == 9 ? 1u : 0u, badCpuReply == 10 ? 0ull : 1ull}};
    }
    Result unlockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        require(allocation > 500 && device > 500 && vendorOwners.at(allocation) == device); ++calls; ++cpuUnlocks;
        return {fail ? -123 : badCpuReply == 1 ? 259 : 0, badCpuReply == 2 ? 777u : 0u, badCpuReply == 3 ? 1ull : 0ull};
    }
    Result translateVendorAllocation(std::uint32_t allocation, std::uint32_t device, std::uint32_t adapter) override {
        require(adapter > 500 && vendorOwners.at(allocation) == device); ++calls;
        lastTranslationHandles = {allocation, device, adapter};
        if (fail) return {-123, 0, badTranslationReply == 5 ? 1ull : 0ull};
        return {badTranslationReply == 3 ? 259 : 0, badTranslationReply == 4 ? 777u : 0u,
                badTranslationReply == 1 ? 0ull : badTranslationReply == 2 ? 1ull << 32 :
                translationOverride ? translationOverride : allocation + 0x10000000ull};
    }
    HwQueueResult createHwQueue(std::uint32_t context, HwQueueDesc desc, std::vector<std::uint8_t>& data) override {
        require(context > 500 && validHwQueue(desc) && data.size() == desc.privateBytes); ++calls;
        lastHwQueueDesc = desc;
        data[0] ^= 255;
        if (badHwQueueReply == 11) data.pop_back();
        if (fail) return {{-123, 0, 0}, badHwQueueReply == 12 ? 777u : 0u, 0, 0};
        const auto queue = badHwQueueReply == 2 ? 0u : ++next;
        if (queue) hwQueueParents.emplace(queue, context);
        return {{badHwQueueReply == 1 ? 259 : 0, queue, badHwQueueReply == 10 ? 1ull : 0ull},
                badHwQueueReply == 3 ? 0u : badHwQueueReply == 4 ? queue : ++next,
                badHwQueueReply == 5 ? 1ull : badHwQueueReply == 6 ? FenceApertureBytes : 8192ull,
                badHwQueueReply == 7 ? 0ull : badHwQueueReply == 8 ? 65537ull : badHwQueueReply == 9 ? MaxGpuAddress : 65536ull};
    }
    Result submitHwQueue(std::uint32_t queue, HwSubmitDesc desc, const std::vector<std::uint8_t>& data) override {
        require(hwQueueParents.count(queue) && validHwSubmit(desc) && data == std::vector<std::uint8_t>{37, 38, 39, 40});
        ++calls; ++submitCalls;
        if (fail) return {-123, 0, badSubmitReply == 5 ? 1ull : 0ull};
        return {badSubmitReply == 1 ? 259 : 0, badSubmitReply == 2 ? 777u : 0u,
                badSubmitReply == 3 ? desc.fence - 1 : badSubmitReply == 4 ? UINT64_MAX : desc.fence};
    }
    Result queueHwCommand(std::uint32_t queue, HwSubmitDesc desc, const std::vector<std::uint8_t>& data) override {
        require(asyncSubmitEnabled); return submitHwQueue(queue,desc,data);
    }
    Result queueHwQueueSignal(std::uint32_t sync, const std::vector<std::uint32_t>& queues, HwQueueSignalDesc desc) override {
        require(syncParents.count(sync) && validHwQueueSignal(desc) && queues.size() == desc.count);
        for (auto queue : queues) require(hwQueueParents.count(queue));
        lastSignalQueues=queues; ++calls; ++hwQueueSignalCalls;
        return {fail ? -123 : badHwQueueSignalReply == 1 ? 259 : 0, badHwQueueSignalReply == 2 ? 777u : 0u,
                badHwQueueSignalReply == 3 ? UINT64_MAX : badHwQueueSignalReply == 4 ? 1ull : 0ull};
    }
    SyncResult createSync(std::uint32_t device, SyncDesc desc) override {
        require(device > 500 && validSync(desc)); ++calls; lastSyncDesc = desc;
        if (fail) return {{-123, 0, 0}, {badSyncReply == 10 ? 1ull : 0ull, 0}};
        const auto handle = badSyncReply == 2 ? 0u : ++next;
        if (handle) syncParents.emplace(handle, device);
        return {{badSyncReply == 1 ? 259 : 0, handle, badSyncReply == 3 ? 1ull : 0ull},
                {desc.type == 1 && badSyncReply != 9 ? 0ull : badSyncReply == 4 ? 1ull : badSyncReply == 5 ? FenceApertureBytes : 8192ull,
                 badSyncReply == 11 ? 65536ull : desc.type == 1 || packetSignalSyncFlags(desc.flags) || badSyncReply == 6 ? 0ull :
                     badSyncReply == 7 ? 65537ull : badSyncReply == 8 ? MaxGpuAddress : 65536ull}};
    }
    Result signalContextSync(std::uint32_t context, std::uint32_t sync, ContextSignalDesc desc) override {
        require(context > 500 && syncParents.count(sync) && validContextSignal(desc)); ++calls; ++contextSignalCalls;
        if (fail) return {-123,0,badContextSignalReply == 5 ? 1ull : 0ull};
        return {badContextSignalReply == 1 ? 259 : 0, badContextSignalReply == 2 ? 777u : 0u,
                badContextSignalReply == 3 ? desc.fence - 1 : badContextSignalReply == 4 ? UINT64_MAX : desc.fence};
    }
    GuestPagingResult createGuestPagingQueue(std::uint32_t h) override {
        require(h > 500);
        auto queue = created();
        const auto sync = badPagingReply == 1 ? 0 : ++next;
        if (badPagingReply == 4) queue.value = UINT64_MAX;
        return {queue, sync, badPagingReply == 2 ? 1ull : badPagingReply == 3 ? FenceApertureBytes : 8192ull};
    }
    Result readPagingFence(std::uint32_t h) override { require(h > 500); ++calls; return {0, 0, 42}; }
    Result createAllocation(std::uint32_t h, std::uint32_t size) override {
        require(h > 500); auto result = created();
        if (result.ntstatus >= 0) buffers.emplace(result.nativeHandle, std::vector<std::uint8_t>(size));
        result.value = size; return result;
    }
    Result createSharedAllocation(std::uint32_t h, GuestRange range) override { return createAllocation(h, range.size); }
    Result copySharedAllocation(std::uint32_t destination, std::uint32_t source, CopyRange range) override {
        auto& to = buffers.at(destination); const auto& from = buffers.at(source); ++calls;
        require(destination != source && range.size && range.size <= to.size() && range.size <= from.size());
        require(range.sourceOffset <= from.size() - range.size && range.destinationOffset <= to.size() - range.size);
        std::memcpy(to.data() + range.destinationOffset, from.data() + range.sourceOffset, range.size);
        return {0, 0, range.size};
    }
    Result writeAllocation(std::uint32_t h, Range range, const std::uint8_t* data) override {
        auto& buffer = buffers.at(h); ++calls;
        require(range.size <= buffer.size() && range.offset <= buffer.size() - range.size);
        std::memcpy(buffer.data() + range.offset, data, range.size); return {0, 0, range.size};
    }
    Result readAllocation(std::uint32_t h, Range range, std::vector<std::uint8_t>& data) override {
        const auto& buffer = buffers.at(h); ++calls;
        require(range.size <= buffer.size() && range.offset <= buffer.size() - range.size);
        data.assign(buffer.begin() + range.offset, buffer.begin() + range.offset + range.size);
        if (shortRead) data.pop_back();
        return {0, 0, range.size};
    }
    Result makeResident(std::uint32_t h, std::uint32_t q) override {
        require(buffers.count(h) && q > 500); ++calls; return {0, 0, 42};
    }
    Result mapAllocation(std::uint32_t h, std::uint32_t q) override {
        require(buffers.count(h) && q > 500); ++calls; return {0, 0, 0x123456789ull};
    }
    Result queryResidency(std::uint32_t h) override { require(buffers.count(h)); ++calls; return {0, 0, 1}; }
    Result destroy(Kind k, std::uint32_t h) override {
        require(h > 500); ++calls;
        if (k == Kind::PagingSync || k == Kind::HwQueueSync) return {0, 0, 0}; // borrowed
        if (k == Kind::VendorAllocation) return destroyVendorAllocations(vendorOwners.at(h), {h});
        if (k == Kind::GpuReservation && badReservationDestroyReply)
            return {badReservationDestroyReply == 1 ? 259 : 0, badReservationDestroyReply == 2 ? 777u : 0u,
                    badReservationDestroyReply == 3 ? 1ull : 0ull};
        if (!fail) {
            destroyed.push_back(k);
            if (k == Kind::Allocation) require(buffers.erase(h) == 1);
            if (k == Kind::HwQueue) require(hwQueueParents.erase(h) == 1);
            if (k == Kind::Sync) require(syncParents.erase(h) == 1);
            if (k == Kind::SharedResource) require(resourceTickets.erase(h) == 1);
            if (k == Kind::GpuReservation) require(reservationParents.erase(h) == 1);
        }
        return {fail ? -123 : 0, 0, 0};
    }
};

static void teardownFailureDependencies() {
    struct TeardownNative : Fake {
        Kind blocked=Kind::HwQueue;Result rejection{-123,0,0};bool reject=false;
        unsigned failures=0;
        std::vector<Kind> attempts;
        native_gpu::Capabilities capabilities() const override {
            auto value=Fake::capabilities();
            value.flags|=AsyncHwSubmitCapability|SyncCapability|GpuReservationCapability|VendorMemoryManagementCapability|
                ExpandedVendorGpuCapability|ExpandedVendorCpuCapability|VendorAllocationCapability|SharedVendorResourceCapability|NativeSharedTextureCapability;
            return value;
        }
        OpenGlProfile beginOpenGl(std::uint32_t version) override { require(version==14);return GuestOpenGlMultiappProfile; }
        bool beginCoordinatedObjects(CoordinatedObjectCapacity value) override { return value.version==9 && validCoordinatedObjectCapacity(value); }
        bool allowsReservationBackedMappings() const override { return true; }
        bool retainsMappedResidencyOnEvict() const override { return true; }
        SyncSharingProfile beginSyncSharing() override { return GuestSyncSharingProfile; }
        Result destroy(Kind kind,std::uint32_t handle) override {
            attempts.push_back(kind);
            if(reject && kind==blocked) { ++failures;return rejection; }
            return Fake::destroy(kind,handle);
        }
    };
    unsigned cases=0;
    for(const bool coordinated:{false,true}) for(unsigned fault=0;fault<13;++fault) {
        TeardownNative native;native.hwQueuesEnabled=true;
        std::unique_ptr<CoordinatedProfiles> profile;
        if(coordinated) profile=std::make_unique<CoordinatedProfiles>(native,GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects);
        auto session=coordinated ? std::make_unique<Session>(native,*profile) : std::make_unique<Session>(native);
        require(!header(session->dispatch(hello())).status);
        const auto create=[&](std::vector<std::uint8_t> packet) {
            const auto result=header(session->dispatch(packet));require(!result.status && result.handle);return result.handle;
        };
        const auto adapter=create(request(Op::OpenAdapter));
        const auto device=create(request(Op::CreateDevice,adapter));
        auto contextPacket=request(Op::CreateContext,device,ContextDesc{0,1,16,12,4,0});
        contextPacket.insert(contextPacket.end(),{1,2,3,4});
        const auto context=create(contextPacket);
        auto queuePacket=request(Op::CreateHwQueue,context,HwQueueDesc{0,4,0,0});
        queuePacket.insert(queuePacket.end(),{37,38,39,40});
        create(queuePacket);
        create(request(Op::CreateAllocation,device,std::uint32_t{4096}));
        require(session->objectCount()==6);
        if(fault) {
            const auto target=(fault-1)/4, reply=(fault-1)%4;
            native.blocked=target==0 ? Kind::HwQueue : target==1 ? Kind::Allocation : Kind::Device;
            native.rejection=reply==0 ? Result{-123,0,0} : reply==1 ? Result{259,0,0} :
                             reply==2 ? Result{0,777,0} : Result{0,0,1};
            native.reject=true;
        }
        if(coordinated) for(unsigned phase=0;phase<4;++phase) session->ownerTeardownPhase(phase);
        else session.reset();
        if(fault) {
            for(const auto kind:native.attempts) {
                const bool forbidden=native.blocked==Kind::HwQueue ? kind!=Kind::HwQueue && kind!=Kind::HwQueueSync :
                                     native.blocked==Kind::Allocation ? kind==Kind::Device || kind==Kind::Adapter : kind==Kind::Adapter;
                if(forbidden) throw std::runtime_error("TEARDOWN_PARENT_GUARD_REPRO: destruction advanced past a failed dependency");
            }
            require(native.failures==1);
            if(native.blocked==Kind::HwQueue) require(native.hwQueueParents.size()==1 && native.buffers.size()==1);
            if(native.blocked==Kind::Allocation) require(native.buffers.size()==1);
            if(coordinated) {
                require(session->objectCount()==(native.blocked==Kind::HwQueue ? 5u : native.blocked==Kind::Allocation ? 3u : 2u));
                native.reject=false;
                for(unsigned phase=0;phase<4;++phase) session->ownerTeardownPhase(phase);
                require(!session->objectCount() && native.buffers.empty() && native.hwQueueParents.empty());
            }
        } else {
            require(native.destroyed==std::vector<Kind>{Kind::HwQueue,Kind::Allocation,Kind::Context,Kind::Device,Kind::Adapter});
            require(native.buffers.empty() && native.hwQueueParents.empty());
            if(coordinated) require(!session->objectCount());
        }
        if(coordinated) require(native.destroyed.size()==5);
        ++cases;
    }
    std::cout<<"TEARDOWN_PARENT_GUARD_PASS cases="<<cases<<" coordinatedRetry=true eof=true mock=true gpu=false\n";
}

static void actorResourceAliases() {
    struct AliasNative : Fake {
        bool equivalent=true;unsigned duplicates=0;int broken=0;std::uint32_t known=0;
        bool samePhysicalAdapter(std::uint32_t a,std::uint32_t b) const override { return equivalent && a>500 && b>500; }
        Result duplicateSharedReference(Kind kind,std::uint32_t source) override {
            require(kind==Kind::SharedResource && resourceTickets.count(source));++duplicates;
            if(broken==3) return {0,0,0};
            if(broken==4) return {0,source,0};
            if(broken==5) return {-123,0,0};
            if(broken==6) return {0,known,0};
            const auto id=++next;resourceTickets.insert(id);
            return {broken==1 ? 259 : broken==7 ? -123 : 0,id,broken==2 ? 1ull : 0ull};
        }
    } native;
    native.vendorEnabled=native.resourceEnabled=native.sharedResourceEnabled=native.nativeTextureEnabled=true;
    Session recipient(native),unnegotiated(native);recipient.dispatch(hello());
    require(!header(recipient.dispatch(request(Op::BeginResourceSharing))).status);
    const auto adapter=header(recipient.dispatch(request(Op::OpenAdapter))).handle;
    const auto device=header(recipient.dispatch(request(Op::CreateDevice,adapter))).handle;
    std::uint32_t alias=0;
    {
        Session donor(native);donor.dispatch(hello());require(!header(donor.dispatch(request(Op::BeginResourceSharing))).status);
        const auto a=header(donor.dispatch(request(Op::OpenAdapter))).handle;
        const auto d=header(donor.dispatch(request(Op::CreateDevice,a))).handle;
        NativeSharedVendorAllocationDesc desc{{{4,0x78100000,0,4,0,0},4,0},{130,73,28,1}};
        auto create=request(Op::CreateNativeSharedVendorResourceAllocation,d,desc);create.insert(create.end(),{37,38,39,40,5,6,7,8});
        const auto output=donor.dispatch(create);VendorResourceReply resource{};
        require(!header(output).status);std::memcpy(&resource,output.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
        const auto ticket=header(donor.dispatch(request(Op::ExportResource,resource.resource))).handle;require(ticket);
        for(const auto kind:{Kind::Adapter,Kind::SharedSync,Kind::VendorResource})
            require(recipient.aliasSharedReference(donor,kind,ticket,adapter).ntstatus<0);
        require(donor.aliasSharedReference(donor,Kind::SharedResource,ticket,a).ntstatus<0);
        require(unnegotiated.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter).ntstatus<0);
        require(recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,device).ntstatus<0);
        require(recipient.aliasSharedReference(donor,Kind::SharedResource,UINT32_MAX,adapter).ntstatus<0);
        native.equivalent=false;require(recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter).ntstatus<0);
        native.equivalent=true;require(!native.duplicates);
        const auto valid=recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter);
        require(!valid.ntstatus && valid.ticket);alias=valid.ticket;
        const auto initial=native.resourceTickets.size();native.known=*native.resourceTickets.rbegin();
        for(int mode=1;mode<=7;++mode) {
            native.broken=mode;const auto result=recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter);
            require(result.ntstatus<0 && !result.ticket && native.resourceTickets.size()==initial);
        }
        native.broken=0;
        for(std::size_t n=1;n<MaxSharedResourceHandles;++n) require(!recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter).ntstatus);
        const auto calls=native.duplicates;require(recipient.aliasSharedReference(donor,Kind::SharedResource,ticket,adapter).ntstatus<0 && native.duplicates==calls);
    }
    require(native.resourceTickets.size()==MaxSharedResourceHandles);
    const auto info=recipient.dispatch(request(Op::QuerySharedResourceInfo,alias,SharedResourceQueryDesc{device,4}));
    require(!header(info).status);
    const auto imported=recipient.dispatch(request(Op::ImportResource,alias,SharedResourceOpenDesc{device,4,8,0}));
    require(!header(imported).status && header(imported).handle);
    std::cout<<"ACTOR_RESOURCE_ALIAS physicalAdapterChecked=true negotiatedOnly=true independentNativeReference=true donorSessionDestroyed=true recipientImport=true malformedRepliesRejected=true quotasPreserved=true native=mock gpu=false\n";
}

static void applicationResourceReferences() {
    struct GroupNative : Fake {
        unsigned duplicates=0;
        native_gpu::Capabilities capabilities() const override {
            auto value=Fake::capabilities();
            value.flags|=AsyncHwSubmitCapability|SyncCapability|GpuReservationCapability|VendorMemoryManagementCapability|
                ExpandedVendorGpuCapability|ExpandedVendorCpuCapability|VendorAllocationCapability|SharedVendorResourceCapability|NativeSharedTextureCapability;
            return value;
        }
        OpenGlProfile beginOpenGl(std::uint32_t version) override { require(version==14);return GuestOpenGlMultiappProfile; }
        bool beginCoordinatedObjects(CoordinatedObjectCapacity value) override { return value.version==9 && validCoordinatedObjectCapacity(value); }
        bool allowsReservationBackedMappings() const override { return true; }
        bool retainsMappedResidencyOnEvict() const override { return true; }
        SyncSharingProfile beginSyncSharing() override { return GuestSyncSharingProfile; }
        bool samePhysicalAdapter(std::uint32_t a,std::uint32_t b) const override { return a>500 && b>500; }
        Result duplicateSharedReference(Kind kind,std::uint32_t source) override {
            require(kind==Kind::SharedResource && resourceTickets.count(source));++duplicates;
            if(resourceTickets.size()>=MaxCoordinatedSharedReferences) return {static_cast<std::int32_t>(0xc0000017u),0,0};
            const auto id=++next;resourceTickets.insert(id);return {0,id,0};
        }
    } native;
    native.vendorEnabled=native.resourceEnabled=native.sharedResourceEnabled=native.nativeTextureEnabled=true;
    CoordinatedProfiles profiles(native,GuestOpenGlMultiappCapacity,GuestCoordinatedMultiappObjects,32);
    Session donor(native,profiles,false),first(native,profiles,true),second(native,profiles,false);
    std::uint32_t adapters[3]{};Session* sessions[]={&donor,&first,&second};
    for(unsigned n=0;n<3;++n) {
        sessions[n]->dispatch(hello());const auto profile=sessions[n]->dispatch(request(Op::BeginResourceSharing));
        require(!header(profile).status);ResourceSharingProfile agreement{};
        std::memcpy(&agreement,profile.data()+sizeof(Header),sizeof agreement);
        require(validApplicationResourceSharingProfile(agreement));
        adapters[n]=header(sessions[n]->dispatch(request(Op::OpenAdapter))).handle;
        require(header(sessions[n]->dispatch(request(Op::CreateDevice,adapters[n]))).handle);
    }
    NativeSharedVendorAllocationDesc desc{{{4,0x78100000,0,4,0,0},4,0},{130,73,28,1}};
    auto create=request(Op::CreateNativeSharedVendorResourceAllocation,2,desc);create.insert(create.end(),{37,38,39,40,5,6,7,8});
    const auto output=donor.dispatch(create);VendorResourceReply resource{};
    require(!header(output).status);std::memcpy(&resource,output.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
    const auto ticket=header(donor.dispatch(request(Op::ExportResource,resource.resource))).handle;require(ticket);
    std::vector<std::uint32_t> firstTickets;
    for(unsigned n=0;n<32;++n) {
        const auto alias=first.aliasSharedReference(donor,Kind::SharedResource,ticket,adapters[1]);
        require(!alias.ntstatus && alias.ticket);firstTickets.push_back(alias.ticket);
    }
    const auto attempts=native.duplicates;
    require(first.aliasSharedReference(donor,Kind::SharedResource,ticket,adapters[1]).ntstatus<0 && native.duplicates==attempts);
    for(unsigned n=0;n<31;++n) require(!second.aliasSharedReference(donor,Kind::SharedResource,ticket,adapters[2]).ntstatus);
    require(native.resourceTickets.size()==64);
    const auto globalRefusal=second.aliasSharedReference(donor,Kind::SharedResource,ticket,adapters[2]);
    require(globalRefusal.ntstatus<0 && !globalRefusal.ticket && native.resourceTickets.size()==64);
    require(!header(first.dispatch(request(Op::CloseSharedResource,firstTickets.front()))).status);
    require(!second.aliasSharedReference(donor,Kind::SharedResource,ticket,adapters[2]).ntstatus && native.resourceTickets.size()==64);
    require(!header(first.dispatch(request(Op::QueryDriverVersion,adapters[1]))).status);
    std::cout<<"APPLICATION_RESOURCE_REFERENCES perActor32=true thirtyThirdRefusedBeforeNative=true group64=true sixtyFifthRefused=true lastCloseReusesSlot=true donorPreserved=true mock=true gpu=false\n";
}
int main() {
    teardownFailureDependencies();
    {
        SubmissionActivity countOverflow{UINT64_MAX,0,0,true};
        countOverflow.record(true,4096);
        require(!countOverflow.exact && countOverflow.acceptedCommands==UINT64_MAX && !countOverflow.asynchronousCommands);
        SubmissionActivity byteOverflow{1,1,UINT64_MAX-4095,true};
        byteOverflow.record(false,4096);
        require(!byteOverflow.exact && byteOverflow.acceptedCommands==1 && byteOverflow.acceptedCommandBytes==UINT64_MAX-4095);
        byteOverflow.record(true,1);
        require(byteOverflow.acceptedCommands==1 && byteOverflow.asynchronousCommands==1);
    }
    applicationResourceReferences();
    {
        static_assert(static_cast<unsigned>(Op::QueryNativeGpuBufferAdmission) == 0x20d0);
        Fake f; Session s(f);
        const NativeGpuBufferDesc desc{65536, 0, 0};
        const auto query = request(Op::QueryNativeGpuBufferAdmission, 0, desc);
        require(header(s.dispatch(query)).status == -71 && !f.nativeGpuBufferAdmissionCalls);
        s.dispatch(hello());
        require(header(s.dispatch(query)).status == -95 && !f.nativeGpuBufferAdmissionCalls);
        f.nativeGpuBufferEnabled = true;
        for (unsigned n = 0; n < 8; ++n) {
            auto d = desc;
            if (n == 0) d.bytes = 0;
            if (n == 1) d.bytes = NativeGpuBufferProfile.maxBytes + 1;
            if (n == 2) d.bytes = UINT64_MAX;
            if (n == 3) d.flags = 1;
            if (n == 4) d.reserved = 1;
            auto p = request(Op::QueryNativeGpuBufferAdmission, n == 5 ? 1 : 0, d);
            if (n == 6) p.pop_back();
            if (n == 7) p.push_back(0);
            require(header(s.dispatch(p)).status == -22 && !f.nativeGpuBufferAdmissionCalls);
        }
        for (const auto status : {static_cast<std::int32_t>(0xc00000bbu), static_cast<std::int32_t>(0xc0000017u),
                                  static_cast<std::int32_t>(0xc0000001u), 0}) {
            f.nativeGpuBufferAdmissionResult = {status, 0, 0};
            const auto out = s.dispatch(query); Reply value{};
            require(!header(out).status && !header(out).handle && out.size() == sizeof(Header) + sizeof value);
            std::memcpy(&value, out.data() + sizeof(Header), sizeof value);
            require(value.ntstatus == status && !value.value && !value.reserved);
        }
        require(f.lastGpuBufferAdmission.bytes == desc.bytes && f.vendorOwners.empty());
        for (const auto bad : std::vector<Result>{{259,0,0}, {0,777,0}, {0,0,1}, {static_cast<std::int32_t>(0xc0000017u),0,1}}) {
            f.nativeGpuBufferAdmissionResult = bad; require(header(s.dispatch(query)).status == -5);
        }
        f.nativeGpuBufferAdmissionResult = {0,0,0};
        require(!header(s.dispatch(query)).status && f.vendorOwners.empty());
        std::cout << "NATIVE_GPU_BUFFER_ADMISSION_WIRE_PASS negotiatedOnly=true invalidRequestsRejected=true memoryStatusPreserved=true retrySameSession=true noGpuObjects=true mock=true gpu=false\n";
    }
    {
        static_assert(static_cast<unsigned>(Op::QueryDeviceExecutionState) == 0x20b0);
        static_assert(static_cast<unsigned>(Op::QueryNativeGpuBufferCaps) == 0x20c0);
        Fake f; f.vendorEnabled = f.resourceEnabled = f.sharedResourceEnabled = true;
        Session s(f), foreign(f);
        const auto query = request(Op::QueryNativeGpuBufferCaps);
        require(header(s.dispatch(query)).status == -71);
        s.dispatch(hello()); foreign.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        NativeSharedGpuBufferAllocationDesc desc{{{4, 0x78100000, 0, 4, 0, 0}, 4, 0}, {65536, 0, 0}};
        const auto create = [&](std::uint32_t parent, NativeSharedGpuBufferAllocationDesc d) {
            auto p = request(Op::CreateNativeSharedGpuBufferAllocation, parent, d);
            p.insert(p.end(), {37, 38, 39, 40, 5, 6, 7, 8}); return p;
        };
        require(header(s.dispatch(query)).status == -95);
        require(header(s.dispatch(create(device, desc))).status == -95 && !f.nativeGpuBufferCalls);
        f.nativeGpuBufferEnabled = true;
        for (int n = 1; n <= 5; ++n) {
            f.badNativeGpuBufferCaps = n;
            require(header(s.dispatch(query)).status == -95);
            require(header(s.dispatch(create(device, desc))).status == -95 && !f.nativeGpuBufferCalls);
        }
        f.badNativeGpuBufferCaps = 0;
        auto out = s.dispatch(query); NativeGpuBufferCaps caps{};
        require(!header(out).status && out.size() == sizeof(Header) + sizeof caps);
        std::memcpy(&caps, out.data() + sizeof(Header), sizeof caps); require(validNativeGpuBufferCaps(caps));
        require(!f.nativeTextureEnabled); // Separate queried buffer contract.
        require(header(s.dispatch(request(Op::QueryNativeGpuBufferCaps, device))).status == -22);
        auto extra = query; extra.push_back(0); require(header(s.dispatch(extra)).status == -22);
        require(header(foreign.dispatch(create(device, desc))).status == -9);
        require(header(s.dispatch(create(adapter, desc))).status == -9 && !f.nativeGpuBufferCalls);
        for (unsigned n = 0; n < 12; ++n) {
            auto d = desc;
            if (n == 0) d.buffer.bytes = 0;
            if (n == 1) d.buffer.bytes = NativeGpuBufferProfile.maxBytes + 1;
            if (n == 2) d.buffer.bytes = UINT64_MAX;
            if (n == 3) d.buffer.flags = 1;
            if (n == 4) d.buffer.reserved = 1;
            if (n == 5) d.shared.reserved = 1;
            if (n == 6) d.shared.runtimeBytes = 0;
            if (n == 7) d.shared.allocation.flags = 0;
            if (n == 8) d.shared.allocation.privateBytes = UINT32_MAX;
            if (n == 9) d.shared.runtimeBytes = 1025;
            auto p = create(device, d);
            if (n == 10) p.pop_back();
            if (n == 11) p.push_back(0);
            require(header(s.dispatch(p)).status == -22 && !f.nativeGpuBufferCalls);
        }
        f.fail = true; out = s.dispatch(create(device, desc));
        require(!header(out).status && !header(out).handle && out.back() == 8 && f.vendorOwners.empty());
        f.fail = false; f.badResourceReply = 1;
        require(header(s.dispatch(create(device, desc))).status == -5 && f.vendorOwners.empty());
        f.badResourceReply = 0;
        for (const auto flags : {0u, 4u}) {
            desc.buffer.flags = flags; out = s.dispatch(create(device, desc));
            VendorResourceReply resource{};
            require(!header(out).status && header(out).handle && out.back() == 8);
            std::memcpy(&resource, out.data() + sizeof(Header) + sizeof(Reply), sizeof resource);
            require(resource.resource && !resource.reserved && f.lastSharedRuntime == std::vector<std::uint8_t>({5,6,7,8}));
            require(!header(s.dispatch(request(Op::DestroyVendorResource, resource.resource, device))).status);
        }
        require(f.nativeGpuBufferCalls == 4 && f.vendorOwners.empty() && f.vendorResources.empty());
        std::cout << "NATIVE_GPU_BUFFER_WIRE_PASS queriedCaps=true invalidRequestsRejected=true ownedDevice=true originalRuntimePreserved=true failedNativeOwnershipRejected=true mock=true gpu=false\n";
    }
    {
        Fake driver;Session session(driver),other(driver);
        require(header(session.dispatch(request(Op::QueryDeviceExecutionState,1))).status==-71);
        session.dispatch(hello());other.dispatch(hello());
        const auto adapter=header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto initialCalls=driver.calls;
        require(header(session.dispatch(request(Op::QueryDeviceExecutionState,adapter))).status==-9);
        require(header(other.dispatch(request(Op::QueryDeviceExecutionState,device))).status==-9);
        auto extra=request(Op::QueryDeviceExecutionState,device);extra.push_back(0);
        require(header(session.dispatch(extra)).status==-22 && driver.calls==initialCalls);
        for(std::uint64_t state=1;state<=7;++state) {
            driver.executionState={0,0,state};const auto out=session.dispatch(request(Op::QueryDeviceExecutionState,device));
            require(!header(out).status && header(out).handle==device && out.size()==sizeof(Header)+sizeof(Reply));
            Reply value{};std::memcpy(&value,out.data()+sizeof(Header),sizeof value);
            require(!value.ntstatus && !value.reserved && value.value==state);
        }
        const auto nativeDevice=driver.lastExecutionDevice;
        const auto otherAdapter=header(other.dispatch(request(Op::OpenAdapter))).handle;
        const auto otherDevice=header(other.dispatch(request(Op::CreateDevice,otherAdapter))).handle;
        require(otherDevice==device); // Equal wire IDs must still resolve per actor.
        require(!header(other.dispatch(request(Op::QueryDeviceExecutionState,otherDevice))).status);
        require(driver.lastExecutionDevice!=nativeDevice);
        for(const auto invalid:std::vector<Result>{{0,0,0},{0,0,8},{0,0,UINT64_MAX},{259,0,1},{0,42,1},{-123,0,1}}) {
            driver.executionState=invalid;
            require(header(session.dispatch(request(Op::QueryDeviceExecutionState,device))).status==-5);
        }
        driver.executionState={static_cast<std::int32_t>(0xc00002b6u),0,0};
        const auto removed=session.dispatch(request(Op::QueryDeviceExecutionState,device));
        Reply status{};std::memcpy(&status,removed.data()+sizeof(Header),sizeof status);
        require(!header(removed).status && status.ntstatus==driver.executionState.ntstatus && !status.value);
    }
    for(const auto kind:{Kind::Adapter,Kind::Device,Kind::PagingQueue,Kind::Allocation,Kind::Context,
            Kind::PagingSync,Kind::VendorAllocation,Kind::Sync,Kind::GpuReservation,Kind::VendorResource})
        require(nativeDestroyWaitsForSubmissions(kind));
    require(nativeDestroyWaitsForSubmissions(static_cast<Kind>(99)));
    require(!nativeDestroyWaitsForSubmissions(Kind::SharedResource) && !nativeDestroyWaitsForSubmissions(Kind::SharedSync));
    require(!nativeDestroyWaitsForSubmissions(Kind::HwQueue) && !nativeDestroyWaitsForSubmissions(Kind::HwQueueSync));
    actorResourceAliases();
    {
        static_assert(static_cast<unsigned>(Op::EvictVendorAllocations)==0x20a1);
        static_assert(static_cast<unsigned>(Op::CreateNativeInteropResource)==0x20a2);
        Fake f; f.vendorEnabled=f.resourceEnabled=f.sharedResourceEnabled=f.nativeTextureEnabled=true;
        Session s(f);
        const auto query=request(Op::QueryNativeInteropCaps);
        require(header(s.dispatch(query)).status==-71);
        s.dispatch(hello());
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto other=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        NativeSharedVendorAllocationDesc desc{{{4,0x78100000,0,4,0,0},4,0},{1281,719,87,1}};
        const auto create=[&](std::uint32_t parent,NativeSharedVendorAllocationDesc d) {
            auto p=request(Op::CreateNativeInteropResource,parent,d);
            p.insert(p.end(),{37,38,39,40,5,6,7,8}); return p;
        };
        require(header(s.dispatch(query)).status==-95);
        require(header(s.dispatch(create(device,desc))).status==-95 && !f.nativeInteropCalls);
        f.nativeInteropEnabled=true;
        for(int n=1;n<=7;++n) {
            f.badNativeInteropCaps=n;
            require(header(s.dispatch(query)).status==-95);
            require(header(s.dispatch(create(device,desc))).status==-95 && !f.nativeInteropCalls);
        }
        f.badNativeInteropCaps=0;
        auto out=s.dispatch(query); NativeInteropCaps caps{};
        require(!header(out).status && out.size()==sizeof(Header)+sizeof caps);
        std::memcpy(&caps,out.data()+sizeof(Header),sizeof caps); require(validNativeInteropCaps(caps));
        require(header(s.dispatch(request(Op::QueryNativeInteropCaps,device))).status==-22);
        auto extra=query; extra.push_back(0); require(header(s.dispatch(extra)).status==-22);
        require(header(s.dispatch(create(adapter,desc))).status==-9 && !f.nativeInteropCalls);
        for(unsigned n=0;n<13;++n) {
            auto d=desc;
            if(n==0) d.texture.width=0;
            if(n==1) d.texture.height=0;
            if(n==2) d.texture.width=4097;
            if(n==3) d.texture.height=4097;
            if(n==4) d.texture.format=29;
            if(n==5) d.texture.flags=3;
            if(n==6) d.shared.reserved=1;
            if(n==7) d.shared.runtimeBytes=0;
            if(n==8) d.shared.allocation.flags=0;
            if(n==9) d.shared.allocation.privateBytes=UINT32_MAX;
            if(n==10) d.texture.width=UINT32_MAX;
            auto p=create(device,d);
            if(n==11) p.pop_back();
            if(n==12) p.push_back(0);
            require(header(s.dispatch(p)).status==-22 && !f.nativeInteropCalls);
        }
        require(validNativeInteropTexture({4096,4096,28,1}));
        require(!validNativeTexture(desc.texture)); // Legacy consumer remains fixed RGBA.
        f.fail=true; out=s.dispatch(create(device,desc));
        require(!header(out).status && !header(out).handle && out.back()==8 && f.vendorOwners.empty());
        f.fail=false; f.badResourceReply=1;
        require(header(s.dispatch(create(device,desc))).status==-5 && f.vendorOwners.empty());
        f.badResourceReply=0;
        std::vector<std::uint32_t> resources;
        for(const auto format:{28u,87u,88u}) {
            desc.texture.format=format; out=s.dispatch(create(device,desc));
            VendorResourceReply resource{};
            require(!header(out).status && header(out).handle);
            std::memcpy(&resource,out.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
            require(resource.resource && !resource.reserved && out.back()==8);
            resources.push_back(resource.resource);
        }
        require(f.vendorOwners.size()==3 && f.nativeInteropCalls==5);
        require(header(s.dispatch(request(Op::DestroyVendorResource,resources[0],other))).status==-9);
        for(const auto resource:resources) require(!header(s.dispatch(request(Op::DestroyVendorResource,resource,device))).status);
        require(f.vendorOwners.empty() && f.vendorResources.empty());
    }
    {
        Fake f; f.vendorEnabled=true; f.residencyEnabled=true; f.gpuVaEnabled=true; f.cpuEnabled=true;
        Session s(f); s.dispatch(hello());
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto other=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto queue=header(s.dispatch(request(Op::CreatePagingQueue,device))).handle;
        const auto query=request(Op::QueryMemoryBudget,adapter,MemoryBudgetDesc{0,0});
        auto before=f.calls;
        require(header(s.dispatch(query)).status==-95 && f.calls==before);
        f.memoryManagementEnabled=true;
        const auto queried=s.dispatch(query); MemoryBudgetReply info{};
        require(!header(queried).status && queried.size()==sizeof(Header)+sizeof(Reply)+sizeof info);
        std::memcpy(&info,queried.data()+sizeof(Header)+sizeof(Reply),sizeof info);
        require(info.budget==268435456 && info.usage==65536 && !info.reservation && info.available==268435456);
        for (unsigned n=1;n<=4;++n) { f.badMemoryBudgetReply=n; require(header(s.dispatch(query)).status==-5); }
        f.badMemoryBudgetReply=0; before=f.calls;
        require(header(s.dispatch(request(Op::QueryMemoryBudget,device,MemoryBudgetDesc{0,0}))).status==-9);
        require(header(s.dispatch(request(Op::QueryMemoryBudget,adapter,MemoryBudgetDesc{2,0}))).status==-22);
        require(header(s.dispatch(request(Op::QueryMemoryBudget,adapter,MemoryBudgetDesc{0,1}))).status==-22);
        auto malformed=query; malformed.push_back(0); require(header(s.dispatch(malformed)).status==-22 && f.calls==before);
        auto creation=request(Op::CreateVendorAllocation,device,VendorAllocationDesc{0,0,0,1,0,0}); creation.push_back(1);
        const auto allocation=header(s.dispatch(creation)).handle;
        auto evict=[&](EvictDesc desc,std::initializer_list<std::uint32_t> ids,std::uint32_t owner) {
            auto packet=request(Op::EvictVendorAllocations,owner,desc);
            for (const auto id:ids) { const auto bytes=reinterpret_cast<const std::uint8_t*>(&id); packet.insert(packet.end(),bytes,bytes+4); }
            return s.dispatch(packet);
        };
        before=f.calls;
        require(header(evict({0,0},{},device)).status==-22);
        require(header(evict({1,4},{allocation},device)).status==-22);
        require(header(evict({2,0},{allocation},device)).status==-22);
        require(header(evict({2,0},{allocation,allocation},device)).status==-22);
        require(header(evict({1,0},{allocation},other)).status==-9);
        require(header(evict({1,0},{device},device)).status==-9 && f.calls==before);
        auto resident=request(Op::MakeVendorResident,queue,ResidentDesc{1,0,0,0});
        const auto idBytes=reinterpret_cast<const std::uint8_t*>(&allocation); resident.insert(resident.end(),idBytes,idBytes+4);
        require(!header(s.dispatch(resident)).status && !header(s.dispatch(resident)).status);
        require(!header(s.dispatch(request(Op::MapVendorAllocation,allocation,GpuVaDesc{queue,0,0,65536,1ull<<40,0,16,1,0}))).status);
        const auto lock=request(Op::LockVendorAllocation,allocation,VendorCpuDesc{device,0});
        const auto unlock=request(Op::UnlockVendorAllocation,allocation,VendorCpuDesc{device,0});
        require(!header(s.dispatch(lock)).status);
        f.evictAttempted=false;
        auto busy=evict({1,0},{allocation},device); Reply result{}; std::memcpy(&result,busy.data()+sizeof(Header),sizeof result);
        require(!header(busy).status && result.ntstatus<0 && !result.value);
        f.evictAttempted=true;
        auto first=evict({1,0},{allocation},device); std::memcpy(&result,first.data()+sizeof(Header),sizeof result);
        require(!header(first).status && !result.ntstatus && result.value==65536);
        before=f.calls; require(header(evict({1,0},{allocation},device)).status==-16 && f.calls==before);
        require(!header(s.dispatch(unlock)).status);
        require(!header(evict({1,0},{allocation},device)).status);
        require(!header(s.dispatch(resident)).status && !header(s.dispatch(resident)).status && !header(s.dispatch(lock)).status);
        f.fail=true;
        const auto partial=evict({1,0},{allocation},device); std::memcpy(&result,partial.data()+sizeof(Header),sizeof result);
        require(!header(partial).status && result.ntstatus==-123 && !result.value);
        f.fail=false; before=f.calls;
        require(header(evict({1,0},{allocation},device)).status==-16 && f.calls==before);
        f.mappedResidencyRetained=true;
        const auto protectedEviction=evict({1,0},{allocation},device);
        require(!header(protectedEviction).status && f.calls==before+1);
        // Transfers may repeat without exhausting the legacy lifetime attempt
        // limit. The count of simultaneously live guest references stays bounded.
        for(unsigned n=0;n<1000;++n) {
            require(!header(s.dispatch(resident)).status);
            require(!header(evict({1,0},{allocation},device)).status);
        }
        for(unsigned n=0;n<MaxVendorResidencyAttempts;++n) require(!header(s.dispatch(resident)).status);
        before=f.calls;require(header(s.dispatch(resident)).status==-24 && f.calls==before);
        for(unsigned n=0;n<MaxVendorResidencyAttempts;++n) require(!header(evict({1,0},{allocation},device)).status);
        f.mappedResidencyRetained=false;
        require(!header(s.dispatch(resident)).status);
        before=f.calls;require(header(evict({1,0},{allocation},device)).status==-16 && f.calls==before);
        require(!header(s.dispatch(unlock)).status);
        for (const auto flags:{0u,1u,2u,3u}) require(!header(evict({1,flags},{allocation},device)).status);
        for (unsigned n=1;n<=2;++n) { f.badEvictReply=n; require(header(evict({1,0},{allocation},device)).status==-5); }
        f.badEvictReply=3; f.fail=true; require(header(evict({1,0},{allocation},device)).status==-5);
        f.badEvictReply=0; f.fail=false;
        std::cout<<"MEMORY_WIRE budgetOwnership=true foreignDeviceRejected=true boundedLists=true cpuLastReferenceProtected=true partialFailureConservative=true nativeBlockedCallPreservesRefs=true gpu=false\n";
    }
    struct OpenGlFake : Fake {
        OpenGlProfile profile = GuestOpenGlProfile;
        unsigned begins = 0;
        std::uint32_t missing = 0;
        native_gpu::Capabilities capabilities() const override {
            auto caps = Fake::capabilities();
            caps.flags |= AsyncHwSubmitCapability | SyncCapability | GpuReservationCapability | UINT32_C(0x80000000);
            if (profile.version>=6) caps.flags |= ExpandedVendorGpuCapability;
            if (profile.version>=7) caps.flags |= ExpandedVendorCpuCapability;
            caps.flags &= ~missing; return caps;
        }
        OpenGlProfile beginOpenGl(std::uint32_t = 1) override { ++begins; return profile; }
    };
    {
        require(lifetimeBudgetAvailable(256,0) && lifetimeBudgetAvailable(UINT64_C(0x100000001),0));
        require(!lifetimeBudgetAvailable(UINT64_MAX,0) && !lifetimeBudgetAvailable(256,256));
        require(lifetimeBudgetAvailable(255,256));
        OpenGlFake f; f.profile=GuestOpenGlDesktopProfile;
        f.vendorEnabled=f.gpuVaEnabled=f.residencyEnabled=f.cpuEnabled=f.hwQueuesEnabled=f.submitEnabled=f.asyncSubmitEnabled=
            f.syncEnabled=f.hwQueueSignalEnabled=f.contextSignalEnabled=true;
        Session s(f); s.dispatch(hello());
        require(!header(s.dispatch(request(Op::BeginOpenGl,0,5u))).status);
        require(s.vendorObjectLimit()==512 && !s.requestLimit() && !s.openGlCommandLimit() && !s.hwQueueSignalLimit());
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        auto contextPacket=request(Op::CreateContext,device,ContextDesc{0,1,16,12,4,0});
        contextPacket.insert(contextPacket.end(),{1,2,3,4});
        const auto context=header(s.dispatch(contextPacket)).handle;
        auto queuePacket=request(Op::CreateHwQueue,context,HwQueueDesc{0,4,0,0});
        queuePacket.insert(queuePacket.end(),{37,38,39,40});
        const auto queue=header(s.dispatch(queuePacket)).handle;
        const auto paging=header(s.dispatch(request(Op::CreatePagingQueue,device))).handle;
        auto allocationPacket=request(Op::CreateVendorAllocation,device,VendorAllocationDesc{0,0,0,1,0,0});
        allocationPacket.push_back(37);
        const auto allocation=header(s.dispatch(allocationPacket)).handle;
        require(adapter && device && context && queue && paging && allocation);
        require(!header(s.dispatch(request(Op::MapVendorAllocation,allocation,GpuVaDesc{paging,0,65536,0,0,0,16,1,0}))).status);
        auto residency=request(Op::MakeVendorResident,paging,ResidentDesc{1,1,0,0});
        const auto allocationBytes=reinterpret_cast<const std::uint8_t*>(&allocation);
        residency.insert(residency.end(),allocationBytes,allocationBytes+4);
        require(!header(s.dispatch(residency)).status);
        require(!header(s.dispatch(request(Op::LockVendorAllocation,allocation,VendorCpuDesc{device,0}))).status);
        const auto sync=header(s.dispatch(request(Op::CreateSync,device,SyncDesc{5,128,0,0,0}))).handle;
        require(sync);
        for(std::uint64_t sequence=1;sequence<=500;++sequence) {
            auto command=request(Op::QueueHwCommand,queue,HwSubmitDesc{65536,sequence,4096,4,0,0});
            command.insert(command.end(),{37,38,39,40});
            require(!header(s.dispatch(command)).status);
            auto signal=request(Op::QueueHwQueueSignal,sync,HwQueueSignalDesc{4,1,sequence*2-1});
            const auto bytes=reinterpret_cast<const std::uint8_t*>(&queue); signal.insert(signal.end(),bytes,bytes+4);
            require(!header(s.dispatch(signal)).status);
            require(!header(s.dispatch(request(Op::SignalContextSync,context,ContextSignalDesc{sync,4,sequence*2}))).status);
        }
        const auto before=f.calls;
        auto replay=request(Op::QueueHwCommand,queue,HwSubmitDesc{65536,500,4096,4,0,0}); replay.insert(replay.end(),{37,38,39,40});
        require(header(s.dispatch(replay)).status==-22 && f.calls==before);
        const auto activity=s.acceptedSubmissionActivity();
        require(activity.exact && activity.acceptedCommands==500 && activity.asynchronousCommands==500 && activity.acceptedCommandBytes==500*4096);
        require(MaxHwSubmissions==16 && MaxHwQueueSignals==16 && MaxVendorCpuSlots==256);
        std::cout<<"OPENGL_DESKTOP_PROFILE rollingCommands=500 rollingHwSignals=500 rollingContextSignals=500 replayRejected=true byteBudgetsUnchanged=true gpu=false\n";
    }
    for(unsigned version:{0u,1u,2u,3u}) {
        OpenGlFake f; f.contextsEnabled=f.hwQueuesEnabled=f.hwQueueSignalEnabled=true;
        f.profile=version==3 ? GuestOpenGlCompositorProfile : version==2 ? GuestOpenGlFenceProfile : GuestOpenGlProfile;
        Session s(f); s.dispatch(hello());
        if(version) require(!header(s.dispatch(version>=2 ? request(Op::BeginOpenGl,0,version) : request(Op::BeginOpenGl))).status);
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        auto contextPacket=request(Op::CreateContext,device,ContextDesc{0,1,16,12,4,0});
        contextPacket.insert(contextPacket.end(),{1,2,3,4});
        const auto context=header(s.dispatch(contextPacket)).handle;
        auto queuePacket=request(Op::CreateHwQueue,context,HwQueueDesc{0,4,0,0});
        queuePacket.insert(queuePacket.end(),{37,38,39,40});
        const auto queue=header(s.dispatch(queuePacket)).handle;
        const auto sync=header(s.dispatch(request(Op::CreateSync,device,SyncDesc{5,128,0,0,0}))).handle;
        require(context && queue && sync && s.hwQueueSignalLimit()==(version==3 ? 256u : version==2 ? 64u : 16u));
        const auto signal=[&](std::uint64_t target) {
            auto packet=request(Op::QueueHwQueueSignal,sync,HwQueueSignalDesc{4,1,target});
            const auto bytes=reinterpret_cast<const std::uint8_t*>(&queue); packet.insert(packet.end(),bytes,bytes+sizeof queue);
            return s.dispatch(packet);
        };
        for(unsigned n=1;n<=s.hwQueueSignalLimit();++n) require(!header(signal(n)).status);
        const auto calls=f.calls;
        require(header(signal(s.hwQueueSignalLimit()+1)).status==-24 && f.calls==calls);
    }
    {
        OpenGlFake f; f.reservationEnabled = true; Session s(f);
        require(header(s.dispatch(request(Op::BeginOpenGl))).status == -71 && !f.begins);
        s.dispatch(hello());
        require(header(s.dispatch(request(Op::BeginOpenGl, 1))).status == -22);
        auto longPacket = request(Op::BeginOpenGl); longPacket.push_back(0);
        require(header(s.dispatch(longPacket)).status == -22 && !f.begins);
        for (const auto missing : {AsyncHwSubmitCapability, SyncCapability, GpuReservationCapability, UINT32_C(0x80000000)}) {
            f.missing = missing;
            require(header(s.dispatch(request(Op::BeginOpenGl))).status == -95 && !f.begins);
        }
        f.missing = 0;
        for (unsigned field = 0; field < 8; ++field) {
            std::uint32_t values[8]{}; std::memcpy(values, &GuestOpenGlProfile, sizeof values); ++values[field];
            std::memcpy(&f.profile, values, sizeof values);
            require(!validOpenGlProfile(f.profile) && header(s.dispatch(request(Op::BeginOpenGl))).status == -95);
        }
        f.profile = GuestOpenGlProfile;
        const auto out = s.dispatch(request(Op::BeginOpenGl)); OpenGlProfile received{};
        require(!header(out).status && out.size() == sizeof(Header) + sizeof received);
        std::memcpy(&received, out.data() + sizeof(Header), sizeof received); require(validOpenGlProfile(received));
        const auto before = f.begins;
        require(header(s.dispatch(request(Op::BeginOpenGl))).status == -16 && f.begins == before);
        require(header(s.dispatch(request(Op::BeginStreaming))).status == -16);
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        for (unsigned n = 0; n < GuestOpenGlProfile.reservationLimit; ++n)
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{0, 67108864, 1ull << 40, 65536}))).handle != 0);
        const auto callsBefore = f.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{0, 67108864, 1ull << 40, 65536}))).status == -24 && f.calls == callsBefore);
        require(MaxGpuReservations == 8 && MaxGpuReservedBytes == GuestOpenGlProfile.reservedBytes && MaxHwSubmissions == 16);
    }
    {
        OpenGlFake f; Session s(f); s.dispatch(hello());
        require(header(s.dispatch(request(Op::OpenAdapter))).handle != 0);
        require(header(s.dispatch(request(Op::BeginOpenGl))).status == -16 && !f.begins);
    }
    {
        OpenGlFake f; Session s(f);
        require(s.objectLimit() == MaxObjects);
        require(header(s.dispatch(request(Op::BeginOpenGl, 0, std::uint32_t{2}))).status == -71);
        s.dispatch(hello());
        for (const auto version : {0u, 1u, 11u, UINT32_MAX})
            require(header(s.dispatch(request(Op::BeginOpenGl, 0, version))).status == -93 && !f.begins);
        for (unsigned field = 0; field < 8; ++field) {
            std::uint32_t words[8]{}; std::memcpy(words, &GuestOpenGlFenceProfile, sizeof words); ++words[field];
            std::memcpy(&f.profile, words, sizeof words);
            require(header(s.dispatch(request(Op::BeginOpenGl, 0, std::uint32_t{2}))).status == -95);
            require(s.objectLimit() == MaxObjects);
        }
        f.profile = GuestOpenGlProfile;
        require(header(s.dispatch(request(Op::BeginOpenGl, 0, std::uint32_t{2}))).status == -95);
        f.profile = GuestOpenGlFenceProfile;
        const auto out = s.dispatch(request(Op::BeginOpenGl, 0, std::uint32_t{2}));
        OpenGlProfile profile{};
        require(!header(out).status && out.size() == sizeof(Header) + sizeof profile);
        std::memcpy(&profile, out.data() + sizeof(Header), sizeof profile);
        require(validOpenGlFenceProfile(profile) && s.objectLimit() == 384 && MaxObjects == 256);
        const auto before = f.begins;
        require(header(s.dispatch(request(Op::BeginOpenGl))).status == -16 && f.begins == before);
        require(header(s.dispatch(request(Op::BeginOpenGl, 0, std::uint32_t{2}))).status == -16 && f.begins == before);
        require(header(s.dispatch(request(Op::BeginStreaming))).status == -16);
        for (std::size_t i = 0; i < s.objectLimit(); ++i)
            require(header(s.dispatch(request(Op::OpenAdapter))).handle != 0);
        const auto calls = f.calls;
        require(s.objectCount() == 384 && header(s.dispatch(request(Op::OpenAdapter))).status == -24 && f.calls == calls);
    }
    {
        OpenGlFake f; f.profile=GuestOpenGlDesktop4kCpuProfile; Session s(f); s.dispatch(hello());
        require(header(s.dispatch(request(Op::BeginOpenGl,0,std::uint32_t{7}))).status==-22 && !f.begins);
        for(unsigned field=0;field<8;++field) {
            auto capacity=GuestOpenGlDesktop4kCapacity;
            std::uint32_t words[8]{}; std::memcpy(words,&capacity,sizeof words); ++words[field];
            std::memcpy(&capacity,words,sizeof words);
            require(header(s.dispatch(request(Op::BeginOpenGl,0,capacity))).status<0 && !f.begins);
        }
        auto legacy=GuestOpenGlDesktop4kCapacity; legacy.version=6;
        require(header(s.dispatch(request(Op::BeginOpenGl,0,legacy))).status==-22 && !f.begins);
        for(const auto missing:{ExpandedVendorGpuCapability,ExpandedVendorCpuCapability}) {
            f.missing=missing;
            require(header(s.dispatch(request(Op::BeginOpenGl,0,GuestOpenGlDesktop4kCapacity))).status==-95 && !f.begins);
        }
        require(vendorCpuByteLimit(ExpandedVendorCpuCapability)==ExpandedVendorCpuBytes);
        require(vendorCpuByteLimit(ExpandedVendorCpuCapability,true)==192u*1024*1024);
        require(vendorCpuByteLimit(0,true)==MaxAllocatedBytes);
        require(!vendorCpuBudgetFits(96u*1024*1024,4096,ExpandedVendorCpuCapability));
        require(vendorCpuBudgetFits(96u*1024*1024,96u*1024*1024,ExpandedVendorCpuCapability,true));
        require(!vendorCpuBudgetFits(192u*1024*1024,4096,ExpandedVendorCpuCapability,true));
        require(!vendorCpuBudgetFits(UINT32_MAX,4096,ExpandedVendorCpuCapability,true));
    }
    {
        require(!validVendorCpuSlots(512) && validVendorCpuSlots(512,true));
        require(!validVendorCpuSlots(1024,true));
        const auto high=511ull*VendorCpuSlotBytes;
        require(!validVendorCpuReply(high,{4096,0,1}));
        require(validVendorCpuReply(high,{4096,0,1},true));
        require(!validVendorCpuReply(512ull*VendorCpuSlotBytes,{4096,0,1},true));
        require(!validVendorCpuReply(high,{MaxVendorCpuMappingBytes,0,1},true));
        OpenGlFake f; f.profile=GuestOpenGlDesktopSparseProfile; Session s(f);s.dispatch(hello());
        require(header(s.dispatch(request(Op::BeginOpenGl,0,std::uint32_t{8}))).status==-22 && !f.begins);
        for(unsigned field=0;field<8;++field) {
            auto capacity=GuestOpenGlDesktopSparseCapacity;
            std::uint32_t words[8]{};std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
            require(header(s.dispatch(request(Op::BeginOpenGl,0,capacity))).status<0 && !f.begins);
        }
        auto old=GuestOpenGlDesktopSparseCapacity;old.version=7;
        require(header(s.dispatch(request(Op::BeginOpenGl,0,old))).status==-22 && !f.begins);
        for(const auto missing:{ExpandedVendorGpuCapability,ExpandedVendorCpuCapability}) {
            f.missing=missing;
            require(header(s.dispatch(request(Op::BeginOpenGl,0,GuestOpenGlDesktopSparseCapacity))).status==-95 && !f.begins);
        }
    }
    // Native CPU receipts above the published 256-slot ceiling are authorized
    // only after the exact v8 profile. Real native calls are explicit fakes here.
    for(const auto version:{7u,8u}) {
        OpenGlFake f;f.profile=version==8 ? GuestOpenGlDesktopSparseProfile : GuestOpenGlDesktop4kCpuProfile;
        f.vendorEnabled=f.gpuVaEnabled=f.cpuEnabled=true;f.badCpuReply=4;
        Session s(f);s.dispatch(hello());
        require(!header(s.dispatch(request(Op::BeginOpenGl,0,version==8 ? GuestOpenGlDesktopSparseCapacity : GuestOpenGlDesktop4kCapacity))).status);
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto paging=header(s.dispatch(request(Op::CreatePagingQueue,device))).handle;
        auto create=request(Op::CreateVendorAllocation,device,VendorAllocationDesc{0,0,0,1,0,0});create.push_back(1);
        const auto allocation=header(s.dispatch(create)).handle;require(allocation);
        require(!header(s.dispatch(request(Op::MapVendorAllocation,allocation,GpuVaDesc{paging,0,65536,0,0,0,16,1,0}))).status);
        const auto locked=s.dispatch(request(Op::LockVendorAllocation,allocation,VendorCpuDesc{device,0}));
        require(header(locked).status==(version==8 ? 0 : -5));
        if(version==8) require(!header(s.dispatch(request(Op::UnlockVendorAllocation,allocation,VendorCpuDesc{device,0}))).status);
    }
    {
        const auto flags=ExpandedVendorGpuCapability;
        require(!vendorGpuBudgetFits(130090,8160,flags,true));
        require(vendorGpuBudgetFits(130090,8160,flags,true,true));
        require(vendorGpuPageLimit(flags,true)==OpenGlDesktop4kGpuMappedBytes/4096);
        require(vendorGpuPageLimit(flags,true,true)==OpenGlDesktopLargeMapBytes/4096);
        require(vendorGpuPageLimit(flags,false,true)==vendorGpuPageLimit(flags));
        require(vendorGpuPageLimit(0,true,true)==vendorGpuPageLimit(0));
        require(!vendorGpuBudgetFits(OpenGlDesktopLargeMapBytes/4096,1,flags,true,true));
        require(!vendorGpuBudgetFits(UINT32_MAX,1,flags,true,true));
        OpenGlFake f;f.profile=GuestOpenGlDesktopLargeMapProfile;Session s(f);s.dispatch(hello());
        require(header(s.dispatch(request(Op::BeginOpenGl,0,std::uint32_t{9}))).status==-22 && !f.begins);
        auto old=GuestOpenGlDesktopSparseCapacity;old.version=9;
        require(header(s.dispatch(request(Op::BeginOpenGl,0,old))).status==-22 && !f.begins);
        for(unsigned field=0;field<10;++field) {
            auto capacity=GuestOpenGlDesktopLargeMapCapacity;
            std::uint32_t words[10]{};std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
            require(header(s.dispatch(request(Op::BeginOpenGl,0,capacity))).status<0 && !f.begins);
        }
        for(const auto missing:{ExpandedVendorGpuCapability,ExpandedVendorCpuCapability}) {
            f.missing=missing;
            require(header(s.dispatch(request(Op::BeginOpenGl,0,GuestOpenGlDesktopLargeMapCapacity))).status==-95 && !f.begins);
        }
    }
    {
        static_assert(sizeof(OpenGlDesktopPresentationCapacity)==48,"fixed v10");
        const std::uint32_t golden[12]{10,1,201326592,0,512,256,0,0,1073741824,0,1073741824,0};
        require(!std::memcmp(golden,&GuestOpenGlDesktopPresentationCapacity,sizeof golden));
        require(!reportedGpuBudgetFits(831991808,0,536870912));
        require(reportedGpuBudgetFits(831991808,33226752,1073741824));
        require(reportedGpuBudgetFits(1073741824,0,1073741824));
        require(!reportedGpuBudgetFits(1073741824,1,1073741824));
        require(!reportedGpuBudgetFits(1,UINT64_MAX,1073741824));
        require(!reportedGpuBudgetFits(UINT64_MAX,1,1073741824));
        OpenGlFake f;f.profile=GuestOpenGlDesktopPresentationProfile;Session s(f);s.dispatch(hello());
        require(header(s.dispatch(request(Op::BeginOpenGl,0,std::uint32_t{10}))).status==-22 && !f.begins);
        auto small=GuestOpenGlDesktopLargeMapCapacity;small.aperture.version=10;
        require(header(s.dispatch(request(Op::BeginOpenGl,0,small))).status==-22 && !f.begins);
        for(unsigned field=0;field<12;++field) {
            auto capacity=GuestOpenGlDesktopPresentationCapacity;
            std::uint32_t words[12]{};std::memcpy(words,&capacity,sizeof words);++words[field];std::memcpy(&capacity,words,sizeof words);
            require(header(s.dispatch(request(Op::BeginOpenGl,0,capacity))).status<0 && !f.begins);
        }
        for(const auto missing:{ExpandedVendorGpuCapability,ExpandedVendorCpuCapability}) {
            f.missing=missing;
            require(header(s.dispatch(request(Op::BeginOpenGl,0,GuestOpenGlDesktopPresentationCapacity))).status==-95 && !f.begins);
        }
        std::cout<<"NATIVE_PRESENTATION_BUDGET fixed48ByteReceipt=true exact1GiBUsage=true overflowRejected=true oldGuardsUnchanged=true gpu=false\n";
    }
    for(const auto version:{3u,4u,5u,6u,7u,8u,9u,10u}) {
        const auto expected=version==10 ? GuestOpenGlDesktopPresentationProfile : version==9 ? GuestOpenGlDesktopLargeMapProfile : version==8 ? GuestOpenGlDesktopSparseProfile : version==7 ? GuestOpenGlDesktop4kCpuProfile : version==6 ? GuestOpenGlDesktop4kProfile : version==5 ? GuestOpenGlDesktopProfile : version==4 ? GuestOpenGlCompositorRenderProfile : GuestOpenGlCompositorProfile;
        OpenGlFake f; f.reservationEnabled=true; Session s(f);
        const auto begin=version==10 ? request(Op::BeginOpenGl,0,GuestOpenGlDesktopPresentationCapacity) : version==9 ? request(Op::BeginOpenGl,0,GuestOpenGlDesktopLargeMapCapacity) : version==8 ? request(Op::BeginOpenGl,0,GuestOpenGlDesktopSparseCapacity) : version==7 ? request(Op::BeginOpenGl,0,GuestOpenGlDesktop4kCapacity) : request(Op::BeginOpenGl,0,version);
        require(header(s.dispatch(begin)).status==-71 && !f.begins);
        s.dispatch(hello());
        for (unsigned field=0;field<8;++field) {
            std::uint32_t words[8]{}; std::memcpy(words,&expected,sizeof words); ++words[field];
            std::memcpy(&f.profile,words,sizeof words);
            require(header(s.dispatch(begin)).status==-95);
            require(s.objectLimit()==256 && s.syncObjectLimit()==96 && s.requestLimit()==10000);
        }
        f.profile=GuestOpenGlFenceProfile;
        require(header(s.dispatch(begin)).status==-95);
        f.profile=expected;
        const auto out=s.dispatch(begin); OpenGlProfile profile{};
        require(!header(out).status && out.size()==sizeof(Header)+sizeof profile);
        std::memcpy(&profile,out.data()+sizeof(Header),sizeof profile);
        require((version==10 ? validOpenGlDesktopPresentationProfile(profile) : version==9 ? validOpenGlDesktopLargeMapProfile(profile) : version==8 ? validOpenGlDesktopSparseProfile(profile) : version==7 ? validOpenGlDesktop4kCpuProfile(profile) : version==6 ? validOpenGlDesktop4kProfile(profile) : version==5 ? validOpenGlDesktopProfile(profile) : version==4 ? validOpenGlCompositorRenderProfile(profile) : validOpenGlCompositorProfile(profile)) &&
                s.objectLimit()==expected.objectLimit && s.syncObjectLimit()==192 &&
                s.requestLimit()==expected.requestLimit && s.openGlCommandLimit()==expected.commandLimit && s.reservationLimit()==expected.reservationLimit);
        const auto before=f.begins;
        require(header(s.dispatch(begin)).status==-16 && f.begins==before);
        require(header(s.dispatch(request(Op::BeginStreaming))).status==-16);
        require(!header(s.dispatch(hello())).status);
        require(s.objectLimit()==256 && s.syncObjectLimit()==96 && s.requestLimit()==10000);
        require(s.reservationLimit()==8);
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle; require(adapter);
        require(header(s.dispatch(begin)).status==-16 && f.begins==before);
        Session reservation(f); reservation.dispatch(hello()); require(!header(reservation.dispatch(begin)).status);
        const auto owner=header(reservation.dispatch(request(Op::OpenAdapter))).handle;
        const auto reserve=request(Op::ReserveGpuAddress,owner,GpuReservationDesc{0,67108864,1ull<<40,65536});
        for(std::size_t n=0;n<expected.reservationLimit;++n) require(header(reservation.dispatch(reserve)).handle);
        const auto calls=f.calls;
        require(header(reservation.dispatch(reserve)).status==-24 && f.calls==calls && MaxGpuReservations==8);
    }
    for (const auto version : {0u,1u,2u,3u}) {
        OpenGlFake f; f.syncEnabled=true;
        f.profile=version==3 ? GuestOpenGlCompositorProfile : version==2 ? GuestOpenGlFenceProfile : GuestOpenGlProfile;
        Session s(f); s.dispatch(hello());
        if(version) require(!header(s.dispatch(version>=2 ? request(Op::BeginOpenGl,0,version) : request(Op::BeginOpenGl))).status);
        require(s.syncObjectLimit()==(version==3 ? 192u : 96u));
        const auto adapter=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto create=request(Op::CreateSync,device,SyncDesc{1,0,0,0,0});
        std::uint32_t last=0;
        for(std::size_t n=0;n<s.syncObjectLimit();++n) { last=header(s.dispatch(create)).handle; require(last); }
        const auto before=f.calls;
        require(header(s.dispatch(create)).status==-24 && f.calls==before);
        require(!header(s.dispatch(request(Op::DestroySync,last))).status);
        require(header(s.dispatch(create)).handle && MaxSyncObjects==96);
    }
    for (const auto version : {1u, 2u, 3u, 5u}) {
        OpenGlFake f;
        f.contextsEnabled = f.hwQueuesEnabled = f.vendorEnabled = f.expandedVendorObjectsEnabled = true;
        f.profile = version == 5 ? GuestOpenGlDesktopProfile : version == 3 ? GuestOpenGlCompositorProfile : version == 2 ? GuestOpenGlFenceProfile : GuestOpenGlProfile;
        Session s(f); s.dispatch(hello());
        require(!header(s.dispatch(version >= 2 ? request(Op::BeginOpenGl, 0, version) : request(Op::BeginOpenGl))).status);
        require(s.hwQueueLimit() == (version >= 2 ? 16u : 8u));
        require(s.vendorObjectLimit() == (version == 5 ? 512u : version == 3 ? 256u : version == 2 ? 192u : 128u));
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto contextPacket = request(Op::CreateContext, device, ContextDesc{0,1,16,12,4,0});
        contextPacket.insert(contextPacket.end(), {1,2,3,4});
        const auto context = header(s.dispatch(contextPacket)).handle;
        auto queuePacket = request(Op::CreateHwQueue, context, HwQueueDesc{0,4,0,0});
        queuePacket.insert(queuePacket.end(), {37,38,39,40});
        for (std::size_t i = 0; i < s.hwQueueLimit(); ++i) require(header(s.dispatch(queuePacket)).handle != 0);
        auto before = f.calls;
        require(header(s.dispatch(queuePacket)).status == -24 && f.calls == before);
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0,0,0,1,0,0});
        allocationPacket.push_back(37);
        for (std::size_t i = 0; i < s.vendorObjectLimit(); ++i) require(header(s.dispatch(allocationPacket)).handle != 0);
        before = f.calls;
        require(header(s.dispatch(allocationPacket)).status == -24 && f.calls == before);
        require(MaxHwQueues == 8 && ExpandedVendorAllocations == 128);
    }
    struct StreamFake : Fake {
        StreamProfile profile = GuestStreamProfile;
        unsigned begins = 0;
        std::uint32_t missing = 0;
        native_gpu::Capabilities capabilities() const override {
            auto caps = Fake::capabilities();
            caps.flags |= NativeSharedTextureCapability | SharedTextureConsumeCapability | AsyncHwSubmitCapability | UINT32_C(0x80000000);
            caps.flags &= ~missing; return caps;
        }
        StreamProfile beginStreaming() override { ++begins; return profile; }
    };
    {
        StreamFake f; Session s(f);
        require(s.requestLimit() == 10000);
        require(header(s.dispatch(request(Op::BeginStreaming))).status == -71 && f.begins == 0);
        s.dispatch(hello());
        require(header(s.dispatch(request(Op::ConsumeStreamingTexture, 1, SharedTextureConsumeDesc{2,0,1}))).status == -71);
        require(header(s.dispatch(request(Op::BeginStreaming, 1))).status == -22);
        auto oversized = request(Op::BeginStreaming); oversized.push_back(0);
        require(header(s.dispatch(oversized)).status == -22 && f.begins == 0);
        for (const auto missing : {NativeSharedTextureCapability, SharedTextureConsumeCapability, AsyncHwSubmitCapability, UINT32_C(0x80000000)}) {
            f.missing = missing;
            require(header(s.dispatch(request(Op::BeginStreaming))).status == -95 && f.begins == 0);
        }
        f.missing = 0;
        for (unsigned field = 0; field < 8; ++field) {
            std::uint32_t values[8]{}; std::memcpy(values, &GuestStreamProfile, sizeof values); ++values[field];
            std::memcpy(&f.profile, values, sizeof values);
            require(!validStreamProfile(f.profile) && header(s.dispatch(request(Op::BeginStreaming))).status == -95);
        }
        f.profile = GuestStreamProfile;
        const auto out = s.dispatch(request(Op::BeginStreaming)); StreamProfile profile{};
        require(!header(out).status && !header(out).handle && out.size() == sizeof(Header) + sizeof profile);
        std::memcpy(&profile, out.data() + sizeof(Header), sizeof profile); require(validStreamProfile(profile));
        require(s.requestLimit() == 50000);
        const auto before = f.begins;
        require(header(s.dispatch(request(Op::BeginStreaming))).status == -16 && f.begins == before);
        require(header(s.dispatch(request(Op::ConsumeSharedTexture, 1, SharedTextureConsumeDesc{2,0,1}))).status == -71);
        require(header(s.dispatch(request(Op::ConsumeStreamingTexture, 1, SharedTextureConsumeDesc{2,0,1}))).status == -9);
        require(streamAuditRound(1) == 1 && streamAuditRound(64) == 2);
        for (unsigned frame = 2; frame < 64; ++frame) require(streamAuditRound(frame) == 0);
    }
    {
        // Negotiation expands lifetime budget, never resets prior attempts.
        StreamFake f; f.syncEnabled = f.contextSignalEnabled = f.hwQueuesEnabled = f.hwQueueSignalEnabled = true;
        Session s(f); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice,a))).handle;
        auto context = request(Op::CreateContext,d,ContextDesc{0,1,16,12,4,0}); context.insert(context.end(),{1,2,3,4});
        const auto c = header(s.dispatch(context)).handle;
        const auto fence = header(s.dispatch(request(Op::CreateSync,d,SyncDesc{5,NoGpuAccessSyncFlag,0,0,0}))).handle;
        auto queue = request(Op::CreateHwQueue,c,HwQueueDesc{0,4,0,0}); queue.insert(queue.end(),{37,38,39,40});
        const auto q = header(s.dispatch(queue)).handle; require(q != 0);
        require(a && d && c && fence);
        require(!header(s.dispatch(request(Op::SignalContextSync,c,ContextSignalDesc{fence,4,1}))).status);
        require(!header(s.dispatch(request(Op::BeginStreaming))).status);
        for (unsigned target = 2; target <= GuestStreamProfile.totalSignals; ++target)
            require(!header(s.dispatch(request(Op::SignalContextSync,c,ContextSignalDesc{fence,4,target}))).status);
        const auto before = f.calls;
        require(header(s.dispatch(request(Op::SignalContextSync,c,ContextSignalDesc{fence,4,257}))).status == -24 && f.calls == before);
        require(f.contextSignalCalls == GuestStreamProfile.totalSignals);
        auto signal = request(Op::QueueHwQueueSignal,fence,HwQueueSignalDesc{0,1,257});
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&q); signal.insert(signal.end(),bytes,bytes+4);
        require(header(s.dispatch(signal)).status == -24 && f.calls == before && f.hwQueueSignalCalls == 0);
    }
    for (const auto cap : {0u,AsyncHwSubmitCapability}) {
        require(validHwSubmitReply({0,0,5},5,cap));
        require(validHwSubmitReply({0,0,6},5,cap));
        require(validHwSubmitReply({-123,0,0},5,cap));
        require(!validHwSubmitReply({-123,0,1},5,cap));
        require(!validHwSubmitReply({259,0,5},5,cap));
        require(!validHwSubmitReply({0,99,5},5,cap));
        require(!validHwSubmitReply({0,0,UINT64_MAX},5,cap));
        require(!validHwSubmitReply({0,0,5},0,cap));
        require(!validHwSubmitReply({0,0,5},UINT64_MAX,cap));
        require(validHwSubmitReply({0,0,0},5,cap) == (cap != 0));
        require(validHwSubmitReply({0,0,4},5,cap) == (cap != 0));
    }
    {
        GpuStateRanges ranges; GpuStateRanges::Plan plan;
        const auto page = 4096ull, base = 67108864ull;
        using Scope=GpuStateRanges::RetirementScope;
        require(ranges.retirementScope(1,base,page,5,false)==Scope::PagingFence);
        require(ranges.retirementScope(1,base,page,5,true)==Scope::OwningDevice);
        require(GpuStateRanges::retirementCoversQueue(Scope::OwningDevice,1,1));
        require(!GpuStateRanges::retirementCoversQueue(Scope::OwningDevice,1,2));
        require(GpuStateRanges::retirementCoversQueue(Scope::AllDevices,1,2));
        require(!GpuStateRanges::retirementCoversQueue(Scope::PagingFence,1,1));
        require(GpuStateRanges::retirementCoversQueue(static_cast<Scope>(99),1,2));
        require(!ranges.requiresRetirement(1,base,page,5,false));
        require(ranges.requiresRetirement(1,base,page,5,true));
        require(ranges.prepare(1, base, 4 * page, 5, plan)); ranges.commit(plan);
        require(ranges.retirementScope(1,base,page,8,false)==Scope::AllDevices);
        require(ranges.retirementScope(1,base,page,8,true)==Scope::AllDevices);
        require(ranges.retirementScope(2,base,page,5,true)==Scope::AllDevices);
        require(!ranges.requiresRetirement(1,base,4*page,5,false));
        require(!ranges.requiresRetirement(1,base+page,page,5,false));
        require(!ranges.requiresRetirement(1,base+4*page,page,8,false));
        require(ranges.requiresRetirement(1,base+3*page,2*page,8,false));
        require(ranges.requiresRetirement(2,base,page,5,false));
        require(ranges.requiresRetirement(1,base,page,5,true));
        require(ranges.prepare(1, base + page, 2 * page, 8, plan)); ranges.commit(plan);
        require(ranges.requiresRetirement(1,base,4*page,5,false));
        require(ranges.size() == 3 && ranges.bytes() == 4 * page);
        require(!ranges.prepare(2, base + page, page, 5, plan) && ranges.size() == 3);
        require(ranges.prepare(1, base + page, 2 * page, 5, plan)); ranges.commit(plan);
        require(ranges.size() == 1 && ranges.bytes() == 4 * page);
        require(ranges.prepareRemove(base + page, 2 * page, plan)); ranges.commit(plan);
        require(ranges.size() == 2 && ranges.bytes() == 2 * page);
        require(!ranges.requiresRetirement(1,base+page,2*page,8,false));
        require(ranges.requiresRetirement(1,base+page,3*page,8,false));
        ranges.release(2); require(ranges.size() == 2); ranges.release(1); require(ranges.empty());
        require(ranges.prepare(1, base, GpuStateRanges::ByteLimit, 4, plan)); ranges.commit(plan);
        require(!ranges.prepare(1, base + GpuStateRanges::ByteLimit, page, 4, plan));
        require(ranges.size() == 1 && ranges.bytes() == GpuStateRanges::ByteLimit);
        ranges.release(1);
        for (std::size_t i = 0; i < GpuStateRanges::Limit; ++i) {
            require(ranges.prepare(1, base + 2 * i * page, page, 4, plan)); ranges.commit(plan);
        }
        require(!ranges.prepare(1, base + 2 * GpuStateRanges::Limit * page, page, 4, plan));
        require(ranges.size() == GpuStateRanges::Limit && ranges.bytes() == GpuStateRanges::Limit * page);
        ranges.release(1); require(ranges.empty());
    }
    Fake state; state.reservationEnabled = state.vendorEnabled = state.gpuVaEnabled = true;
    {
        Session s(state);
        GpuVaDesc desc{3, 0, 67108864, 0, 0, 0, 16, 5, 0};
        require(header(s.dispatch(request(Op::MapGpuState, 4, desc))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        desc.queue = queue;
        const auto owned = header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{desc.base, 0, 0, 65536}))).handle;
        auto before = state.calls;
        require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -95 && state.calls == before);
        state.gpuStateEnabled = true;
        for (unsigned n = 0; n < 17; ++n) {
            auto bad = desc;
            switch (n) {
                case 0: bad.base = 0; break;
                case 1: bad.base++; break;
                case 2: bad.offsetPages = 1; break;
                case 3: bad.protection = 12; break;
                case 4: bad.protection = 16; break;
                case 5: bad.protection = 1; break;
                case 6: bad.protection = UINT64_MAX; break;
                case 7: bad.sizePages = 0; break;
                case 8: bad.sizePages = MaxGpuReservationBytes / 4096 + 1; break;
                case 9: bad.base = MaxGpuAddress - 4096; break;
                case 10: bad.reserved = 1; break;
                case 11: bad.driverProtection = 1; break;
                case 12: bad.minimum = 1; break;
                case 13: bad.maximum = MaxGpuAddress + 4096; break;
                case 14: bad.minimum = MaxGpuAddress; break;
                case 15: bad.maximum = 1; break;
                case 16: bad.queue = 0; break;
            }
            require(header(s.dispatch(request(Op::MapGpuState, owned, bad))).status == -22 && state.calls == before);
        }
        auto shortPacket = request(Op::MapGpuState, owned, desc); shortPacket.pop_back();
        require(header(s.dispatch(shortPacket)).status == -22);
        auto foreign = desc; foreign.base += 65536;
        require(header(s.dispatch(request(Op::MapGpuState, owned, foreign))).status == -9);
        require(header(s.dispatch(request(Op::MapGpuState, device, desc))).status == -9);
        require(header(s.dispatch(request(Op::MapGpuState, 501, desc))).status == -9);
        const auto other = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto otherDevice = header(s.dispatch(request(Op::CreateDevice, other))).handle;
        foreign = desc; foreign.queue = header(s.dispatch(request(Op::CreatePagingQueue, otherDevice))).handle;
        before = state.calls;
        require(header(s.dispatch(request(Op::MapGpuState, owned, foreign))).status == -9 && state.calls == before);
        for (int n = 1; n <= 4; ++n) {
            state.badGpuStateReply = n;
            require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -5);
        }
        state.fail = true;
        for (int n = 5; n <= 6; ++n) {
            state.badGpuStateReply = n;
            require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -5);
        }
        state.badGpuStateReply = 0;
        auto packet = s.dispatch(request(Op::MapGpuState, owned, desc)); Reply nt{}; GpuVaReply fence{};
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(!header(packet).status && nt.ntstatus == -123 && !nt.value);
        state.fail = false;
        for (const auto protection : {4ull, 5ull, 6ull, 7ull, 8ull, 9ull, 10ull, 11ull}) {
            desc.protection = protection; packet = s.dispatch(request(Op::MapGpuState, owned, desc));
            std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
            std::memcpy(&fence, packet.data() + sizeof(Header) + sizeof(Reply), sizeof fence);
            require(!header(packet).status && header(packet).handle == owned && nt.ntstatus == 259 && nt.value == desc.base && fence.fence == 7019);
        }
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 4, 0, 0});
        allocationPacket.insert(allocationPacket.end(), 4, 0);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        auto allocationDesc = desc; allocationDesc.protection = 1;
        require(!header(s.dispatch(request(Op::MapVendorAllocation, allocation, allocationDesc))).status);
        before = state.calls;
        auto partial = desc; partial.sizePages = 8;
        require(header(s.dispatch(request(Op::MapGpuState, owned, partial))).status == -16 && state.calls == before);
        state.fail = true;
        packet = s.dispatch(request(Op::MapGpuState, owned, desc)); std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(!header(packet).status && nt.ntstatus == -123);
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, allocationDesc))).status == -16);
        state.fail = false;
        require(!header(s.dispatch(request(Op::MapGpuState, owned, desc))).status);
        // Allocation and its CPU/residency ownership survive the page-table
        // replacement. A fresh mapping can replace those state pages again.
        require(!header(s.dispatch(request(Op::MapVendorAllocation, allocation, allocationDesc))).status && state.vendorOwners.size() == 1);
        require(!header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status);
        require(header(s.dispatch(request(Op::MapGpuState, owned, desc))).status == -9);
    }
    require(state.reservationParents.empty() && state.vendorOwners.empty());
    const GpuReservationDesc reservationInput{0, 67108864, 1ull << 40, 65536};
    Fake reservation;
    {
        Session s(reservation);
        require(header(s.dispatch(request(Op::ReserveGpuAddress, 1, reservationInput))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto other = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto before = reservation.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -95 && reservation.calls == before);
        reservation.reservationEnabled = true;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, device, reservationInput))).status == -9);
        require(header(s.dispatch(request(Op::ReserveGpuAddress, 501, reservationInput))).status == -9);
        for (unsigned n = 0; n < 11; ++n) {
            auto bad = reservationInput;
            switch (n) {
                case 0: bad.bytes = 0; break;
                case 1: ++bad.bytes; break;
                case 2: bad.bytes = MaxGpuReservationBytes + 65536; break;
                case 3: bad.base = 1; break;
                case 4: ++bad.minimum; break;
                case 5: ++bad.maximum; break;
                case 6: bad.base = MaxGpuAddress; break;
                case 7: bad.minimum = MaxGpuAddress; break;
                case 8: bad.maximum = MaxGpuAddress + 65536; break;
                case 9: bad.maximum = bad.minimum; break;
                default: bad.base = MaxGpuAddress - 65536; bad.bytes = 131072; break;
            }
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, bad))).status == -22 && reservation.calls == before);
        }
        auto shortPacket = request(Op::ReserveGpuAddress, adapter, reservationInput); shortPacket.pop_back();
        require(header(s.dispatch(shortPacket)).status == -22);
        reservation.fail = true;
        auto failure = s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput)); Reply nt{};
        std::memcpy(&nt, failure.data() + sizeof(Header), sizeof nt);
        require(!header(failure).status && !header(failure).handle && nt.ntstatus == -123 && !nt.value);
        reservation.badReservationReply = 9;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -5);
        reservation.fail = false;
        for (int n = 1; n <= 7; ++n) {
            reservation.badReservationReply = n;
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -5 && reservation.reservationParents.empty());
        }
        reservation.badReservationReply = 8;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{67108864, 0, 0, 65536}))).status == -5);
        reservation.badReservationReply = 0;
        const auto reservedPacket = s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput));
        const auto reserved = header(reservedPacket).handle;
        std::memcpy(&nt, reservedPacket.data() + sizeof(Header), sizeof nt);
        require(reserved && reserved < 500 && nt.ntstatus == 0 && validGpuReservationOutput(reservationInput, nt.value));
        before = reservation.calls;
        require(header(s.dispatch(request(Op::CloseAdapter, adapter))).status == -16);
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{nt.value, 0, 0, 65536}))).status == -16);
        require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{other, 0}))).status == -9);
        require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 1}))).status == -22);
        require(header(s.dispatch(request(Op::FreeGpuReservation, device, FreeGpuReservationDesc{adapter, 0}))).status == -9 && reservation.calls == before);
        for (int n = 1; n <= 3; ++n) {
            reservation.badReservationDestroyReply = n;
            require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}))).status == -5 && reservation.reservationParents.size() == 1);
        }
        reservation.badReservationDestroyReply = 0; reservation.fail = true;
        failure = s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}));
        std::memcpy(&nt, failure.data() + sizeof(Header), sizeof nt);
        require(!header(failure).status && nt.ntstatus == -123 && reservation.reservationParents.size() == 1);
        reservation.fail = false;
        require(!header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}))).status);
        before = reservation.calls;
        require(header(s.dispatch(request(Op::FreeGpuReservation, reserved, FreeGpuReservationDesc{adapter, 0}))).status == -9 && reservation.calls == before);
        for (std::size_t n = 0; n < MaxGpuReservations; ++n)
            require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).handle != 0);
        before = reservation.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -24 && reservation.calls == before);
    }
    require(reservation.reservationParents.empty() && reservation.destroyed.back() == Kind::Adapter);
    Fake reservationBudget; reservationBudget.reservationEnabled = true;
    {
        Session s(reservationBudget); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        auto maximumRange = reservationInput; maximumRange.bytes = MaxGpuReservationBytes;
        for (unsigned n = 0; n < 4; ++n) require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, maximumRange))).handle != 0);
        const auto before = reservationBudget.calls;
        require(header(s.dispatch(request(Op::ReserveGpuAddress, adapter, reservationInput))).status == -24 && reservationBudget.calls == before);
    }
    Fake reservationOverlap; reservationOverlap.reservationEnabled = reservationOverlap.vendorEnabled = reservationOverlap.gpuVaEnabled = true;
    {
        Session s(reservationOverlap); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 4, 0, 0});
        allocationPacket.insert(allocationPacket.end(), 4, 0);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        const auto owned = header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{67108864, 0, 0, 65536}))).handle;
        require(owned && !header(s.dispatch(request(Op::MapVendorAllocation, allocation, GpuVaDesc{queue, 0, 67108864, 0, 0, 0, 32, 1, 0}))).status);
        const auto before = reservationOverlap.calls;
        require(header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status == -16 && reservationOverlap.calls == before);
        // The reservation was created last; disconnect still destroys its
        // mapped allocation before releasing the address range and adapter.
    }
    require(reservationOverlap.reservationParents.empty() && reservationOverlap.vendorOwners.empty());
    const auto allocationDestroyed = std::find(reservationOverlap.destroyed.begin(), reservationOverlap.destroyed.end(), Kind::VendorAllocation);
    const auto rangeDestroyed = std::find(reservationOverlap.destroyed.begin(), reservationOverlap.destroyed.end(), Kind::GpuReservation);
    require(allocationDestroyed < rangeDestroyed && rangeDestroyed < reservationOverlap.destroyed.end() - 1 && reservationOverlap.destroyed.back() == Kind::Adapter);
    Fake reservationMapped; reservationMapped.reservationEnabled = reservationMapped.vendorEnabled = reservationMapped.gpuVaEnabled = true;
    {
        Session s(reservationMapped); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 4, 0, 0});
        allocationPacket.insert(allocationPacket.end(), 4, 0);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        const auto owned = header(s.dispatch(request(Op::ReserveGpuAddress, adapter, GpuReservationDesc{67108864, 0, 0, 65536}))).handle;
        const auto mapping = request(Op::MapVendorAllocation, allocation, GpuVaDesc{queue, 0, 67108864, 0, 0, 0, 16, 1, 0});
        require(owned && !header(s.dispatch(mapping)).status);
        reservationMapped.fail = true;
        auto failure = s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0})); Reply nt{};
        std::memcpy(&nt, failure.data() + sizeof(Header), sizeof nt);
        require(!header(failure).status && nt.ntstatus == -123);
        const auto before = reservationMapped.calls;
        require(header(s.dispatch(mapping)).status == -16 && reservationMapped.calls == before);
        reservationMapped.fail = false;
        require(!header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status);
        // Successful release invalidates the GPU mapping while retaining the
        // allocation itself. It can then receive a fresh GPU mapping.
        require(!header(s.dispatch(mapping)).status && reservationMapped.vendorOwners.size() == 1);
        require(header(s.dispatch(request(Op::FreeGpuReservation, owned, FreeGpuReservationDesc{adapter, 0}))).status == -9);
    }
    require(reservationMapped.reservationParents.empty() && reservationMapped.vendorOwners.empty());
    require(validVendorCpuSlots(16) && validVendorCpuSlots(32) && validVendorCpuSlots(64) && validVendorCpuSlots(128) && validVendorCpuSlots(256));
    for (const auto slots : {0u, 1u, 8u, 15u, 17u, 31u, 33u, 63u, 65u, 127u, 129u, 255u, 257u, 512u, UINT32_MAX}) require(!validVendorCpuSlots(slots));
    require(vendorCpuByteLimit(0) == 16777216 && vendorCpuByteLimit(NoBroadcastSignalHwQueueCapability) == 16777216);
    require(vendorCpuByteLimit(ExpandedVendorCpuCapability) == 33554432);
    for (const auto capabilities : {0u,ExpandedVendorCpuCapability}) {
        const auto budget = vendorCpuByteLimit(capabilities);
        require(vendorCpuBudgetFits(budget-4096,4096,capabilities));
        require(!vendorCpuBudgetFits(budget,4096,capabilities));
        require(!vendorCpuBudgetFits(budget+4096,4096,capabilities));
        require(!vendorCpuBudgetFits(UINT32_MAX,4096,capabilities));
        require(!vendorCpuBudgetFits(4096,UINT32_MAX,capabilities));
    }
    Fake driver;
    {
        Session s(driver);
        require(header(s.dispatch(request(Op::OpenAdapter))).status == -71 && driver.calls == 0);
        require(header(s.dispatch(hello(2))).status == -93);
        require(header(s.dispatch(hello())).status == 0);
        auto a = header(s.dispatch(request(Op::OpenAdapter)));
        require(a.status == 0 && a.handle == 1); // never the native handle 501
        auto d = header(s.dispatch(request(Op::CreateDevice, a.handle)));
        auto q = header(s.dispatch(request(Op::CreatePagingQueue, d.handle)));
        require(d.handle == 2 && q.handle == 3);
        const auto before = driver.calls;
        require(header(s.dispatch(request(Op::CloseAdapter, a.handle))).status == -16);
        require(header(s.dispatch(request(Op::DestroyDevice, d.handle))).status == -16);
        require(header(s.dispatch(request(Op::QueryDriverVersion, q.handle))).status == -9);
        require(header(s.dispatch(request(Op::CreateDevice, 501))).status == -9);
        require(header(s.dispatch(hello())).status == -16 && driver.calls == before);
        auto p = s.dispatch(request(Op::ReadPagingFence, q.handle));
        Reply r{}; std::memcpy(&r, p.data() + sizeof(Header), sizeof r);
        require(header(p).status == 0 && r.ntstatus == 0 && r.reserved == 0 && r.value == 42);
        Session isolated(driver); isolated.dispatch(hello());
        require(header(isolated.dispatch(request(Op::CreateDevice, a.handle))).status == -9);
        driver.fail = true;
        p = s.dispatch(request(Op::DestroyPagingQueue, q.handle));
        std::memcpy(&r, p.data() + sizeof(Header), sizeof r);
        require(header(p).status == 0 && r.ntstatus == -123);
        driver.fail = false;
        require(header(s.dispatch(request(Op::ReadPagingFence, q.handle))).status == 0);
        // Disconnect without explicit close: reverse child-before-parent cleanup.
    }
    require(driver.destroyed == std::vector<Kind>{Kind::PagingQueue, Kind::Device, Kind::Adapter});
    Fake contexts;
    {
        Session s(contexts);
        auto sync = request(Op::CreateContext, 2, ContextDesc{0, 0, 8, 0, 0, 0});
        require(header(s.dispatch(sync)).status == -71);
        s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        require(d == 2);
        auto before = contexts.calls;
        contexts.contextsEnabled = false;
        require(header(s.dispatch(sync)).status == -95 && contexts.calls == before);
        contexts.contextsEnabled = true;
        require(header(s.dispatch(request(Op::CreateContext, a, ContextDesc{0, 0, 8, 0, 0, 0}))).status == -9);
        for (const auto desc : {ContextDesc{0, 1, 4, 12, 0, 0}, ContextDesc{0, 0, 8, 0, 0, 1},
                               ContextDesc{0, 1, 16, 12, MaxContextPrivateBytes + 1, 0},
                               ContextDesc{64, 1, 16, 12, 1, 0}, ContextDesc{0, 1, 16, 12, 1, 0}})
            require(header(s.dispatch(request(Op::CreateContext, d, desc))).status == -22);
        require(contexts.calls == before);
        auto c = header(s.dispatch(sync)).handle; require(c != 0);
        require(header(s.dispatch(request(Op::DestroyDevice, d))).status == -16);
        require(header(s.dispatch(request(Op::CreatePagingQueue, c))).status == -9);
        auto p = request(Op::CreateContext, d, ContextDesc{0, 1, 16, 12, 3, 0});
        p.insert(p.end(), {11, 22, 33});
        const auto output = s.dispatch(p);
        require(output.size() == sizeof(Header) + sizeof(Reply) + 3 && output.back() == 33);
        require(output[sizeof(Header) + sizeof(Reply)] == (11 ^ 255));
        contexts.fail = true;
        const auto failed = s.dispatch(p);
        Reply body{}; std::memcpy(&body, failed.data() + sizeof(Header), sizeof body);
        require(header(failed).handle == 0 && body.ntstatus == -123 && failed.size() == sizeof(Header) + sizeof(Reply));
        contexts.fail = false; contexts.badContextReply = true;
        require(header(s.dispatch(p)).status == -5 && contexts.destroyed.back() == Kind::Context);
        contexts.badContextReply = false;
        require(header(s.dispatch(request(Op::DestroyContext, c, std::uint32_t{0}))).status == -22);
        require(header(s.dispatch(request(Op::DestroyContext, c))).status == 0);
        before = contexts.calls;
        require(header(s.dispatch(request(Op::DestroyContext, c))).status == -9 && contexts.calls == before);
        // Leave the second context alive: disconnect destroys it before device.
    }
    require(contexts.destroyed == std::vector<Kind>{Kind::Context, Kind::Context, Kind::Context, Kind::Device, Kind::Adapter});
    Fake glContexts;
    {
        Session s(glContexts); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        auto packet = [&](ContextDesc desc, unsigned bytes = 3) {
            auto p = request(Op::CreateContext, d, desc);
            p.insert(p.end(), bytes, 11); return p;
        };
        const auto before = glContexts.calls;
        for (const auto bad : {ContextDesc{0,1,4,16,3,0}, ContextDesc{0,1,2,16,3,0},
                ContextDesc{0,1,8,16,3,0}, ContextDesc{0,0,16,16,3,0}, ContextDesc{64,1,16,16,3,0},
                ContextDesc{0,1,16,16,3,1}, ContextDesc{0,1,16,16,MaxContextPrivateBytes+1,0},
                ContextDesc{0,1,16,1,3,0}, ContextDesc{0,1,16,5,3,0},
                ContextDesc{0,1,16,17,3,0}, ContextDesc{0,1,16,21,3,0}})
            require(header(s.dispatch(packet(bad))).status == -22);
        require(header(s.dispatch(packet({0,1,16,16,3,0},2))).status == -22);
        require(glContexts.calls == before);
        const auto p = packet({0,1,16,16,3,0});
        const auto output = s.dispatch(p); const auto c = header(output).handle;
        require(c && !header(output).status && output.size() == sizeof(Header)+sizeof(Reply)+3);
        require(output[sizeof(Header)+sizeof(Reply)] == (11^255));
        require(header(s.dispatch(request(Op::DestroyContext,c))).status == 0);
    }
    require(glContexts.destroyed == std::vector<Kind>{Kind::Context,Kind::Device,Kind::Adapter});
    Fake adapterQueries;
    {
        Session s(adapterQueries);
        const QueryDesc desc{0, 50616, 0, 0};
        require(header(s.dispatch(request(Op::BeginAdapterQuery, 1, desc))).status == -71);
        s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        const auto before = adapterQueries.calls;
        adapterQueries.queriesEnabled = false;
        require(header(s.dispatch(request(Op::BeginAdapterQuery, a, desc))).status == -95);
        adapterQueries.queriesEnabled = true;
        require(header(s.dispatch(request(Op::BeginAdapterQuery, device, desc))).status == -9);
        for (const auto invalid : {QueryDesc{0, 0, 0, 0}, QueryDesc{0, MaxQueryBytes + 1, 0, 0},
                QueryDesc{0, 4, 1, 0}, QueryDesc{0, 4, 0, 1}, QueryDesc{13, 8, 0, 0},
                QueryDesc{7, 4, 0, 0}, QueryDesc{41, 24, 0, 0}, QueryDesc{48, 552, 0, 0}})
            require(header(s.dispatch(request(Op::BeginAdapterQuery, a, invalid))).status == -22);
        require(adapterQueries.calls == before);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, a, desc))).status == 0);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, a, desc))).status == -16);
        require(header(s.dispatch(request(Op::CloseAdapter, a))).status == -16);
        require(header(s.dispatch(request(Op::RunAdapterQuery, a))).status == -71);
        require(header(s.dispatch(request(Op::ReadAdapterQuery, a, Range{0, 4}))).status == -71);
        auto hole = request(Op::WriteAdapterQuery, a, Range{1, 1}); hole.push_back(9);
        require(header(s.dispatch(hole)).status == -71);
        for (std::uint32_t offset = 0; offset < desc.bytes;) {
            const auto count = std::min(MaxChunk, desc.bytes - offset);
            auto input = request(Op::WriteAdapterQuery, a, Range{offset, count});
            for (std::uint32_t i = 0; i < count; ++i) input.push_back(static_cast<std::uint8_t>((offset + i) % 251));
            require(header(s.dispatch(input)).status == 0);
            require(header(s.dispatch(input)).status == -71); // overlap/retry
            offset += count;
        }
        auto output = s.dispatch(request(Op::RunAdapterQuery, a));
        Reply result{}; std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        require(header(output).status == 0 && result.ntstatus == 0 && result.value == desc.bytes);
        require(adapterQueries.calls == before + 1);
        require(header(s.dispatch(request(Op::RunAdapterQuery, a))).status == -71);
        hole = request(Op::WriteAdapterQuery, a, Range{0, 1}); hole.push_back(9);
        require(header(s.dispatch(hole)).status == -71);
        for (std::uint32_t offset = 0; offset < desc.bytes;) {
            const auto count = std::min(MaxChunk, desc.bytes - offset);
            output = s.dispatch(request(Op::ReadAdapterQuery, a, Range{offset, count}));
            require(output.size() == sizeof(Header) + sizeof(Reply) + count && output.size() <= native_gpu::MaxPacket);
            for (std::uint32_t i = 0; i < count; ++i)
                require(output[sizeof(Header) + sizeof(Reply) + i] == static_cast<std::uint8_t>(((offset + i) % 251) ^ 255));
            offset += count;
        }
        require(header(s.dispatch(request(Op::ReadAdapterQuery, a, Range{UINT32_MAX, 4}))).status == -22);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a, std::uint32_t{0}))).status == -22);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == -2);
        // Cancellation frees staging memory before adapter teardown.
        const auto b = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto c = header(s.dispatch(request(Op::OpenAdapter))).handle;
        for (const auto id : {a, b})
            require(header(s.dispatch(request(Op::BeginAdapterQuery, id, QueryDesc{0, MaxQueryBytes, 0, 0}))).status == 0);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, c, desc))).status == -24);
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        require(header(s.dispatch(request(Op::BeginAdapterQuery, c, desc))).status == 0);
        require(header(s.dispatch(request(Op::CloseAdapter, c))).status == -16);
        require(header(s.dispatch(request(Op::EndAdapterQuery, c))).status == 0);
        auto prepare = [&] {
            require(header(s.dispatch(request(Op::BeginAdapterQuery, a, QueryDesc{13, 4, 0, 0}))).status == 0);
            auto input = request(Op::WriteAdapterQuery, a, Range{0, 4}); input.insert(input.end(), {1, 2, 3, 4});
            require(header(s.dispatch(input)).status == 0);
        };
        prepare(); adapterQueries.fail = true;
        output = s.dispatch(request(Op::RunAdapterQuery, a));
        std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        require(result.ntstatus == -123);
        output = s.dispatch(request(Op::ReadAdapterQuery, a, Range{0, 4}));
        std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        require(result.ntstatus == -123 && output.back() == (4 ^ 255));
        adapterQueries.fail = false;
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        prepare(); adapterQueries.badQueryReply = true;
        require(header(s.dispatch(request(Op::RunAdapterQuery, a))).status == -5);
        adapterQueries.badQueryReply = false;
        prepare(); // malformed native reply discarded the staging object
        require(header(s.dispatch(request(Op::EndAdapterQuery, a))).status == 0);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == 0);
        require(header(s.dispatch(request(Op::CloseAdapter, a))).status == 0);
        // b deliberately retains an unfinished 64 KiB query on disconnect.
    }
    require(adapterQueries.destroyed == std::vector<Kind>{Kind::Device, Kind::Adapter, Kind::Adapter, Kind::Adapter});
    Fake queryClient;
    {
        Session s(queryClient); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        std::vector<std::uint8_t> input(50616);
        for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<std::uint8_t>(i % 251);
        auto exchange = [&](const std::vector<std::uint8_t>& packet) { return s.dispatch(packet); };
        const auto result = adapterQuery(exchange, a, 0, input);
        require(result.ntstatus == 0 && result.data.size() == input.size());
        for (std::size_t i = 0; i < input.size(); ++i) require(result.data[i] == (input[i] ^ 255));
        queryClient.fail = true;
        require(adapterQuery(exchange, a, 0, input).ntstatus == -123);
        queryClient.fail = false;
        bool corrupted = false;
        auto corrupt = [&](const std::vector<std::uint8_t>& packet) {
            auto output = s.dispatch(packet);
            if (header(packet).type == static_cast<std::uint32_t>(Op::ReadAdapterQuery)) output.pop_back();
            return output;
        };
        try { adapterQuery(corrupt, a, 0, input); } catch (const QueryProtocolError&) { corrupted = true; }
        require(corrupted);
        // A malformed reply cancels the staging object; a fresh query works.
        require(adapterQuery(exchange, a, 13, {0, 0, 0, 0}).ntstatus == 0);
        require(header(s.dispatch(request(Op::CloseAdapter, a))).status == 0);
    }
    Fake memory;
    {
        Session s(memory); s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        const auto otherDevice = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto otherQueue = header(s.dispatch(request(Op::CreatePagingQueue, otherDevice))).handle;
        const auto allocation = header(s.dispatch(request(Op::CreateAllocation, device, std::uint32_t{4096}))).handle;
        require(allocation && header(s.dispatch(request(Op::DestroyDevice, device))).status == -16);
        auto before = memory.calls;
        require(header(s.dispatch(request(Op::MakeResident, allocation, otherQueue))).status == -9);
        require(header(s.dispatch(request(Op::MapAllocation, allocation, device))).status == -9);
        require(header(s.dispatch(request(Op::ReadAllocation, queue, Range{0, 4}))).status == -9);
        for (const auto range : {Range{4095, 2}, Range{UINT32_MAX, 2}, Range{0, MaxChunk + 1}, Range{0, 0}})
            require(header(s.dispatch(request(Op::ReadAllocation, allocation, range))).status == -22);
        for (const auto size : {0u, 1u, MaxAllocation + 4096u})
            require(header(s.dispatch(request(Op::CreateAllocation, device, size))).status == -22);
        auto packet = request(Op::WriteAllocation, allocation, Range{0, 4});
        require(header(s.dispatch(packet)).status == -22 && memory.calls == before);
        packet.insert(packet.end(), {10, 20, 30, 40});
        require(header(s.dispatch(packet)).status == 0);
        packet = s.dispatch(request(Op::ReadAllocation, allocation, Range{0, 4}));
        require(packet.size() == sizeof(Header) + sizeof(Reply) + 4);
        require(std::vector<std::uint8_t>(packet.end() - 4, packet.end()) == std::vector<std::uint8_t>{10, 20, 30, 40});
        memory.shortRead = true;
        require(header(s.dispatch(request(Op::ReadAllocation, allocation, Range{0, 4}))).status == -5);
        memory.shortRead = false;
        require(header(s.dispatch(request(Op::MakeResident, allocation, queue))).status == 0);
        packet = s.dispatch(request(Op::MapAllocation, allocation, queue));
        Reply result{}; std::memcpy(&result, packet.data() + sizeof(Header), sizeof result);
        require(result.value == 0x123456789ull);
        memory.fail = true;
        packet = s.dispatch(request(Op::DestroyAllocation, allocation));
        std::memcpy(&result, packet.data() + sizeof(Header), sizeof result); require(result.ntstatus == -123);
        memory.fail = false;
        require(header(s.dispatch(request(Op::DestroyAllocation, allocation))).status == 0);
        require(header(s.dispatch(request(Op::QueryResidency, allocation))).status == -9);
        // New allocation retains a larger monotonic ID. Leave it for disconnect cleanup.
        require(header(s.dispatch(request(Op::CreateAllocation, device, std::uint32_t{4096}))).handle > allocation);
        const auto shared = header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x100000, 4096, 0}))).handle;
        require(shared != 0);
        before = memory.calls;
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x100000, 4096, 0}))).status == -16);
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{UINT64_MAX - 4095, 8192, 0}))).status == -22);
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{1, 4096, 0}))).status == -22);
        require(header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0, 4096, 1}))).status == -22);
        require(memory.calls == before);
        const auto from = header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x200000, 65536, 0}))).handle;
        const auto to = header(s.dispatch(request(Op::CreateSharedAllocation, device, GuestRange{0x210000, 65536, 0}))).handle;
        const auto foreign = header(s.dispatch(request(Op::CreateSharedAllocation, otherDevice, GuestRange{0x220000, 65536, 0}))).handle;
        before = memory.calls;
        for (const auto copy : {CopyRange{from, UINT32_MAX, 0, 1}, CopyRange{from, 0, 65535, 2},
                                CopyRange{from, 0, 0, 0}, CopyRange{to, 0, 0, 4}})
            require(header(s.dispatch(request(Op::CopySharedAllocation, to, copy))).status == -22);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{foreign, 0, 0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{allocation, 0, 0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{queue, 0, 0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::CopySharedAllocation, shared, CopyRange{from, 0, 0, 4}))).status == -22);
        require(memory.calls == before);
        packet = request(Op::WriteAllocation, from, Range{3, 4}); packet.insert(packet.end(), {91, 23, 201, 45});
        require(header(s.dispatch(packet)).status == 0);
        require(header(s.dispatch(request(Op::CopySharedAllocation, to, CopyRange{from, 3, 7, 4}))).status == 0);
        packet = s.dispatch(request(Op::ReadAllocation, to, Range{7, 4}));
        require(std::vector<std::uint8_t>(packet.end() - 4, packet.end()) == std::vector<std::uint8_t>{91, 23, 201, 45});
    }
    require(memory.buffers.empty() && memory.destroyed.back() == Kind::Adapter);
    Fake budget;
    {
        Session s(budget); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        budget.fail = true;
        auto packet = s.dispatch(request(Op::CreateAllocation, d, MaxAllocation));
        Reply result{}; std::memcpy(&result, packet.data() + sizeof(Header), sizeof result);
        require(result.ntstatus == -123 && header(packet).handle == 0);
        budget.fail = false;
        std::uint32_t last = 0;
        for (std::uint32_t size = 0; size < MaxAllocatedBytes; size += MaxAllocation) {
            last = header(s.dispatch(request(Op::CreateAllocation, d, MaxAllocation))).handle; require(last != 0);
        }
        const auto before = budget.calls;
        require(header(s.dispatch(request(Op::CreateAllocation, d, std::uint32_t{4096}))).status == -24 && budget.calls == before);
        require(header(s.dispatch(request(Op::DestroyAllocation, last))).status == 0);
        require(header(s.dispatch(request(Op::CreateAllocation, d, MaxAllocation))).status == 0);
    }
    Fake bounded;
    {
        Session s(bounded); s.dispatch(hello());
        for (std::size_t i = 0; i < MaxObjects; ++i) require(header(s.dispatch(request(Op::OpenAdapter))).status == 0);
        const auto before = bounded.calls;
        require(header(s.dispatch(request(Op::OpenAdapter))).status == -24 && bounded.calls == before);
    }
    Fake paging;
    {
        Session s(paging);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue))).status == -71);
        s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        auto before = paging.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -95 && paging.calls == before);
        paging.guestPagingEnabled = true;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, a))).status == -9);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d, unsigned{0}))).status == -22);
        require(paging.calls == before);
        for (int invalid = 1; invalid <= 4; ++invalid) {
            paging.badPagingReply = invalid;
            const auto destroys = paging.destroyed.size();
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -5);
            require(paging.destroyed.size() == destroys + 1 && paging.destroyed.back() == Kind::PagingQueue);
        }
        paging.badPagingReply = 0; paging.fail = true;
        auto packet = s.dispatch(request(Op::CreateGuestPagingQueue, d));
        Reply nt{}; std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(packet.size() == sizeof(Header) + sizeof(Reply) && !header(packet).handle && nt.ntstatus == -123);
        paging.fail = false;
        packet = s.dispatch(request(Op::CreateGuestPagingQueue, d));
        const auto queue = header(packet).handle;
        PagingReply info{}; require(packet.size() == sizeof(Header) + sizeof(Reply) + sizeof info);
        std::memcpy(&info, packet.data() + sizeof(Header) + sizeof(Reply), sizeof info);
        require(queue == 3 && info.sync == 4 && !info.reserved && info.offset == 8192);
        before = paging.calls;
        require(header(s.dispatch(request(Op::ReadPagingFence, info.sync))).status == -9);
        require(header(s.dispatch(request(Op::DestroyPagingQueue, info.sync))).status == -9);
        require(header(s.dispatch(request(Op::DestroyDevice, d))).status == -16 && paging.calls == before);
        paging.fail = true;
        packet = s.dispatch(request(Op::DestroyPagingQueue, queue));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); require(nt.ntstatus == -123);
        require(header(s.dispatch(request(Op::DestroyDevice, d))).status == -16);
        paging.fail = false;
        require(header(s.dispatch(request(Op::ReadPagingFence, queue))).status == 0);
        const auto destroys = paging.destroyed.size();
        require(header(s.dispatch(request(Op::DestroyPagingQueue, queue))).status == 0);
        require(paging.destroyed.size() == destroys + 1); // no separate native sync destruction
        require(header(s.dispatch(request(Op::ReadPagingFence, queue))).status == -9);
        require(header(s.dispatch(request(Op::DestroyPagingQueue, info.sync))).status == -9);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle > info.sync);
    }
    require(paging.destroyed[paging.destroyed.size() - 3] == Kind::PagingQueue);
    require(paging.destroyed.back() == Kind::Adapter);
    Fake vendor;
    {
        Session s(vendor);
        const VendorAllocationDesc desc{4, 0x78100000, 0, 586, 0, 0};
        auto create = [&](std::uint32_t device, VendorAllocationDesc input = VendorAllocationDesc{4, 0x78100000, 0, 586, 0, 0}) {
            auto packet = request(Op::CreateVendorAllocation, device, input); packet.insert(packet.end(), input.privateBytes, 37); return packet;
        };
        auto release = [&](std::uint32_t device, std::vector<std::uint32_t> ids) {
            auto packet = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{static_cast<std::uint32_t>(ids.size()), 0});
            const auto bytes = reinterpret_cast<const std::uint8_t*>(ids.data());
            if (!ids.empty()) packet.insert(packet.end(), bytes, bytes + ids.size() * sizeof(ids[0]));
            return packet;
        };
        require(header(s.dispatch(create(2))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto before = vendor.calls;
        require(header(s.dispatch(create(device))).status == -95 && vendor.calls == before);
        vendor.vendorEnabled = true;
        require(header(s.dispatch(create(adapter))).status == -9);
        require(validVendorAllocation(VendorAllocationDesc{4, 0xa0000000, 0, 586, 0, 0}) &&
                validVendorAllocation(VendorAllocationDesc{4, 0xc8000000, 0, 586, 0, 0}) &&
                validVendorAllocation(VendorAllocationDesc{4, desc.priority, UninitializedDisplaySource, 586, 0, 0}));
        for (const auto input : {VendorAllocationDesc{5, desc.priority, 0, 586, 0, 0}, VendorAllocationDesc{4, 0xc8000001, 0, 586, 0, 0},
                                 VendorAllocationDesc{5, desc.priority, UninitializedDisplaySource, 586, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, UINT32_MAX - 1, 586, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, 1, 586, 0, 0}, VendorAllocationDesc{4, desc.priority, 0, 0, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, 0, MaxVendorPrivateBytes + 1, 0, 0},
                                 VendorAllocationDesc{4, desc.priority, 0, 586, 1, 0}, VendorAllocationDesc{4, desc.priority, 0, 586, 0, 1}})
            require(header(s.dispatch(create(device, input))).status == -22);
        auto shortInput = create(device); shortInput.pop_back(); require(header(s.dispatch(shortInput)).status == -22);
        require(vendor.calls == before);
        vendor.fail = true;
        auto packet = s.dispatch(create(device)); Reply nt{}; std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(!header(packet).handle && !header(packet).status && nt.ntstatus == -123 && packet.back() == 37 &&
                packet[sizeof(Header) + sizeof(Reply)] == (37 ^ 255) && packet.size() == sizeof(Header) + sizeof(Reply) + desc.privateBytes);
        vendor.fail = false;
        for (const auto invalid : {1, 3}) { // malformed native in/out and GPU address must clean the native object
            vendor.badVendorReply = invalid;
            require(header(s.dispatch(create(device))).status == -5 && vendor.vendorOwners.empty());
        }
        vendor.badVendorReply = 0;
        const auto first = header(s.dispatch(create(device))).handle;
        require(first == 3); // failed allocations never consume successful object IDs
        const auto second = header(s.dispatch(create(device, VendorAllocationDesc{4, desc.priority, UninitializedDisplaySource, 586, 0, 0}))).handle;
        const auto other = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        before = vendor.calls;
        require(header(s.dispatch(request(Op::ReadAllocation, first, Range{0, 4}))).status == -9);
        require(header(s.dispatch(request(Op::DestroyAllocation, first))).status == -9);
        require(header(s.dispatch(release(device, {}))).status == -22);
        require(header(s.dispatch(release(device, {first, first}))).status == -22);
        require(header(s.dispatch(release(other, {first}))).status == -9);
        require(header(s.dispatch(release(device, {first, device}))).status == -9);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == -16 && vendor.calls == before);
        vendor.fail = true;
        packet = s.dispatch(release(device, {first, second})); std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt);
        require(nt.ntstatus == -123 && vendor.vendorOwners.size() == 2);
        vendor.fail = false;
        require(header(s.dispatch(release(device, {first, second}))).status == 0 && vendor.vendorOwners.empty());
        require(header(s.dispatch(release(device, {first}))).status == -9);
        std::vector<std::uint32_t> boundedObjects;
        for (std::size_t n = 0; n < MaxVendorAllocations; ++n) {
            const auto id = header(s.dispatch(create(device))).handle; require(id != 0); boundedObjects.push_back(id);
        }
        before = vendor.calls;
        require(header(s.dispatch(create(device))).status == -24 && vendor.calls == before);
        vendor.expandedVendorObjectsEnabled = true;
        for (std::size_t n = MaxVendorAllocations; n < ExpandedVendorAllocations; ++n) {
            const auto id = header(s.dispatch(create(device))).handle; require(id != 0); boundedObjects.push_back(id);
        }
        before = vendor.calls;
        require(header(s.dispatch(create(device))).status == -24 && vendor.calls == before);
        vendor.expandedVendorObjectsEnabled = false;
        require(header(s.dispatch(create(device))).status == -24 && vendor.calls == before);
        for (std::size_t n = 0; n <= ExpandedVendorAllocations - MaxVendorAllocations; ++n)
            require(header(s.dispatch(release(device,{boundedObjects[n]}))).status == 0);
        require(header(s.dispatch(create(device))).handle != 0);
        before = vendor.calls;
        require(header(s.dispatch(create(device))).status == -24 && vendor.calls == before);
    }
    require(vendor.vendorOwners.empty() && vendor.destroyed.back() == Kind::Adapter);
    Fake gpuVa; gpuVa.vendorEnabled = true;
    {
        Session s(gpuVa); const GpuVaDesc input{3, 0, 0, 67108864, 1099511627776ull, 0, 16, 1, 0};
        require(header(s.dispatch(request(Op::MapVendorAllocation, 4, input))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto create = [&] {
            auto packet = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{4, 0x78100000, 0, 586, 0, 0});
            packet.insert(packet.end(), 586, 0); return header(s.dispatch(packet)).handle;
        };
        auto release = [&](std::uint32_t id) {
            auto packet = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
            const auto start = packet.size(); packet.resize(start + 4); std::memcpy(packet.data() + start, &id, 4);
            return s.dispatch(packet);
        };
        const auto allocation = create(); require(queue == 3 && allocation == 4);
        auto before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -95 && gpuVa.calls == before);
        gpuVa.gpuVaEnabled = true;
        for (unsigned n = 0; n < 12; ++n) {
            auto invalid = input;
            switch (n) {
                case 0: invalid.queue = 0; break;
                case 1: invalid.reserved = 1; break;
                case 2: invalid.sizePages = 0; break;
                case 3: invalid.sizePages = MaxVendorMapPages + 1; break;
                case 4: invalid.offsetPages = UINT64_MAX; break;
                case 5: invalid.protection = 16; break;
                case 6: invalid.driverProtection = 1; break;
                case 7: invalid.base = MaxGpuAddress - 4096; break;
                case 8: invalid.minimum = 1; break;
                case 9: invalid.maximum = MaxGpuAddress + 4096; break;
                case 10: invalid.maximum = invalid.minimum; break;
                default: invalid.base = 1; break;
            }
            require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, invalid))).status == -22);
        }
        auto other = input; other.queue = device;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, other))).status == -9);
        require(header(s.dispatch(request(Op::MapVendorAllocation, queue, input))).status == -9);
        const auto device2 = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        other.queue = header(s.dispatch(request(Op::CreatePagingQueue, device2))).handle;
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, other))).status == -9 && gpuVa.calls == before);
        gpuVa.fail = true;
        auto packet = s.dispatch(request(Op::MapVendorAllocation, allocation, input)); Reply nt{}; GpuVaReply fence{};
        require(packet.size() == sizeof(Header) + sizeof(Reply) + sizeof(GpuVaReply));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&fence, packet.data() + sizeof(Header) + sizeof(Reply), sizeof fence);
        require(nt.ntstatus == -123 && !nt.value && !fence.fence && header(packet).handle == allocation);
        gpuVa.fail = false;
        for (int invalid = 1; invalid <= 5; ++invalid) {
            gpuVa.badGpuVaReply = invalid;
            require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -5);
        }
        gpuVa.badGpuVaReply = 0;
        packet = s.dispatch(request(Op::MapVendorAllocation, allocation, input));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&fence, packet.data() + sizeof(Header) + sizeof(Reply), sizeof fence);
        require(!header(packet).status && nt.ntstatus == 0x103 && validGpuVaOutput(input, nt.value) && fence.fence == 7019);
        const auto competing = create(); auto collision = input; collision.base = nt.value;
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, competing, collision))).status == -16 && gpuVa.calls == before);
        require(header(release(competing)).status == 0);
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -16 && gpuVa.calls == before);
        gpuVa.fail = true; release(allocation); gpuVa.fail = false;
        require(header(s.dispatch(request(Op::MapVendorAllocation, allocation, input))).status == -16);
        require(header(release(allocation)).status == 0);
        for (std::size_t n = 0; n < MaxVendorMappedPages / MaxVendorMapPages; ++n) {
            const auto id = create(); auto large = input; large.sizePages = MaxVendorMapPages;
            require(header(s.dispatch(request(Op::MapVendorAllocation, id, large))).status == 0);
        }
        const auto extra = create(); auto large = input; large.sizePages = MaxVendorMapPages;
        before = gpuVa.calls;
        require(header(s.dispatch(request(Op::MapVendorAllocation, extra, large))).status == -24 && gpuVa.calls == before);
        // Disconnect releases all mapped native allocations before their devices.
    }
    require(gpuVa.vendorOwners.empty());
    struct BudgetGpu : Fake {
        bool expanded = false;
        native_gpu::Capabilities capabilities() const override {
            auto c = Fake::capabilities();
            if (expanded) c.flags |= ExpandedVendorGpuCapability;
            return c;
        }
    };
    {
        const auto flags=ExpandedVendorGpuCapability;
        const auto legacy=vendorGpuPageLimit(flags),fourk=vendorGpuPageLimit(flags,true);
        require(legacy==ExpandedVendorGpuMappedBytes/4096 && fourk==131072u);
        require(vendorGpuPageLimit(0,true)==MaxVendorMappedPages);
        require(!vendorGpuBudgetFits(legacy,1,flags) && vendorGpuBudgetFits(legacy,1,flags,true));
        require(vendorGpuBudgetFits(fourk-1,1,flags,true) && !vendorGpuBudgetFits(fourk,1,flags,true));
        require(!vendorGpuBudgetFits(fourk+1,1,flags,true) && !vendorGpuBudgetFits(UINT32_MAX,1,flags,true));
        require(!vendorGpuBudgetFits(0,UINT64_MAX,flags,true));
    }
    for (const bool expanded : {false,true}) {
        BudgetGpu gpuBudget; gpuBudget.vendorEnabled = gpuBudget.gpuVaEnabled = true; gpuBudget.expanded = expanded;
        const auto flags = gpuBudget.capabilities().flags;
        const auto limit = vendorGpuPageLimit(flags);
        require(limit == (expanded ? 16384u : 8192u));
        require(vendorGpuBudgetFits(limit-1,1,flags) && !vendorGpuBudgetFits(limit,1,flags));
        require(!vendorGpuBudgetFits(limit+1,1,flags) && !vendorGpuBudgetFits(UINT32_MAX,1,flags));
        require(!vendorGpuBudgetFits(0,UINT64_MAX,flags));
        {
            Session s(gpuBudget); s.dispatch(hello());
            const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
            const auto device = header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
            const auto queue = header(s.dispatch(request(Op::CreatePagingQueue,device))).handle;
            std::vector<std::uint32_t> held;
            const auto create = [&] {
                auto packet = request(Op::CreateVendorAllocation,device,VendorAllocationDesc{0,0,0,1,0,0}); packet.push_back(1);
                const auto allocation = header(s.dispatch(packet)).handle; require(allocation != 0); return allocation;
            };
            const auto map = [&](std::uint32_t allocation,unsigned index) {
                return s.dispatch(request(Op::MapVendorAllocation,allocation,GpuVaDesc{queue,0,
                    67108864ull+index*std::uint64_t(MaxVendorCpuMappingBytes),67108864,1ull<<40,0,MaxVendorMapPages,1,0}));
            };
            for (unsigned n=0;n<limit/MaxVendorMapPages;++n) {
                held.push_back(create()); require(header(map(held.back(),n)).status == 0);
            }
            const auto extra = create(); auto before = gpuBudget.calls;
            require(header(map(extra,static_cast<unsigned>(held.size()))).status == -24 && gpuBudget.calls == before);
            auto destroy = request(Op::DestroyVendorAllocations,device,DestroyVendorDesc{1,0});
            const auto bytes = reinterpret_cast<const std::uint8_t*>(&held[0]); destroy.insert(destroy.end(),bytes,bytes+4);
            require(header(s.dispatch(destroy)).status == 0);
            if (expanded) {
                gpuBudget.expanded = false; before = gpuBudget.calls;
                require(header(map(extra,0)).status == -24 && gpuBudget.calls == before);
                gpuBudget.expanded = true;
            }
            require(header(map(extra,0)).status == 0);
        }
        require(gpuBudget.vendorOwners.empty());
    }
    Fake resident; resident.vendorEnabled = true;
    {
        Session s(resident);
        auto make = [&](std::uint32_t queue, const std::vector<std::uint32_t>& ids, std::uint32_t flags = 1,
                        const std::vector<std::uint32_t>& priorities = std::vector<std::uint32_t>{}) {
            auto packet = request(Op::MakeVendorResident, queue, ResidentDesc{static_cast<std::uint32_t>(ids.size()), flags, priorities.empty() ? 0u : 1u, 0});
            for (const auto& values : {ids, priorities}) {
                const auto start = packet.size(); packet.resize(start + values.size() * 4);
                if (!values.empty()) std::memcpy(packet.data() + start, values.data(), values.size() * 4);
            }
            return packet;
        };
        auto create = [&](std::uint32_t device) {
            auto packet = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{4, 0x78100000, 0, 586, 0, 0});
            packet.insert(packet.end(), 586, 0); return header(s.dispatch(packet)).handle;
        };
        auto release = [&](std::uint32_t device, std::uint32_t id) {
            auto packet = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
            const auto start = packet.size(); packet.resize(start + 4); std::memcpy(packet.data() + start, &id, 4);
            return s.dispatch(packet);
        };
        require(header(s.dispatch(make(3, {4}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        const auto first = create(device), second = create(device);
        auto before = resident.calls;
        require(header(s.dispatch(make(queue, {first}))).status == -95 && resident.calls == before);
        resident.residencyEnabled = true;
        require(header(s.dispatch(make(queue, {}))).status == -22);
        require(header(s.dispatch(make(queue, std::vector<std::uint32_t>(17, first)))).status == -22);
        require(header(s.dispatch(make(queue, {first}, 2))).status == -22);
        require(header(s.dispatch(make(queue, {first}, 4))).status == -22);
        require(header(s.dispatch(make(queue, {first, first}))).status == -22);
        auto truncated = make(queue, {first}, 1, {0x78100000}); truncated.pop_back();
        require(header(s.dispatch(truncated)).status == -22);
        require(header(s.dispatch(make(queue, {first, second}, 1, {0x78100000}))).status == -22);
        auto invalid = make(queue, {first}); invalid[28] = 1; // reserved field
        require(header(s.dispatch(invalid)).status == -22);
        invalid = make(queue, {first}); invalid[24] = 2; // priority-present is boolean
        require(header(s.dispatch(invalid)).status == -22);
        require(header(s.dispatch(make(device, {first}))).status == -9);
        require(header(s.dispatch(make(queue, {device}))).status == -9);
        require(header(s.dispatch(make(queue, {999}))).status == -9 && resident.calls == before);
        const auto other = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto foreign = create(other);
        before = resident.calls;
        require(header(s.dispatch(make(queue, {first, foreign}))).status == -9 && resident.calls == before);
        resident.fail = true;
        auto packet = s.dispatch(make(queue, {first, second}, 1, {0x78100000, 0x78000000}));
        Reply nt{}; ResidentReply out{};
        require(packet.size() == sizeof(Header) + sizeof(Reply) + sizeof(ResidentReply) && header(packet).handle == queue && !header(packet).status);
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&out, packet.data() + sizeof(Header) + sizeof(Reply), sizeof out);
        require(nt.ntstatus == static_cast<std::int32_t>(0xc0000017u) && nt.value == 7009 && out.count == 1 && out.bytesToTrim == 65536);
        require(resident.lastResidentHandles.size() == 2 && resident.lastResidentHandles[0] > 500 &&
                resident.lastResidentPriorities == std::vector<std::uint32_t>{0x78100000, 0x78000000});
        resident.fail = false;
        for (int n = 1; n <= 4; ++n) {
            resident.badResidentReply = n; require(header(s.dispatch(make(queue, {first}))).status == -5);
        }
        resident.badResidentReply = 0;
        packet = s.dispatch(make(queue, {first}, 0));
        std::memcpy(&nt, packet.data() + sizeof(Header), sizeof nt); std::memcpy(&out, packet.data() + sizeof(Header) + sizeof(Reply), sizeof out);
        require(!header(packet).status && nt.ntstatus == 259 && nt.value == 7011 && out.count == 1 && !out.reserved && !out.bytesToTrim);
        // Failed native destruction retains ownership and admits another call.
        resident.fail = true; release(device, first); resident.fail = false;
        require(header(s.dispatch(make(queue, {first}))).status == 0);
        require(header(release(device, first)).status == 0);
        before = resident.calls;
        require(header(s.dispatch(make(queue, {first}))).status == -9 && resident.calls == before);
        const auto capped = create(device);
        for (unsigned n = 0; n < MaxVendorResidencyAttempts; ++n)
            require(header(s.dispatch(make(queue, {capped}))).status == 0);
        before = resident.calls;
        require(header(s.dispatch(make(queue, {capped}))).status == -24 && resident.calls == before);
        // EOF releases all allocations, including those with successful or failed residency.
    }
    require(resident.vendorOwners.empty());
    Fake cpu; cpu.vendorEnabled = cpu.gpuVaEnabled = cpu.cpuEnabled = true;
    {
        Session s(cpu);
        require(header(s.dispatch(request(Op::LockVendorAllocation, 1, VendorCpuDesc{2, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto other = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto queue = header(s.dispatch(request(Op::CreatePagingQueue, device))).handle;
        auto create = [&] {
            auto packet = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
            packet.push_back(1); return header(s.dispatch(packet)).handle;
        };
        auto mapped = [&](std::uint32_t allocation, std::uint64_t offset = 0) {
            return s.dispatch(request(Op::MapVendorAllocation, allocation, GpuVaDesc{queue, 0, 0, 65536, 1ull << 40, offset, 16, 1, 0}));
        };
        const auto allocation = create(); require(allocation != 0);
        const auto lock = request(Op::LockVendorAllocation, allocation, VendorCpuDesc{device, 0});
        const auto unlock = request(Op::UnlockVendorAllocation, allocation, VendorCpuDesc{device, 0});
        auto before = cpu.calls;
        cpu.cpuEnabled = false; require(header(s.dispatch(lock)).status == -95); cpu.cpuEnabled = true;
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation))).status == -22);
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation, VendorCpuDesc{device, 1}))).status == -22);
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation, VendorCpuDesc{0, 0}))).status == -22);
        require(header(s.dispatch(request(Op::LockVendorAllocation, allocation, VendorCpuDesc{other, 0}))).status == -9);
        require(header(s.dispatch(request(Op::LockVendorAllocation, device, VendorCpuDesc{device, 0}))).status == -9);
        require(header(s.dispatch(lock)).status == -95 && header(s.dispatch(unlock)).status == -9 && cpu.calls == before);
        require(header(mapped(allocation)).status == 0);
        const auto subrange = create(); require(header(mapped(subrange, 1)).status == 0);
        before = cpu.calls;
        require(header(s.dispatch(request(Op::LockVendorAllocation, subrange, VendorCpuDesc{device, 0}))).status == -95 && cpu.calls == before);
        cpu.fail = true;
        auto out = s.dispatch(lock); Reply reply{}; std::memcpy(&reply, out.data() + sizeof(Header), sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123 && reply.value == 0 && out.size() == sizeof(Header) + sizeof(Reply) + sizeof(VendorCpuReply));
        cpu.fail = false;
        for (int malformedCpu = 1; malformedCpu <= 10; ++malformedCpu) {
            cpu.badCpuReply = malformedCpu; require(header(s.dispatch(lock)).status == -5);
        }
        cpu.badCpuReply = 0;
        out = s.dispatch(lock); VendorCpuReply body{};
        std::memcpy(&reply, out.data() + sizeof(Header), sizeof reply);
        std::memcpy(&body, out.data() + sizeof(Header) + sizeof reply, sizeof body);
        require(header(out).handle == allocation && reply.value == 0 && body.bytes == 65536 && body.generation == 1);
        before = cpu.calls; require(header(s.dispatch(lock)).status == -16 && cpu.calls == before);
        auto release = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&allocation); release.insert(release.end(), bytes, bytes + 4);
        require(header(s.dispatch(release)).status == -16 && cpu.calls == before);
        const auto duplicate = create(); require(header(mapped(duplicate)).status == 0);
        require(header(s.dispatch(request(Op::LockVendorAllocation, duplicate, VendorCpuDesc{device, 0}))).status == -5);
        cpu.fail = true; out = s.dispatch(unlock); std::memcpy(&reply, out.data() + sizeof(Header), sizeof reply);
        require(header(out).status == 0 && reply.ntstatus == -123);
        cpu.fail = false;
        require(header(s.dispatch(lock)).status == -16);
        for (int malformedCpu = 1; malformedCpu <= 3; ++malformedCpu) {
            cpu.badCpuReply = malformedCpu; require(header(s.dispatch(unlock)).status == -5);
        }
        cpu.badCpuReply = 0;
        require(header(s.dispatch(unlock)).status == 0);
        before = cpu.calls; require(header(s.dispatch(unlock)).status == -9 && cpu.calls == before);
        require(header(s.dispatch(lock)).status == 0); // A new lease can reuse the slot after unlock.
        // EOF retains the vendor allocation for native child-before-parent destruction.
    }
    require(cpu.vendorOwners.empty() && cpu.cpuLocks > 0 && cpu.cpuUnlocks > 0);
    struct BudgetCpu : Fake {
        bool expanded = false;
        std::uint64_t cpuOffset = 0;
        native_gpu::Capabilities capabilities() const override {
            auto c = Fake::capabilities();
            if (expanded) c.flags |= ExpandedVendorCpuCapability;
            return c;
        }
        VendorCpuResult lockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
            require(vendorOwners.at(allocation) == device); ++calls; ++cpuLocks;
            const auto offset = cpuOffset; cpuOffset += MaxVendorCpuMappingBytes;
            return {{0,0,offset},{MaxVendorCpuMappingBytes,0,offset / VendorCpuSlotBytes + 1}};
        }
    };
    for (const bool expanded : {false,true}) {
        BudgetCpu cpuBudget; cpuBudget.vendorEnabled = cpuBudget.gpuVaEnabled = cpuBudget.cpuEnabled = true; cpuBudget.expanded = expanded;
        {
            Session s(cpuBudget); s.dispatch(hello());
            const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
            const auto device = header(s.dispatch(request(Op::CreateDevice,adapter))).handle;
            const auto queue = header(s.dispatch(request(Op::CreatePagingQueue,device))).handle;
            const auto create = [&](unsigned index) {
                auto p = request(Op::CreateVendorAllocation,device,VendorAllocationDesc{0,0,0,1,0,0}); p.push_back(1);
                const auto allocation = header(s.dispatch(p)).handle; require(allocation != 0);
                const auto base = 67108864ull + index * std::uint64_t(MaxVendorCpuMappingBytes);
                require(header(s.dispatch(request(Op::MapVendorAllocation,allocation,
                    GpuVaDesc{queue,0,base,67108864,1ull<<40,0,1024,1,0}))).status == 0);
                return allocation;
            };
            std::vector<std::uint32_t> held;
            const unsigned count = expanded ? 8u : 4u;
            for (unsigned n = 0; n < count; ++n) {
                held.push_back(create(n));
                require(header(s.dispatch(request(Op::LockVendorAllocation,held.back(),VendorCpuDesc{device,0}))).status == 0);
            }
            if (!expanded) {
                const auto excess = create(count); const auto before = cpuBudget.calls;
                require(header(s.dispatch(request(Op::LockVendorAllocation,excess,VendorCpuDesc{device,0}))).status == -24 && cpuBudget.calls == before);
            }
            // A failed unlock keeps bytes charged. Successful retry and native
            // destruction make room for a replacement view on the same device.
            cpuBudget.fail = true;
            const auto failed = s.dispatch(request(Op::UnlockVendorAllocation,held[0],VendorCpuDesc{device,0}));
            Reply nt{}; std::memcpy(&nt,failed.data()+sizeof(Header),sizeof nt); require(nt.ntstatus < 0);
            cpuBudget.fail = false;
            require(header(s.dispatch(request(Op::UnlockVendorAllocation,held[0],VendorCpuDesc{device,0}))).status == 0);
            auto destroy = request(Op::DestroyVendorAllocations,device,DestroyVendorDesc{1,0});
            const auto bytes = reinterpret_cast<const std::uint8_t*>(&held[0]); destroy.insert(destroy.end(),bytes,bytes+4);
            require(header(s.dispatch(destroy)).status == 0);
            const auto replacement = create(0);
            if (expanded) {
                cpuBudget.expanded = false; const auto before = cpuBudget.calls;
                require(header(s.dispatch(request(Op::LockVendorAllocation,replacement,VendorCpuDesc{device,0}))).status == -24 && cpuBudget.calls == before);
                cpuBudget.expanded = true;
            }
            require(header(s.dispatch(request(Op::LockVendorAllocation,replacement,VendorCpuDesc{device,0}))).status == 0);
        }
        // The fake counts explicit unlock attempts; EOF destruction delegates
        // still-locked allocation cleanup to the native owner.
        require(cpuBudget.vendorOwners.empty() && cpuBudget.cpuLocks == (expanded ? 9u : 5u) && cpuBudget.cpuUnlocks == 2);
    }
    Fake translation; translation.vendorEnabled = translation.translationEnabled = true;
    {
        Session s(translation);
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, 3, VendorTranslationDesc{2, 1, 0, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto otherAdapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto otherDevice = header(s.dispatch(request(Op::CreateDevice, otherAdapter))).handle;
        auto create = [&] {
            auto p = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
            p.push_back(37); return header(s.dispatch(p)).handle;
        };
        const auto allocation = create(), second = create();
        const auto translate = request(Op::TranslateVendorAllocation, allocation, VendorTranslationDesc{device, adapter, 0, 0});
        const auto before = translation.calls;
        translation.translationEnabled = false; require(header(s.dispatch(translate)).status == -95);
        translation.translationEnabled = true;
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, allocation))).status == -22);
        for (const auto desc : {VendorTranslationDesc{0, adapter, 0, 0}, VendorTranslationDesc{device, 0, 0, 0},
                               VendorTranslationDesc{device, adapter, 1, 0}, VendorTranslationDesc{device, adapter, 0, 1}})
            require(header(s.dispatch(request(Op::TranslateVendorAllocation, allocation, desc))).status == -22);
        for (const auto desc : {VendorTranslationDesc{otherDevice, otherAdapter, 0, 0}, VendorTranslationDesc{device, otherAdapter, 0, 0},
                               VendorTranslationDesc{allocation, adapter, 0, 0}, VendorTranslationDesc{device, device, 0, 0}})
            require(header(s.dispatch(request(Op::TranslateVendorAllocation, allocation, desc))).status == -9);
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, device, VendorTranslationDesc{device, adapter, 0, 0}))).status == -9);
        require(translation.calls == before);
        translation.fail = true;
        auto out = s.dispatch(translate); Reply body{}; std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && header(out).handle == allocation && body.ntstatus == -123 && body.value == 0);
        translation.badTranslationReply = 5; require(header(s.dispatch(translate)).status == -5);
        translation.fail = false;
        for (int malformedTranslation = 1; malformedTranslation <= 4; ++malformedTranslation) {
            translation.badTranslationReply = malformedTranslation; require(header(s.dispatch(translate)).status == -5);
        }
        translation.badTranslationReply = 0;
        out = s.dispatch(translate); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).handle == allocation && body.ntstatus == 0 && body.value == 0x10000000ull + allocation + 500);
        require(translation.lastTranslationHandles == std::vector<std::uint32_t>{allocation + 500, device + 500, adapter + 500});
        require(header(s.dispatch(translate)).status == 0);
        translation.translationOverride = static_cast<std::uint32_t>(body.value);
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, second, VendorTranslationDesc{device, adapter, 0, 0}))).status == -5);
        ++translation.translationOverride; require(header(s.dispatch(translate)).status == -5);
        translation.translationOverride = 0;
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, second, VendorTranslationDesc{device, adapter, 0, 0}))).status == 0);
        // The driver token never becomes a typed wire identity.
        require(header(s.dispatch(request(Op::TranslateVendorAllocation, static_cast<std::uint32_t>(body.value), VendorTranslationDesc{device, adapter, 0, 0}))).status == -9);
    }
    require(translation.vendorOwners.empty());
    Fake hardware; hardware.hwQueuesEnabled = hardware.vendorEnabled = true;
    {
        Session s(hardware);
        require(header(s.dispatch(request(Op::CreateHwQueue, 3, HwQueueDesc{0, 4, 0, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto contextPacket = request(Op::CreateContext, device, ContextDesc{0, 1, 16, 12, 4, 0});
        contextPacket.insert(contextPacket.end(), {1, 2, 3, 4});
        const auto context = header(s.dispatch(contextPacket)).handle;
        const auto syncContext = header(s.dispatch(request(Op::CreateContext, device, ContextDesc{0, 0, 8, 0, 0, 0}))).handle;
        auto create = [&](std::uint32_t parent, HwQueueDesc desc = {0, 4, 0, 0}) {
            auto p = request(Op::CreateHwQueue, parent, desc); p.insert(p.end(), {37, 38, 39, 40}); return p;
        };
        auto before = hardware.calls;
        hardware.hwQueuesEnabled = false; require(header(s.dispatch(create(context))).status == -95); hardware.hwQueuesEnabled = true;
        require(header(s.dispatch(create(device))).status == -9);
        require(header(s.dispatch(create(syncContext))).status == -95);
        require(header(s.dispatch(request(Op::CreateHwQueue, context))).status == -22);
        for (const auto desc : {HwQueueDesc{1, 4, 0, 0}, HwQueueDesc{3, 4, 0, 0}, HwQueueDesc{4, 4, 0, 0}, HwQueueDesc{8, 4, 0, 0},
                               HwQueueDesc{0, 0, 0, 0}, HwQueueDesc{0, 4001, 0, 0}, HwQueueDesc{0, 4, 1, 0}, HwQueueDesc{0, 4, 0, 1}})
            require(header(s.dispatch(create(context, desc))).status == -22);
        require(hardware.calls == before);
        require(header(s.dispatch(create(context, {2, 4, 0, 0}))).status == -95 && hardware.calls == before);
        hardware.noBroadcastHwQueuesEnabled = true;
        require(header(s.dispatch(create(context, {6, 4, 0, 0}))).status == -95 && hardware.calls == before);
        hardware.noBroadcastWaitHwQueuesEnabled = true; hardware.noBroadcastHwQueuesEnabled = false;
        require(header(s.dispatch(create(context, {6, 4, 0, 0}))).status == -95 && hardware.calls == before);
        hardware.noBroadcastHwQueuesEnabled = true;
        const auto internalHelper = header(s.dispatch(create(context, {6, 4, 0, 0}))).handle;
        require(internalHelper && hardware.lastHwQueueDesc.flags == InternalNoBroadcastHwQueueFlags);
        require(header(s.dispatch(request(Op::DestroyHwQueue, internalHelper))).status == 0);
        hardware.noBroadcastWaitHwQueuesEnabled = false;
        const auto helper = header(s.dispatch(create(context, {2, 4, 0, 0}))).handle;
        require(helper && hardware.lastHwQueueDesc.flags == NoBroadcastSignalHwQueueFlag);
        before = hardware.calls;
        require(header(s.dispatch(create(device, {2, 4, 0, 0}))).status == -9 && hardware.calls == before);
        require(header(s.dispatch(create(syncContext, {2, 4, 0, 0}))).status == -95 && hardware.calls == before);
        require(header(s.dispatch(request(Op::DestroyHwQueue, helper))).status == 0);
        require(header(s.dispatch(request(Op::DestroyHwQueue, helper))).status == -9);
        hardware.noBroadcastHwQueuesEnabled = false;
        hardware.fail = true;
        auto out = s.dispatch(create(context)); Reply body{}; HwQueueReply progress{};
        std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        std::memcpy(&progress, out.data() + sizeof(Header) + sizeof body, sizeof progress);
        require(header(out).status == 0 && !header(out).handle && body.ntstatus == -123 && !progress.sync && !progress.gpuAddress && out.back() == 40);
        hardware.badHwQueueReply = 12; require(header(s.dispatch(create(context))).status == -5);
        hardware.fail = false;
        for (int malformedQueue = 1; malformedQueue <= 11; ++malformedQueue) {
            hardware.badHwQueueReply = malformedQueue; require(header(s.dispatch(create(context))).status == -5 && hardware.hwQueueParents.empty());
        }
        hardware.badHwQueueReply = 0;
        out = s.dispatch(create(context)); const auto queue = header(out).handle;
        std::memcpy(&progress, out.data() + sizeof(Header) + sizeof body, sizeof progress);
        require(queue && progress.sync && progress.sync != queue && progress.offset == 8192 && progress.gpuAddress == 65536);
        require(out[sizeof(Header) + sizeof body + sizeof progress] == (37 ^ 255));
        before = hardware.calls;
        require(header(s.dispatch(request(Op::DestroyContext, context))).status == -16);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == -16);
        require(header(s.dispatch(request(Op::DestroyHwQueue, progress.sync))).status == -9 && hardware.calls == before);
        auto allocationPacket = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0, 0, 0, 1, 0, 0}); allocationPacket.push_back(37);
        const auto allocation = header(s.dispatch(allocationPacket)).handle;
        auto release = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&allocation); release.insert(release.end(), bytes, bytes + 4);
        before = hardware.calls; require(header(s.dispatch(release)).status == -16 && hardware.calls == before);
        hardware.retirementEnabled = true;
        hardware.fail = true; out = s.dispatch(release); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && !hardware.vendorOwners.empty());
        hardware.fail = false;
        for (int bad = 1; bad <= 3; ++bad) {
            hardware.badVendorDestroyReply = bad;
            require(header(s.dispatch(release)).status == -5 && !hardware.vendorOwners.empty());
        }
        hardware.badVendorDestroyReply = 0;
        require(header(s.dispatch(release)).status == 0 && hardware.vendorOwners.empty() && !hardware.hwQueueParents.empty());
        before = hardware.calls; require(header(s.dispatch(release)).status == -9 && hardware.calls == before);
        hardware.retirementEnabled = false;
        hardware.fail = true; out = s.dispatch(request(Op::DestroyHwQueue, queue)); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && !hardware.hwQueueParents.empty());
        hardware.fail = false;
        require(header(s.dispatch(request(Op::DestroyHwQueue, queue))).status == 0);
        require(header(s.dispatch(request(Op::DestroyHwQueue, queue))).status == -9);
        for (std::size_t n = 0; n < MaxHwQueues; ++n) require(header(s.dispatch(create(context))).handle != 0);
        before = hardware.calls; require(header(s.dispatch(create(context))).status == -24 && hardware.calls == before);
        // An allocation created after the queues still outlives them at EOF.
        require(header(s.dispatch(allocationPacket)).handle != 0);
        hardware.destroyed.clear();
    }
    require(hardware.hwQueueParents.empty() && hardware.vendorOwners.empty() && hardware.destroyed.size() >= MaxHwQueues + 5);
    for (std::size_t n = 0; n < MaxHwQueues; ++n) require(hardware.destroyed[n] == Kind::HwQueue);
    require(hardware.destroyed[MaxHwQueues] == Kind::VendorAllocation);
    Fake synchronization; synchronization.syncEnabled = synchronization.guestPagingEnabled = true;
    {
        Session s(synchronization);
        require(header(s.dispatch(request(Op::CreateSync, 2, SyncDesc{5, 0, 0, 0, 0}))).status == -71);
        s.dispatch(hello());
        const auto adapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(s.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto syncPaging = s.dispatch(request(Op::CreateGuestPagingQueue, device)); PagingReply borrowed{};
        std::memcpy(&borrowed, syncPaging.data() + sizeof(Header) + sizeof(Reply), sizeof borrowed);
        const auto create = request(Op::CreateSync, device, SyncDesc{5, 0, 1, 0, 42});
        const auto before = synchronization.calls;
        synchronization.syncEnabled = false; require(header(s.dispatch(create)).status == -95); synchronization.syncEnabled = true;
        require(header(s.dispatch(request(Op::CreateSync, adapter, SyncDesc{5, 0, 0, 0, 0}))).status == -9);
        require(header(s.dispatch(request(Op::CreateSync, device))).status == -22);
        for (const auto desc : {SyncDesc{4, 0, 0, 0, 0}, SyncDesc{5, 1, 0, 0, 0}, SyncDesc{5, 0, 2, 0, 0},
                               SyncDesc{5, 0, 0, 1, 0}, SyncDesc{1, 0, 0, 0, 2}, SyncDesc{1, 0, 1, 0, 0},
                               SyncDesc{1, NoGpuAccessSyncFlag, 0, 0, 0}, SyncDesc{5, NoGpuAccessSyncFlag | 1, 0, 0, 0}})
            require(header(s.dispatch(request(Op::CreateSync, device, desc))).status == -22);
        require(header(s.dispatch(request(Op::DestroySync, borrowed.sync))).status == -9 && synchronization.calls == before);
        synchronization.fail = true;
        auto out = s.dispatch(create); Reply body{}; SyncReply view{};
        std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(header(out).status == 0 && !header(out).handle && body.ntstatus == -123 && !view.offset && !view.gpuAddress);
        synchronization.badSyncReply = 10; require(header(s.dispatch(create)).status == -5);
        synchronization.fail = false;
        for (int malformedSync = 1; malformedSync <= 8; ++malformedSync) {
            synchronization.badSyncReply = malformedSync; require(header(s.dispatch(create)).status == -5 && synchronization.syncParents.empty());
        }
        synchronization.badSyncReply = 9;
        require(header(s.dispatch(request(Op::CreateSync, device, SyncDesc{1, 0, 0, 0, 0}))).status == -5 && synchronization.syncParents.empty());
        synchronization.badSyncReply = 0;
        out = s.dispatch(create); const auto fence = header(out).handle;
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(fence && view.offset == 8192 && view.gpuAddress == 65536 && synchronization.lastSyncDesc.initial == 42 && synchronization.lastSyncDesc.affinity == 1);
        const auto mutex = header(s.dispatch(request(Op::CreateSync, device, SyncDesc{1, 0, 0, 0, 1}))).handle;
        require(mutex && synchronization.lastSyncDesc.initial == 1);
        require(header(s.dispatch(request(Op::DestroyDevice, device))).status == -16);
        synchronization.fail = true;
        out = s.dispatch(request(Op::DestroySync, mutex)); std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && synchronization.syncParents.size() == 2);
        synchronization.fail = false;
        require(header(s.dispatch(request(Op::DestroySync, mutex))).status == 0);
        require(header(s.dispatch(request(Op::DestroySync, mutex))).status == -9);
        const auto cpuFenceRequest = request(Op::CreateSync, device, SyncDesc{5, NoGpuAccessSyncFlag, 0, 0, 42});
        synchronization.badSyncReply = 11;
        require(header(s.dispatch(cpuFenceRequest)).status == -5 && synchronization.syncParents.size() == 1);
        synchronization.badSyncReply = 0;
        out = s.dispatch(cpuFenceRequest); const auto cpuFence = header(out).handle;
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(cpuFence && view.offset == 8192 && !view.gpuAddress && synchronization.lastSyncDesc.flags == NoGpuAccessSyncFlag);
        require(header(s.dispatch(request(Op::DestroySync, cpuFence))).status == 0);
        const auto noMaxRequest = request(Op::CreateSync, device, SyncDesc{5, NoSignalMaxValueOnTdrSyncFlag, 0, 0, 42});
        synchronization.badSyncReply = 6; // A GPU-accessible fence must have a valid GPU address.
        require(header(s.dispatch(noMaxRequest)).status == -5 && synchronization.syncParents.size() == 1);
        synchronization.badSyncReply = 0;
        out = s.dispatch(noMaxRequest); const auto noMaxFence = header(out).handle;
        std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
        require(noMaxFence && view.offset == 8192 && view.gpuAddress == 65536 &&
                synchronization.lastSyncDesc.flags == NoSignalMaxValueOnTdrSyncFlag && synchronization.lastSyncDesc.initial == 42);
        require(header(s.dispatch(request(Op::DestroySync, noMaxFence))).status == 0);
        for (const auto flags : {NtSharedNoGpuAccessSyncFlags,
                                 NtSharedNoGpuAccessSyncFlags | UnwaitCpuWaitersOnlyOnDestroySyncFlag}) {
            const auto sharedRequest = request(Op::CreateSync, device, SyncDesc{5, flags, 1, 0, 42});
            synchronization.badSyncReply = 11;
            require(header(s.dispatch(sharedRequest)).status == -5 && synchronization.syncParents.size() == 1);
            synchronization.badSyncReply = 0;
            out = s.dispatch(sharedRequest); const auto sharedFence = header(out).handle;
            std::memcpy(&view, out.data() + sizeof(Header) + sizeof body, sizeof view);
            require(sharedFence && view.offset == 8192 && !view.gpuAddress &&
                    synchronization.lastSyncDesc.flags == flags && synchronization.lastSyncDesc.initial == 42);
            require(header(s.dispatch(request(Op::DestroySync, sharedFence))).status == 0);
        }
        for (std::uint32_t flags = 0; flags < 4096; ++flags) {
            require(validSync({5, flags, 0, 0, 0}) == (flags == 0 || flags == 3 || flags == 64 || flags == 128 || flags == 131 || flags == 1155));
            require(validSync({1, flags, 0, 0, 0}) == (flags == 0));
        }
        for (std::size_t n = 1; n < MaxSyncObjects; ++n)
            require(header(s.dispatch(request(Op::CreateSync, device, SyncDesc{1, 0, 0, 0, 0}))).handle != 0);
        require(header(s.dispatch(create)).status == -24);
    }
    require(synchronization.syncParents.empty());
    Fake submission; submission.submitEnabled = submission.hwQueuesEnabled = submission.vendorEnabled = submission.gpuVaEnabled =
                     submission.residencyEnabled = submission.cpuEnabled = true;
    {
        Session s(submission); const HwSubmitDesc validSubmit{65536, 5, 4096, 4, 0, 0};
        require(header(s.dispatch(request(Op::SubmitHwQueue, 3, validSubmit))).status == -71);
        s.dispatch(hello());
        const auto submitAdapter = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto submitDevice = header(s.dispatch(request(Op::CreateDevice, submitAdapter))).handle;
        auto submitContextPacket = request(Op::CreateContext, submitDevice, ContextDesc{0, 1, 16, 12, 4, 0});
        submitContextPacket.insert(submitContextPacket.end(), {1, 2, 3, 4});
        const auto submitContext = header(s.dispatch(submitContextPacket)).handle;
        auto submitQueuePacket = request(Op::CreateHwQueue, submitContext, HwQueueDesc{0, 4, 0, 0});
        submitQueuePacket.insert(submitQueuePacket.end(), {37, 38, 39, 40});
        const auto submitQueue = header(s.dispatch(submitQueuePacket)).handle;
        const auto submitPaging = header(s.dispatch(request(Op::CreatePagingQueue, submitDevice))).handle;
        auto submitAllocationPacket = request(Op::CreateVendorAllocation, submitDevice, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
        submitAllocationPacket.push_back(37);
        const auto submitAllocation = header(s.dispatch(submitAllocationPacket)).handle;
        auto submitPacket = [&](std::uint32_t queue, HwSubmitDesc desc) {
            auto p = request(Op::SubmitHwQueue, queue, desc); p.insert(p.end(), {37, 38, 39, 40}); return p;
        };
        auto queuedPacket = [&](std::uint32_t queue, HwSubmitDesc desc) {
            auto p = request(Op::QueueHwCommand, queue, desc); p.insert(p.end(), {37, 38, 39, 40}); return p;
        };
        auto submitBefore = submission.calls;
        submission.submitEnabled = false;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -95);
        submission.submitEnabled = true;
        require(header(s.dispatch(submitPacket(submitDevice, validSubmit))).status == -9);
        require(header(s.dispatch(request(Op::SubmitHwQueue, submitQueue))).status == -22);
        for (unsigned n = 0; n < 11; ++n) {
            auto invalid = validSubmit;
            switch (n) {
                case 0: invalid.address = 0; break;
                case 1: ++invalid.address; break;
                case 2: invalid.address = MaxGpuAddress - 4096; invalid.bytes = 8192; break;
                case 3: invalid.bytes = 0; break;
                case 4: ++invalid.bytes; break;
                case 5: invalid.bytes = MaxAllocation + 4096; break;
                case 6: invalid.fence = 0; break;
                case 7: invalid.fence = UINT64_MAX; break;
                case 8: invalid.privateBytes = 4001; break;
                case 9: invalid.primaries = 1; break;
                default: invalid.reserved = 1; break;
            }
            require(header(s.dispatch(submitPacket(submitQueue, invalid))).status == -22);
        }
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -9 && submission.calls == submitBefore);
        const GpuVaDesc submitMapping{submitPaging, 0, 65536, 0, 0, 0, 16, 1, 0};
        require(header(s.dispatch(request(Op::MapVendorAllocation, submitAllocation, submitMapping))).status == 0);
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -16 && submission.calls == submitBefore);
        auto submitResidency = request(Op::MakeVendorResident, submitPaging, ResidentDesc{1, 1, 0, 0});
        const auto submitIdBytes = reinterpret_cast<const std::uint8_t*>(&submitAllocation);
        submitResidency.insert(submitResidency.end(), submitIdBytes, submitIdBytes + 4);
        require(header(s.dispatch(submitResidency)).status == 0);
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -16 && submission.calls == submitBefore);
        require(header(s.dispatch(request(Op::LockVendorAllocation, submitAllocation, VendorCpuDesc{submitDevice, 0}))).status == 0);
        // Mesa may allocate buffers it has not yet made resident. They must
        // not prevent submission of a different, resident command buffer.
        auto unusedAllocation = request(Op::CreateVendorAllocation, submitDevice, VendorAllocationDesc{0, 0, 0, 1, 0, 0});
        unusedAllocation.push_back(37);
        require(header(s.dispatch(unusedAllocation)).handle != 0);
        auto submitOutside = validSubmit; submitOutside.address = 131072;
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, submitOutside))).status == -9 && submission.calls == submitBefore);
        auto submitOverrun = validSubmit; submitOverrun.address += 61440; submitOverrun.bytes = 8192;
        require(header(s.dispatch(submitPacket(submitQueue, submitOverrun))).status == -9 && submission.calls == submitBefore);
        submission.fail = true; auto submitOut = s.dispatch(submitPacket(submitQueue, validSubmit)); Reply submitNt{};
        std::memcpy(&submitNt, submitOut.data() + sizeof(Header), sizeof submitNt);
        require(header(submitOut).handle == submitQueue && !header(submitOut).status && submitNt.ntstatus == -123 && !submitNt.value);
        submission.badSubmitReply = 5; require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -5);
        submission.fail = false;
        for (int malformedSubmit = 1; malformedSubmit <= 4; ++malformedSubmit) {
            submission.badSubmitReply = malformedSubmit;
            require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -5);
        }
        submission.badSubmitReply = 0;
        // A negotiated queue submission may return before its GPU fence.
        // Reject device loss and malformed status/handle even in that mode.
        submission.asyncSubmitEnabled = true;
        for (int malformedSubmit : {1,2,4}) {
            submission.badSubmitReply = malformedSubmit;
            require(header(s.dispatch(queuedPacket(submitQueue, validSubmit))).status == -5);
        }
        require(s.acceptedSubmissionActivity().exact && !s.acceptedSubmissionActivity().acceptedCommands &&
                !s.acceptedSubmissionActivity().acceptedCommandBytes);
        submission.badSubmitReply = 3;
        // Advertising async does not change the legacy opcode's contract.
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -5);
        submitOut = s.dispatch(queuedPacket(submitQueue, validSubmit));
        std::memcpy(&submitNt, submitOut.data() + sizeof(Header), sizeof submitNt);
        require(!header(submitOut).status && submitNt.ntstatus == 0 && submitNt.value == validSubmit.fence - 1);
        require(s.acceptedSubmissionActivity().acceptedCommands==1 && s.acceptedSubmissionActivity().asynchronousCommands==1 &&
                s.acceptedSubmissionActivity().acceptedCommandBytes==validSubmit.bytes);
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -22 && submission.calls == submitBefore);
        auto synchronousNext = validSubmit; ++synchronousNext.fence;
        submission.asyncSubmitEnabled = false;
        submitBefore = submission.calls;
        require(header(s.dispatch(queuedPacket(submitQueue, synchronousNext))).status == -95 && submission.calls == submitBefore);
        require(header(s.dispatch(submitPacket(submitQueue, synchronousNext))).status == -5);
        submission.badSubmitReply = 0;
        submitOut = s.dispatch(submitPacket(submitQueue, synchronousNext));
        std::memcpy(&submitNt, submitOut.data() + sizeof(Header), sizeof submitNt);
        require(!header(submitOut).status && submitNt.ntstatus == 0 && submitNt.value == synchronousNext.fence);
        require(s.acceptedSubmissionActivity().exact && s.acceptedSubmissionActivity().acceptedCommands==2 &&
                s.acceptedSubmissionActivity().asynchronousCommands==1 && s.acceptedSubmissionActivity().acceptedCommandBytes==8192);
        {
            Session other(submission);
            require(other.acceptedSubmissionActivity().exact && !other.acceptedSubmissionActivity().acceptedCommands);
            require(s.acceptedSubmissionActivity().acceptedCommands==2);
        }
        submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, validSubmit))).status == -22 && submission.calls == submitBefore);
        auto submitNext = synchronousNext;
        while (submission.submitCalls < MaxHwSubmissions) {
            ++submitNext.fence; require(header(s.dispatch(submitPacket(submitQueue, submitNext))).status == 0);
        }
        ++submitNext.fence; submitBefore = submission.calls;
        require(header(s.dispatch(submitPacket(submitQueue, submitNext))).status == -24 && submission.calls == submitBefore);
    }
    require(submission.hwQueueParents.empty() && submission.vendorOwners.empty());
    Fake priorityDriver;
    {
        Session s(priorityDriver);
        auto priorityPacket = request(Op::SetContextInProcessPriority, 99, std::int32_t{0});
        require(header(s.dispatch(priorityPacket)).status == -71); s.dispatch(hello());
        priorityDriver.contextsEnabled = false;
        require(header(s.dispatch(priorityPacket)).status == -95); priorityDriver.contextsEnabled = true;
        require(header(s.dispatch(priorityPacket)).status == -9);
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        auto contextPacket = request(Op::CreateContext, d, ContextDesc{0, 1, 16, 12, 4, 0});
        contextPacket.insert(contextPacket.end(), {1, 2, 3, 4});
        const auto context = header(s.dispatch(contextPacket)).handle;
        const auto before = priorityDriver.calls;
        for (const auto invalid : {INT32_MIN, -1, 2, 7, INT32_MAX})
            require(header(s.dispatch(request(Op::SetContextInProcessPriority, context, invalid))).status == -22);
        require(header(s.dispatch(request(Op::SetContextInProcessPriority, d, std::int32_t{0}))).status == -9);
        require(header(s.dispatch(request(Op::SetContextInProcessPriority, context))).status == -22);
        priorityPacket = request(Op::SetContextInProcessPriority, context, std::int32_t{0}); priorityPacket.push_back(0);
        require(header(s.dispatch(priorityPacket)).status == -22 && priorityDriver.calls == before);
        priorityPacket.pop_back();
        priorityDriver.fail = true; auto out = s.dispatch(priorityPacket); Reply body{};
        std::memcpy(&body, out.data() + sizeof(Header), sizeof body);
        require(header(out).status == 0 && body.ntstatus == -123 && header(out).handle == context);
        priorityDriver.fail = false;
        for (int bad = 1; bad <= 3; ++bad) {
            priorityDriver.badContextPriorityReply = bad;
            require(header(s.dispatch(priorityPacket)).status == -5);
        }
        priorityDriver.badContextPriorityReply = 0;
        require(header(s.dispatch(priorityPacket)).status == 0);
        require(header(s.dispatch(request(Op::SetContextInProcessPriority, context, std::int32_t{1}))).status == 0);
        require(header(s.dispatch(request(Op::DestroyContext, context))).status == 0);
        const auto afterDestroy = priorityDriver.calls;
        require(header(s.dispatch(priorityPacket)).status == -9 && priorityDriver.calls == afterDestroy);
    }
    Fake pagingQuota; pagingQuota.guestPagingEnabled = true;
    {
        Session s(pagingQuota); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        for (std::size_t n = 0; n < (MaxObjects - 2) / 2; ++n)
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0);
        require(s.objectCount() == MaxObjects);
        const auto before = pagingQuota.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -24 && pagingQuota.calls == before);
    }
    // A two-handle queue cannot consume the last singleton slot. Retiring
    // that singleton opens exactly two slots, without lifting the aggregate cap.
    {
        Session s(pagingQuota); s.dispatch(hello());
        const auto a = header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto d = header(s.dispatch(request(Op::CreateDevice, a))).handle;
        for (std::size_t n = 0; n < (MaxObjects - 4) / 2; ++n)
            require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0);
        const auto spare = header(s.dispatch(request(Op::OpenAdapter))).handle;
        require(spare && s.objectCount() == MaxObjects - 1);
        const auto before = pagingQuota.calls;
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).status == -24 && pagingQuota.calls == before);
        require(header(s.dispatch(request(Op::CloseAdapter, spare))).status == 0 && s.objectCount() == MaxObjects - 2);
        require(header(s.dispatch(request(Op::CreateGuestPagingQueue, d))).handle != 0 && s.objectCount() == MaxObjects);
    }
    Fake contextSignals; contextSignals.contextSignalEnabled = true; contextSignals.contextsEnabled = true; contextSignals.syncEnabled = true;
    {
        Session signalSession(contextSignals);
        require(header(signalSession.dispatch(request(Op::SignalContextSync,1,ContextSignalDesc{2,4,1}))).status == -71);
        signalSession.dispatch(hello());
        const auto adapterId = header(signalSession.dispatch(request(Op::OpenAdapter))).handle;
        const auto deviceId = header(signalSession.dispatch(request(Op::CreateDevice,adapterId))).handle;
        const auto otherDevice = header(signalSession.dispatch(request(Op::CreateDevice,adapterId))).handle;
        auto contextPacket = request(Op::CreateContext,deviceId,ContextDesc{0,1,16,12,1,0}); contextPacket.push_back(37);
        const auto contextId = header(signalSession.dispatch(contextPacket)).handle;
        const auto fenceId = header(signalSession.dispatch(request(Op::CreateSync,deviceId,SyncDesc{5,128,0,0,0}))).handle;
        const auto otherFence = header(signalSession.dispatch(request(Op::CreateSync,otherDevice,SyncDesc{5,128,0,0,0}))).handle;
        const auto mutexId = header(signalSession.dispatch(request(Op::CreateSync,deviceId,SyncDesc{1,0,0,0,0}))).handle;
        const auto gpuFence = header(signalSession.dispatch(request(Op::CreateSync,deviceId,SyncDesc{5,0,0,0,0}))).handle;
        const auto before = contextSignals.calls;
        contextSignals.contextSignalEnabled = false;
        require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,1}))).status == -95);
        contextSignals.contextSignalEnabled = true;
        for (const auto desc : {ContextSignalDesc{0,4,1},ContextSignalDesc{fenceId,0,1},ContextSignalDesc{fenceId,5,1},
                               ContextSignalDesc{fenceId,4,0},ContextSignalDesc{fenceId,4,UINT64_MAX}})
            require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,desc))).status == -22);
        for (const auto bad : {otherFence,mutexId,gpuFence,deviceId,999u})
            require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{bad,4,1}))).status == -9);
        require(header(signalSession.dispatch(request(Op::SignalContextSync,deviceId,ContextSignalDesc{fenceId,4,1}))).status == -9);
        require(contextSignals.calls == before);
        for (const auto flags : {NtSharedNoGpuAccessSyncFlags,
                                 NtSharedNoGpuAccessSyncFlags | UnwaitCpuWaitersOnlyOnDestroySyncFlag}) {
            const auto shared = header(signalSession.dispatch(request(Op::CreateSync,deviceId,SyncDesc{5,flags,0,0,0}))).handle;
            require(shared && !header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{shared,4,1}))).status);
            require(!header(signalSession.dispatch(request(Op::DestroySync,shared))).status);
        }
        require(!header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,1}))).status);
        require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,1}))).status == -22);
        for (const auto mode : {1,2,3,4}) {
            contextSignals.badContextSignalReply = mode;
            require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,2}))).status == -5);
        }
        contextSignals.badContextSignalReply = 0; contextSignals.fail = true;
        auto out = signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,2})); Reply nativeFailure{};
        std::memcpy(&nativeFailure,out.data()+sizeof(Header),sizeof nativeFailure);
        require(!header(out).status && nativeFailure.ntstatus == -123);
        contextSignals.badContextSignalReply = 5;
        require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,2}))).status == -5);
        contextSignals.fail = false; contextSignals.badContextSignalReply = 0;
        std::uint64_t target = 2;
        while (contextSignals.contextSignalCalls < MaxContextSignals)
            require(!header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,target++}))).status);
        const auto after = contextSignals.calls;
        require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,target}))).status == -24 && contextSignals.calls == after);
        require(!header(signalSession.dispatch(request(Op::DestroySync,fenceId))).status);
        require(header(signalSession.dispatch(request(Op::SignalContextSync,contextId,ContextSignalDesc{fenceId,4,target}))).status == -9);
    }
    require(contextSignals.syncParents.empty());
    Fake hwSignals; hwSignals.hwQueueSignalEnabled=hwSignals.hwQueuesEnabled=hwSignals.syncEnabled=hwSignals.guestPagingEnabled=true;
    {
        Session session(hwSignals);
        auto signal=[&](std::uint32_t sync, std::vector<std::uint32_t> queues, std::uint64_t value, std::uint32_t flags=0) {
            auto packet=request(Op::QueueHwQueueSignal,sync,HwQueueSignalDesc{flags,static_cast<std::uint32_t>(queues.size()),value});
            const auto data=reinterpret_cast<const std::uint8_t*>(queues.data());
            if (!queues.empty()) packet.insert(packet.end(),data,data+queues.size()*sizeof(queues[0]));
            return session.dispatch(packet);
        };
        require(header(signal(1,{2},1)).status == -71); session.dispatch(hello());
        const auto adapter=header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device=header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto other=header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        auto contextPacket=request(Op::CreateContext,device,ContextDesc{0,1,16,12,4,0});
        contextPacket.insert(contextPacket.end(),{1,2,3,4});
        const auto context=header(session.dispatch(contextPacket)).handle;
        auto queuePacket=request(Op::CreateHwQueue,context,HwQueueDesc{0,4,0,0}); queuePacket.insert(queuePacket.end(),{37,38,39,40});
        const auto q1=header(session.dispatch(queuePacket)).handle, q2=header(session.dispatch(queuePacket)).handle;
        const auto fence=header(session.dispatch(request(Op::CreateSync,device,SyncDesc{5,NoGpuAccessSyncFlag,1,0,0}))).handle;
        const auto foreign=header(session.dispatch(request(Op::CreateSync,other,SyncDesc{5,NoGpuAccessSyncFlag,1,0,0}))).handle;
        const auto gpuFence=header(session.dispatch(request(Op::CreateSync,device,SyncDesc{5,0,1,0,0}))).handle;
        auto before=hwSignals.calls;
        hwSignals.hwQueueSignalEnabled=false;
        require(header(signal(fence,{q1},1)).status == -95); hwSignals.hwQueueSignalEnabled=true;
        for (const auto flags : {1u,2u,5u,8u,UINT32_MAX}) require(header(signal(fence,{q1},1,flags)).status == -22);
        require(header(signal(fence,{},1)).status == -22);
        require(header(signal(fence,std::vector<std::uint32_t>(MaxHwQueues+1,q1),1)).status == -22);
        require(header(signal(fence,{q1},0)).status == -22);
        require(header(signal(fence,{q1},UINT64_MAX)).status == -22);
        require(header(signal(fence,{q1,q1},1)).status == -9);
        for (auto bad : {0u,device,context,999u}) require(header(signal(fence,{bad},1)).status == -9);
        for (auto bad : {foreign,gpuFence,q1,999u}) require(header(signal(bad,{q1},1)).status == -9);
        require(hwSignals.calls == before);
        for (int bad : {1,2,3}) { hwSignals.badHwQueueSignalReply=bad; require(header(signal(fence,{q1},1)).status == -5); }
        hwSignals.fail=true; hwSignals.badHwQueueSignalReply=4;
        require(header(signal(fence,{q1},1)).status == -5);
        hwSignals.badHwQueueSignalReply=0;
        auto out=signal(fence,{q1},1); Reply failure{}; std::memcpy(&failure,out.data()+sizeof(Header),sizeof failure);
        require(!header(out).status && failure.ntstatus == -123 && !failure.value);
        hwSignals.fail=false;
        out=signal(fence,{q2,q1},1,4); Reply queued{}; std::memcpy(&queued,out.data()+sizeof(Header),sizeof queued);
        require(!header(out).status && !queued.ntstatus && !queued.value && hwSignals.lastSignalQueues.size() == 2 &&
                hwSignals.lastSignalQueues[0] != hwSignals.lastSignalQueues[1]);
        before=hwSignals.calls; require(header(signal(fence,{q1},1)).status == -22 && hwSignals.calls == before);
        std::uint64_t value=2;
        while (hwSignals.hwQueueSignalCalls < MaxHwQueueSignals) require(!header(signal(fence,{q1},value++)).status);
        before=hwSignals.calls; require(header(signal(fence,{q1},value)).status == -24 && hwSignals.calls == before);
    }
    require(hwSignals.hwQueueParents.empty() && hwSignals.syncParents.empty());
    Fake resources; resources.vendorEnabled = true; resources.resourceEnabled = true;
    {
        Session session(resources);
        auto create = [&](std::uint32_t device) {
            auto packet = request(Op::CreateVendorResourceAllocation, device, VendorAllocationDesc{4, 0x78100000, UninitializedDisplaySource, 4, 0, 0});
            packet.insert(packet.end(), {37, 38, 39, 40}); return packet;
        };
        require(header(session.dispatch(create(1))).status == -71);
        session.dispatch(hello());
        const auto adapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(session.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto other = header(session.dispatch(request(Op::CreateDevice, adapter))).handle;
        const auto before = resources.calls;
        resources.resourceEnabled = false;
        require(header(session.dispatch(create(device))).status == -95 && resources.calls == before);
        resources.resourceEnabled = true;
        auto malformedPacket = create(device); malformedPacket.pop_back();
        require(header(session.dispatch(malformedPacket)).status == -22 && resources.calls == before);
        for (const auto mode : {1, 2}) {
            resources.badResourceReply = mode;
            require(header(session.dispatch(create(device))).status == -5 && resources.vendorOwners.empty() && resources.vendorResources.empty());
        }
        resources.fail = true; resources.badResourceReply = 0;
        auto out = session.dispatch(create(device)); Reply failed{}; VendorResourceReply absent{};
        std::memcpy(&failed, out.data() + sizeof(Header), sizeof failed);
        std::memcpy(&absent, out.data() + sizeof(Header) + sizeof failed, sizeof absent);
        require(!header(out).status && !header(out).handle && failed.ntstatus == -123 && !absent.resource);
        resources.badResourceReply = 3;
        require(header(session.dispatch(create(device))).status == -5);
        resources.fail = false; resources.badResourceReply = 0;
        out = session.dispatch(create(device));
        const auto allocation = header(out).handle; VendorResourceReply resource{};
        std::memcpy(&resource, out.data() + sizeof(Header) + sizeof(Reply), sizeof resource);
        require(allocation && resource.resource && resource.resource != allocation && !resource.reserved);
        require(resources.vendorOwners.size() == 1 && resources.vendorResources.size() == 1 && session.objectCount() == 5);
        require(header(session.dispatch(request(Op::DestroyVendorResource, allocation, device))).status == -9);
        require(header(session.dispatch(request(Op::DestroyVendorResource, resource.resource, other))).status == -9);
        require(header(session.dispatch(request(Op::DestroyDevice, device))).status == -16);
        resources.fail = true;
        out = session.dispatch(request(Op::DestroyVendorResource, resource.resource, device));
        std::memcpy(&failed, out.data() + sizeof(Header), sizeof failed);
        require(!header(out).status && header(out).handle == resource.resource && failed.ntstatus == -123 && session.objectCount() == 5);
        resources.fail = false;
        require(!header(session.dispatch(request(Op::DestroyVendorResource, resource.resource, device))).status && session.objectCount() == 3);
        require(header(session.dispatch(request(Op::DestroyVendorResource, resource.resource, device))).status == -9);
        out = session.dispatch(create(device));
        const auto second = header(out).handle;
        resources.guestPagingEnabled = true; resources.gpuVaEnabled = true;
        const auto resourcePaging = header(session.dispatch(request(Op::CreateGuestPagingQueue, device))).handle;
        const GpuVaDesc protectedMap{resourcePaging,0,0,67108864,1099511627776ull,0,16,1,VendorDefaultDriverProtection};
        require(!header(session.dispatch(request(Op::MapVendorAllocation, second, protectedMap))).status);
        auto release = request(Op::DestroyVendorAllocations, device, DestroyVendorDesc{1, 0});
        const auto bytes = reinterpret_cast<const std::uint8_t*>(&second); release.insert(release.end(), bytes, bytes + sizeof second);
        require(!header(session.dispatch(release)).status && resources.vendorResources.empty() && session.objectCount() == 5);
        auto standalone = request(Op::CreateVendorAllocation, device, VendorAllocationDesc{0,0,0,1,0,0}); standalone.push_back(37);
        const auto standaloneId = header(session.dispatch(standalone)).handle;
        const auto nativeBeforeMap = resources.calls;
        require(header(session.dispatch(request(Op::MapVendorAllocation, standaloneId, protectedMap))).status == -22 && resources.calls == nativeBeforeMap);
        require(header(session.dispatch(create(device))).handle != 0); // Disconnect also releases its resource.
    }
    require(resources.vendorResources.empty() && resources.vendorOwners.empty());
    {
        Session session(resources); session.dispatch(hello());
        const auto adapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        std::uint32_t device = 0;
        for (std::size_t n = 0; n < MaxObjects - 2; ++n) device = header(session.dispatch(request(Op::CreateDevice, adapter))).handle;
        auto packet = request(Op::CreateVendorResourceAllocation, device, VendorAllocationDesc{0,0,0,1,0,0}); packet.push_back(37);
        const auto before = resources.calls;
        require(session.objectCount() == MaxObjects - 1 && header(session.dispatch(packet)).status == -24 && resources.calls == before);
    }
    Fake shared; shared.vendorEnabled = shared.resourceEnabled = true;
    {
        Session session(shared);
        SharedVendorAllocationDesc desc{{4,0x78100000,UninitializedDisplaySource,4,0,0},4,0};
        auto create = [&](std::uint32_t device, SharedVendorAllocationDesc d) {
            auto packet = request(Op::CreateSharedVendorResourceAllocation,device,d);
            packet.insert(packet.end(),{37,38,39,40,5,6,7,8}); return packet;
        };
        require(header(session.dispatch(create(1,desc))).status == -71);
        session.dispatch(hello());
        const auto adapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto other = header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto before = shared.calls;
        require(header(session.dispatch(create(device,desc))).status == -95 && shared.calls == before);
        shared.sharedResourceEnabled = true;
        require(header(session.dispatch(create(adapter,desc))).status == -9 && shared.calls == before);
        for (int mode = 0; mode < 8; ++mode) {
            auto invalid = desc;
            if (mode == 0) invalid.runtimeBytes = 0;
            if (mode == 1) invalid.runtimeBytes = 1025;
            if (mode == 2) invalid.reserved = 1;
            if (mode == 3) invalid.allocation.reserved2 = 1;
            if (mode == 4) invalid.allocation.flags = 0;
            if (mode == 5) invalid.allocation.privateBytes = 4001;
            auto packet = create(device,invalid);
            if (mode == 6) packet.pop_back();
            if (mode == 7) packet.push_back(0);
            require(header(session.dispatch(packet)).status == -22 && shared.calls == before);
        }
        auto maximum = desc; maximum.runtimeBytes = 1024; maximum.allocation.privateBytes = 3024;
        require(validSharedVendorAllocation(maximum)); ++maximum.allocation.privateBytes;
        require(!validSharedVendorAllocation(maximum));
        shared.fail = true;
        auto out = session.dispatch(create(device,desc)); Reply failure{}; VendorResourceReply absent{};
        std::memcpy(&failure,out.data()+sizeof(Header),sizeof failure);
        std::memcpy(&absent,out.data()+sizeof(Header)+sizeof failure,sizeof absent);
        require(!header(out).status && !header(out).handle && failure.ntstatus == -123 && !absent.resource);
        require(out.back() == 8 && shared.vendorOwners.empty());
        shared.fail = false;
        shared.badVendorReply = 1;
        require(header(session.dispatch(create(device,desc))).status == -5 && shared.vendorOwners.empty());
        shared.badVendorReply = 0;
        out = session.dispatch(create(device,desc)); VendorResourceReply resource{};
        std::memcpy(&resource,out.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
        require(!header(out).status && header(out).handle && resource.resource);
        require(shared.lastSharedRuntime == std::vector<std::uint8_t>({5,6,7,8}));
        require(out[out.size()-8] == (37 ^ 255) && out.back() == 8);
        require(header(session.dispatch(request(Op::DestroyVendorResource,resource.resource,other))).status == -9);
        require(!header(session.dispatch(request(Op::DestroyVendorResource,resource.resource,device))).status);
        require(shared.vendorOwners.empty() && shared.vendorResources.empty());
        require(header(session.dispatch(request(Op::DestroyVendorResource,resource.resource,device))).status == -9);
    }
    Fake native; native.vendorEnabled = native.resourceEnabled = native.sharedResourceEnabled = true;
    {
        Session session(native);
        NativeSharedVendorAllocationDesc desc{{{4,0x78100000,0,4,0,0},4,0},{130,73,28,1}};
        auto create = [&](std::uint32_t device, NativeSharedVendorAllocationDesc d) {
            auto p = request(Op::CreateNativeSharedVendorResourceAllocation,device,d);
            p.insert(p.end(),{37,38,39,40,5,6,7,8}); return p;
        };
        require(header(session.dispatch(create(1,desc))).status == -71);
        session.dispatch(hello());
        const auto adapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        require(header(session.dispatch(create(device,desc))).status == -95 && !native.nativeTextureCalls);
        native.nativeTextureEnabled = true;
        require(header(session.dispatch(create(adapter,desc))).status == -9 && !native.nativeTextureCalls);
        for (unsigned n=0;n<10;++n) {
            auto d = desc;
            if (n==0) d.texture.width=0;
            if (n==1) d.texture.height=74;
            if (n==2) d.texture.format=87;
            if (n==3) d.texture.flags=3;
            if (n==4) d.shared.reserved=1;
            if (n==5) d.shared.runtimeBytes=0;
            if (n==6) d.shared.allocation.flags=0;
            if (n==7) d.shared.allocation.privateBytes=UINT32_MAX;
            auto p=create(device,d);
            if (n==8) p.pop_back();
            if (n==9) p.push_back(0);
            require(header(session.dispatch(p)).status == -22 && !native.nativeTextureCalls);
        }
        auto maximum=desc; maximum.shared.runtimeBytes=1024; maximum.shared.allocation.privateBytes=3008;
        require(validNativeSharedVendorAllocation(maximum)); ++maximum.shared.allocation.privateBytes;
        require(!validNativeSharedVendorAllocation(maximum));
        native.fail=true;
        auto out=session.dispatch(create(device,desc));
        require(!header(out).status && !header(out).handle && native.vendorOwners.empty() && out.back()==8);
        native.fail=false; native.badResourceReply=1;
        require(header(session.dispatch(create(device,desc))).status==-5 && native.vendorOwners.empty());
        native.badResourceReply=0;
        out=session.dispatch(create(device,desc)); VendorResourceReply resource{};
        std::memcpy(&resource,out.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
        require(!header(out).status && header(out).handle && resource.resource && native.nativeTextureCalls==3);
        require(native.lastSharedRuntime==std::vector<std::uint8_t>({5,6,7,8}));
        require(out[out.size()-8]==(37^255) && out.back()==8);
        require(!header(session.dispatch(request(Op::DestroyVendorResource,resource.resource,device))).status);
        require(native.vendorOwners.empty() && native.vendorResources.empty());
    }
    // Native resource references are separately negotiated and typed. They
    // outlive the source resource/device but retain their owning adapter.
    Fake exported; exported.vendorEnabled = exported.resourceEnabled = exported.sharedResourceEnabled = exported.nativeTextureEnabled = true;
    for(const auto version:{3u,4u}) {
        struct PhysicalFake : OpenGlFake {
            std::set<std::uint32_t> sameGpu;
            bool equivalent=true;
            Result openAdapter() override {
                auto result=Fake::openAdapter();
                if(sameGpu.size()<2) sameGpu.insert(result.nativeHandle);
                return result;
            }
            bool samePhysicalAdapter(std::uint32_t a,std::uint32_t b) const override {
                return equivalent && sameGpu.count(a) && sameGpu.count(b);
            }
        } f;
        f.profile=version==4 ? GuestOpenGlCompositorRenderProfile : GuestOpenGlCompositorProfile;
        f.vendorEnabled=f.resourceEnabled=f.sharedResourceEnabled=f.nativeTextureEnabled=true;
        Session s(f); s.dispatch(hello()); require(!header(s.dispatch(request(Op::BeginOpenGl,0,version))).status);
        require(!header(s.dispatch(request(Op::BeginResourceSharing))).status);
        const auto a=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto first=header(s.dispatch(request(Op::CreateDevice,a))).handle;
        const auto b=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto second=header(s.dispatch(request(Op::CreateDevice,b))).handle;
        const auto foreign=header(s.dispatch(request(Op::OpenAdapter))).handle;
        const auto foreignDevice=header(s.dispatch(request(Op::CreateDevice,foreign))).handle;
        NativeSharedVendorAllocationDesc desc{{{4,0x78100000,0,4,0,0},4,0},{130,73,28,1}};
        auto create=request(Op::CreateNativeSharedVendorResourceAllocation,first,desc);
        create.insert(create.end(),{37,38,39,40,5,6,7,8});
        const auto out=s.dispatch(create); VendorResourceReply resource{};
        require(!header(out).status && out.size()>=sizeof(Header)+sizeof(Reply)+sizeof resource);
        std::memcpy(&resource,out.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
        const auto ticket=header(s.dispatch(request(Op::ExportResource,resource.resource))).handle; require(ticket);
        auto query=[&](std::uint32_t device) { return s.dispatch(request(Op::QuerySharedResourceInfo,ticket,SharedResourceQueryDesc{device,0})); };
        auto open=[&](std::uint32_t device) { return s.dispatch(request(Op::ImportResource,ticket,SharedResourceOpenDesc{device,4,8,0})); };
        require(!header(query(first)).status);
        require(header(query(foreignDevice)).status==-9 && header(open(foreignDevice)).status==-9);
        require(header(query(b)).status==-9 && header(open(0)).status==-22);
        f.equivalent=false;
        require(header(query(second)).status==-9 && header(open(second)).status==-9);
        f.equivalent=true;
        require(header(query(second)).status==(version==4?0:-9));
        require(header(open(second)).status==(version==4?0:-9));
        require(!s.resourceAdaptersCompatible(a,UINT32_MAX));
    }
    {
        Session session(exported);
        require(header(session.dispatch(request(Op::BeginResourceSharing))).status == -71);
        require(header(session.dispatch(request(Op::ExportResource, 1))).status == -71);
        session.dispatch(hello());
        auto malformed = request(Op::BeginResourceSharing); malformed.push_back(0);
        require(header(session.dispatch(malformed)).status == -22);
        exported.nativeTextureEnabled = false;
        require(header(session.dispatch(request(Op::BeginResourceSharing))).status == -95);
        exported.nativeTextureEnabled = true; exported.resourceSharingSupported = false;
        require(header(session.dispatch(request(Op::BeginResourceSharing))).status == -95);
        exported.resourceSharingSupported = true;
        auto out = session.dispatch(request(Op::BeginResourceSharing));
        ResourceSharingProfile profile{};
        require(!header(out).status && out.size() == sizeof(Header) + sizeof profile);
        std::memcpy(&profile, out.data() + sizeof(Header), sizeof profile);
        require(validResourceSharingProfile(profile)); ++profile.handleLimit; require(!validResourceSharingProfile(profile));
        require(header(session.dispatch(request(Op::BeginResourceSharing))).status == -16);
        const auto adapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(session.dispatch(request(Op::CreateDevice, adapter))).handle;
        NativeSharedVendorAllocationDesc desc{{{4,0x78100000,0,4,0,0},4,0},{130,73,28,1}};
        auto packet = request(Op::CreateNativeSharedVendorResourceAllocation, device, desc);
        packet.insert(packet.end(), {37,38,39,40,5,6,7,8});
        out = session.dispatch(packet);
        VendorResourceReply source{};
        std::memcpy(&source, out.data() + sizeof(Header) + sizeof(Reply), sizeof source);
        require(!header(out).status && source.resource);
        const auto allocation = header(out).handle;
        require(header(session.dispatch(request(Op::ExportResource, allocation))).status == -9);
        require(header(session.dispatch(request(Op::CloseSharedResource, source.resource))).status == -9);
        exported.fail = true;
        out = session.dispatch(request(Op::ExportResource, source.resource));
        Reply failure{}; std::memcpy(&failure, out.data() + sizeof(Header), sizeof failure);
        require(!header(out).status && !header(out).handle && failure.ntstatus == -123 && exported.resourceTickets.empty());
        exported.fail = false;
        for (int bad = 1; bad <= 2; ++bad) {
            exported.badResourceExport = bad;
            require(header(session.dispatch(request(Op::ExportResource, source.resource))).status == -5 && exported.resourceTickets.empty());
        }
        exported.badResourceExport = 0;
        std::vector<std::uint32_t> tickets;
        for (std::size_t n = 0; n < MaxSharedResourceHandles; ++n) {
            out = session.dispatch(request(Op::ExportResource, source.resource));
            require(!header(out).status && header(out).handle);
            tickets.push_back(header(out).handle);
        }
        const auto calls = exported.resourceExports;
        require(header(session.dispatch(request(Op::ExportResource, source.resource))).status == -24 && exported.resourceExports == calls);
        auto query = [&](std::uint32_t id, SharedResourceQueryDesc desc) { return session.dispatch(request(Op::QuerySharedResourceInfo, id, desc)); };
        require(header(query(source.resource, {device,0})).status == -9);
        require(header(query(tickets.front(), {allocation,0})).status == -9);
        require(header(query(tickets.front(), {device,MaxSharedResourceRuntimeBytes+1})).status == -22);
        const auto otherAdapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto otherDevice = header(session.dispatch(request(Op::CreateDevice, otherAdapter))).handle;
        require(header(query(tickets.front(), {otherDevice,0})).status == -9);
        require(!header(session.dispatch(request(Op::DestroyDevice, otherDevice))).status);
        require(!header(session.dispatch(request(Op::CloseAdapter, otherAdapter))).status);
        for (int bad = 1; bad <= 6; ++bad) {
            exported.badResourceInfo = bad;
            require(header(query(tickets.front(), {device,0})).status == -5);
        }
        exported.badResourceInfo = 0;
        out = query(tickets.front(), {device,0});
        SharedResourceInfo metadata{}; std::memcpy(&metadata, out.data()+sizeof(Header)+sizeof(Reply), sizeof metadata);
        require(!header(out).status && validSharedResourceInfo(metadata) && metadata.runtimeBytes == 4 && out.size() == sizeof(Header)+sizeof(Reply)+sizeof metadata);
        require(header(query(tickets.front(), {device,3})).status == -22);
        out = query(tickets.front(), {device,4});
        require(!header(out).status && out.back() == 8 && out.size() == sizeof(Header)+sizeof(Reply)+sizeof metadata+4);
        exported.fail = true; out = query(tickets.front(), {device,4});
        require(!header(out).status && out.back() == 0); exported.fail = false;
        require(header(session.dispatch(request(Op::CloseSharedSync, tickets.front()))).status == -71);
        malformed = request(Op::CloseSharedResource, tickets.front()); malformed.push_back(0);
        require(header(session.dispatch(malformed)).status == -22);
        exported.fail = true;
        out = session.dispatch(request(Op::CloseSharedResource, tickets.front()));
        require(!header(out).status && exported.resourceTickets.size() == MaxSharedResourceHandles);
        exported.fail = false;
        const auto importDevice = header(session.dispatch(request(Op::CreateDevice, adapter))).handle;
        const SharedResourceOpenDesc importDesc{importDevice,4,8,0};
        auto import = [&](SharedResourceOpenDesc d) { return session.dispatch(request(Op::ImportResource, tickets.front(), d)); };
        auto badDesc = importDesc; badDesc.runtimeBytes = 0;
        require(header(import(badDesc)).status == -22);
        badDesc = importDesc; badDesc.device = allocation;
        require(header(import(badDesc)).status == -9);
        const auto vendorCount = exported.vendorOwners.size();
        for (int bad = 1; bad <= 8; ++bad) {
            exported.badResourceImport = bad;
            require(header(import(importDesc)).status == -5 && exported.vendorOwners.size() == vendorCount);
        }
        exported.badResourceImport = 0; exported.fail = true;
        out = import(importDesc); require(!header(out).status && !header(out).handle && out.back() == 0);
        exported.fail = false; out = import(importDesc);
        SharedResourceOpenReply imported{}; std::memcpy(&imported, out.data()+sizeof(Header)+sizeof(Reply), sizeof imported);
        require(!header(out).status && header(out).handle && imported.resource && imported.allocationDriverBytes == 8 && out.back() == 42);
        require(header(session.dispatch(request(Op::DestroyVendorResource, imported.resource, device))).status == -9);
        require(!header(session.dispatch(request(Op::DestroyVendorResource, imported.resource, importDevice))).status);
        require(!header(session.dispatch(request(Op::DestroyDevice, importDevice))).status);
        require(!header(session.dispatch(request(Op::DestroyVendorResource, source.resource, device))).status);
        require(!header(session.dispatch(request(Op::DestroyDevice, device))).status);
        require(header(session.dispatch(request(Op::CloseAdapter, adapter))).status == -16);
        require(!header(session.dispatch(request(Op::CloseSharedResource, tickets.front()))).status);
        require(header(session.dispatch(request(Op::CloseSharedResource, tickets.front()))).status == -9);
        // Leave the remaining references for disconnect cleanup after their
        // source allocation and device have already been destroyed.
    }
    require(exported.resourceTickets.empty() && exported.vendorOwners.empty() && exported.vendorResources.empty());
    std::cout << "PASS: typed native resource export negotiation, quota, rollback, adapter lifetime and disconnect cleanup\n";
    Fake consumer; consumer.vendorEnabled = consumer.resourceEnabled = consumer.sharedResourceEnabled = consumer.syncEnabled = true;
    {
        Session session(consumer);
        const SharedTextureConsumeDesc desc{1,0,1};
        require(header(session.dispatch(request(Op::ConsumeSharedTexture,1,desc))).status == -71);
        session.dispatch(hello());
        const auto adapter = header(session.dispatch(request(Op::OpenAdapter))).handle;
        const auto device = header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        const auto other = header(session.dispatch(request(Op::CreateDevice,adapter))).handle;
        auto packet = request(Op::CreateSharedVendorResourceAllocation,device,SharedVendorAllocationDesc{{4,0x78100000,0,4,0,0},4,0});
        packet.insert(packet.end(),{37,38,39,40,5,6,7,8});
        const auto out = session.dispatch(packet); VendorResourceReply resource{};
        std::memcpy(&resource,out.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
        const auto sync = header(session.dispatch(request(Op::CreateSync,device,SyncDesc{5,NoGpuAccessSyncFlag,0,0,0}))).handle;
        const SharedTextureConsumeDesc handoff{sync,0,1};
        auto before = consumer.calls;
        require(header(session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,handoff))).status == -95 && consumer.calls == before);
        consumer.consumeSharedEnabled = true;
        for (int mode=0;mode<6;++mode) {
            auto d = handoff;
            if (mode==0) d.sync=0;
            if (mode==1) d.reserved=1;
            if (mode==2) d.fence=0;
            if (mode==3) d.fence=UINT64_MAX;
            auto p = request(Op::ConsumeSharedTexture,resource.resource,d);
            if (mode==4) p.pop_back();
            if (mode==5) p.push_back(0);
            require(header(session.dispatch(p)).status == -22 && consumer.calls == before);
        }
        for (const auto wrongResource : {adapter,device,header(out).handle})
            require(header(session.dispatch(request(Op::ConsumeSharedTexture,wrongResource,handoff))).status == -9 && consumer.calls == before);
        const auto wrongDeviceSync = header(session.dispatch(request(Op::CreateSync,other,SyncDesc{5,NoGpuAccessSyncFlag,0,0,0}))).handle;
        const auto gpuSync = header(session.dispatch(request(Op::CreateSync,device,SyncDesc{5,0,0,0,0}))).handle;
        const auto mutex = header(session.dispatch(request(Op::CreateSync,device,SyncDesc{1,0,0,0,0}))).handle;
        before = consumer.calls;
        for (const auto wrongSync : {device,wrongDeviceSync,gpuSync,mutex})
            require(header(session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,SharedTextureConsumeDesc{wrongSync,0,1}))).status == -9 && consumer.calls == before);
        auto nonshared = request(Op::CreateVendorResourceAllocation,device,VendorAllocationDesc{0,0,0,1,0,0}); nonshared.push_back(37);
        const auto output = session.dispatch(nonshared); VendorResourceReply notShared{};
        std::memcpy(&notShared,output.data()+sizeof(Header)+sizeof(Reply),sizeof notShared);
        before = consumer.calls;
        require(header(session.dispatch(request(Op::ConsumeSharedTexture,notShared.resource,handoff))).status == -9 && consumer.calls == before);
        for (const auto bad : {1,2}) {
            consumer.badConsumeReply=bad;
            require(header(session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,handoff))).status == -5);
        }
        consumer.fail=true; consumer.badConsumeReply=3;
        require(header(session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,handoff))).status == -5);
        consumer.badConsumeReply=0;
        auto response=session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,handoff)); Reply r{};
        std::memcpy(&r,response.data()+sizeof(Header),sizeof r);
        require(!header(response).status && r.ntstatus == -123 && !r.value);
        consumer.fail=false;
        response=session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,handoff));
        std::memcpy(&r,response.data()+sizeof(Header),sizeof r);
        require(!header(response).status && header(response).handle==resource.resource && r.ntstatus==0 && r.value==42);
        require(!header(session.dispatch(request(Op::DestroyVendorResource,resource.resource,device))).status);
        before=consumer.calls;
        require(header(session.dispatch(request(Op::ConsumeSharedTexture,resource.resource,handoff))).status == -9 && consumer.calls == before);
    }
    for (unsigned scenario=0;scenario<12;++scenario) {
        const bool queued=scenario>=5;
        const unsigned replyMode=queued ? scenario-5 : scenario;
        const auto operation=queued ? Op::QueueCompositorFrame : Op::CaptureCompositorResource;
        struct CaptureFake : OpenGlFake {
            std::set<std::uint32_t> sameGpu, sharedFences;
            unsigned captures = 0;
            std::uint64_t frames = 0;
            bool retried = false;
            unsigned mode = 0;
            bool equivalent = true;
            CaptureFake() {
                profile=GuestOpenGlCompositorRenderProfile;
                vendorEnabled=resourceEnabled=sharedResourceEnabled=nativeTextureEnabled=consumeSharedEnabled=syncEnabled=true;
            }
            Result openAdapter() override {
                const auto result=Fake::openAdapter();
                if (sameGpu.size()<2) sameGpu.insert(result.nativeHandle);
                return result;
            }
            bool samePhysicalAdapter(std::uint32_t a,std::uint32_t b) const override {
                return equivalent && sameGpu.count(a) && sameGpu.count(b);
            }
            SyncSharingProfile beginSyncSharing() override { return GuestSyncSharingProfile; }
            Result exportSync(std::uint32_t sync) override {
                require(syncParents.count(sync)); const auto ticket=++next; sharedFences.insert(ticket); return {0,ticket,0};
            }
            Result captureCompositorResource(std::uint32_t resource,std::uint32_t sync,CompositorCaptureDesc desc) override {
                require(resourceTickets.count(resource) && sharedFences.count(sync) && desc.syncTicket==sync && validCompositorCapture(desc));
                ++captures; ++calls;
                return {mode==1?259:mode>=3?-123:0,mode==2?777u:0u,mode==3?1ull:mode==4?0ull:12345ull};
            }
            Result queueCompositorFrame(std::uint32_t resource,std::uint32_t sync,CompositorCaptureDesc desc) override {
                require(resourceTickets.count(resource) && sharedFences.count(sync) && desc.syncTicket==sync && validCompositorFrame(desc));
                ++calls;
                if (mode==6 && !retried) { retried=true; return {static_cast<std::int32_t>(0xc000022du),0,0}; }
                if (mode==1) return {259,0,0};
                if (mode==2) return {0,777,frames+1};
                if (mode==3) return {-123,0,1};
                if (mode==4) return {-123,0,0};
                if (mode==5) return {0,0,frames+2};
                return {0,0,++frames};
            }
        } f;
        f.mode=replyMode;
        Session capture(f);
        const CompositorCaptureDesc initial{2,130,73,0x34324241u,520,0,1};
        require(header(capture.dispatch(request(operation,1,initial))).status==-71);
        capture.dispatch(hello());
        require(!header(capture.dispatch(request(Op::BeginOpenGl,0,4u))).status);
        require(!header(capture.dispatch(request(Op::BeginResourceSharing))).status);
        require(!header(capture.dispatch(request(Op::BeginSyncSharing))).status);
        const auto a=header(capture.dispatch(request(Op::OpenAdapter))).handle;
        const auto first=header(capture.dispatch(request(Op::CreateDevice,a))).handle;
        const auto b=header(capture.dispatch(request(Op::OpenAdapter))).handle;
        const auto second=header(capture.dispatch(request(Op::CreateDevice,b))).handle;
        const auto foreign=header(capture.dispatch(request(Op::OpenAdapter))).handle;
        const auto foreignDevice=header(capture.dispatch(request(Op::CreateDevice,foreign))).handle;
        NativeSharedVendorAllocationDesc allocation{{{4,0x78100000,0,4,0,0},4,0},{130,73,28,1}};
        auto packet=request(Op::CreateNativeSharedVendorResourceAllocation,first,allocation);
        packet.insert(packet.end(),{37,38,39,40,5,6,7,8});
        const auto created=capture.dispatch(packet); VendorResourceReply resource{};
        std::memcpy(&resource,created.data()+sizeof(Header)+sizeof(Reply),sizeof resource);
        const auto ticket=header(capture.dispatch(request(Op::ExportResource,resource.resource))).handle; require(ticket);
        const auto sourceSync=header(capture.dispatch(request(Op::CreateSync,second,SyncDesc{5,NtSharedGpuSyncFlags,0,0,0}))).handle;
        const auto sync=header(capture.dispatch(request(Op::ExportSync,sourceSync))).handle; require(sync);
        const auto foreignSync=header(capture.dispatch(request(Op::CreateSync,foreignDevice,SyncDesc{5,NtSharedGpuSyncFlags,0,0,0}))).handle;
        const auto foreignTicket=header(capture.dispatch(request(Op::ExportSync,foreignSync))).handle; require(foreignTicket);
        const CompositorCaptureDesc valid{sync,130,73,0x34324241u,520,0,1};
        const auto before=f.calls;
        for (unsigned field=0;field<9;++field) {
            auto bad=valid;
            if(field==0) bad.syncTicket=0;
            if(field==1) bad.width=0;
            if(field==2) bad.height=4097;
            if(field==3) bad.width=4097;
            if(field==4) bad.stride=524;
            if(field==5) bad.format=0;
            if(field==6) bad.flags=queued?4u:1u;
            if(field==7) bad.fence=0;
            if(field==8) bad.fence=UINT64_MAX;
            require(header(capture.dispatch(request(operation,ticket,bad))).status==-22);
        }
        for (const auto wrong:{resource.resource,first,UINT32_MAX})
            require(header(capture.dispatch(request(operation,wrong,valid))).status==-9);
        for (const auto wrong:{sourceSync,ticket,foreignTicket,UINT32_MAX}) {
            auto bad=valid; bad.syncTicket=wrong;
            require(header(capture.dispatch(request(operation,ticket,bad))).status==-9);
        }
        f.equivalent=false;
        require(header(capture.dispatch(request(operation,ticket,valid))).status==-9);
        f.equivalent=true; f.consumeSharedEnabled=false;
        require(header(capture.dispatch(request(operation,ticket,valid))).status==-95);
        f.consumeSharedEnabled=true;
        packet=request(operation,ticket,valid); packet.push_back(0);
        require(header(capture.dispatch(packet)).status==-22 && f.calls==before && f.captures==0);
        // The scoped exported fence remains usable after its original object is destroyed.
        require(!header(capture.dispatch(request(Op::DestroySync,sourceSync))).status);
        if (queued) {
            auto audit=valid; audit.flags=CompositorFrameAudit;
            if (replyMode==0 || replyMode==6) {
                if (replyMode==6) {
                    const auto out=capture.dispatch(request(operation,ticket,audit)); Reply result{};
                    std::memcpy(&result,out.data()+sizeof(Header),sizeof result);
                    require(!header(out).status && result.ntstatus==static_cast<std::int32_t>(0xc000022du) && !result.value);
                }
                for (unsigned frame=1;frame<=12;++frame) {
                    const auto out=capture.dispatch(request(operation,ticket,frame==1 || frame==12 ? audit : valid)); Reply result{};
                    std::memcpy(&result,out.data()+sizeof(Header),sizeof result);
                    require(!header(out).status && header(out).handle==ticket && !result.ntstatus && result.value==frame);
                }
                require(header(capture.dispatch(request(operation,ticket,audit))).status==-24);
                require(!header(capture.dispatch(request(operation,ticket,valid))).status && f.frames==13);
                require(header(capture.dispatch(request(Op::CaptureCompositorResource,ticket,valid))).status==-16);
                auto final=valid;final.flags=CompositorFrameFinal;
                const auto ended=capture.dispatch(request(operation,ticket,final));Reply endReply{};
                std::memcpy(&endReply,ended.data()+sizeof(Header),sizeof endReply);
                require(!header(ended).status && !endReply.ntstatus && endReply.value==14 && f.frames==14);
                const auto endCalls=f.calls;
                require(header(capture.dispatch(request(operation,ticket,valid))).status==-108 && f.calls==endCalls);
                require(header(capture.dispatch(request(operation,ticket,final))).status==-108 && f.calls==endCalls);
                require(!header(capture.dispatch(request(Op::CloseSharedResource,ticket))).status);
                const auto calls=f.calls;
                require(header(capture.dispatch(request(operation,ticket,valid))).status==-108 && f.calls==calls);
                std::cout<<"COMPOSITOR_STREAM_END finalFrame=14 acknowledged=true lateFramesRejected=true gpu=false\n";
            } else {
                for (unsigned attempt=0;attempt<2;++attempt) {
                    const auto out=capture.dispatch(request(operation,ticket,audit));
                    if (replyMode==4) {
                        Reply result{}; std::memcpy(&result,out.data()+sizeof(Header),sizeof result);
                        require(!header(out).status && result.ntstatus==-123 && !result.value);
                    } else require(header(out).status==-5);
                }
                require(header(capture.dispatch(request(operation,ticket,audit))).status==-24);
            }
            std::cout<<"COMPOSITOR_QUEUE_WIRE ownedTickets=true sameGpu=true repeatedFrames=13 auditLimit=2 retryPreservesAuditBudget=true malformedRepliesRejected=true gpu=false\n";
            continue;
        }
        for(unsigned round=0;round<2;++round) {
            const auto out=capture.dispatch(request(operation,ticket,valid)); Reply result{};
            if (replyMode>=1 && replyMode<=3) require(header(out).status==-5);
            else {
                std::memcpy(&result,out.data()+sizeof(Header),sizeof result);
                require(!header(out).status && header(out).handle==ticket &&
                    result.ntstatus==(replyMode==4?-123:0) && result.value==(replyMode==4?0ull:12345ull));
            }
        }
        require(header(capture.dispatch(request(operation,ticket,valid))).status==-24 && f.captures==2);
        std::cout<<"COMPOSITOR_CAPTURE_WIRE ticketOwnership=true samePhysicalGpu=true foreignGpuRejected=true retiredSourceFenceReference=true boundedAttempts=2 pixels=false\n";
    }
    Fake malformed; Session s(malformed); s.dispatch(hello());
    auto p = request(Op::OpenAdapter); p.push_back(0);
    require(header(s.dispatch(p)).status == -22);
    p = request(Op::OpenAdapter); p[8] = 1; require(header(s.dispatch(p)).status == -22);
    p = request(Op::OpenAdapter); p[12] = 1; require(header(s.dispatch(p)).status == -22);
    for (std::uint32_t op = 1; op <= 8; ++op) {
        p = request(static_cast<Op>(op)); require(header(s.dispatch(p)).status == -95);
    }
    require(s.dispatch({1, 2, 3}).empty() && malformed.calls == 0);
    require(header(s.dispatch(hello(2))).status == -93);
    require(header(s.dispatch(request(Op::OpenAdapter))).status == -71);
    std::cout << "PASS: WDDM wire ownership, allocation bounds/budget, chunked queries, NTSTATUS, cleanup, ABI rejection\n";
}
