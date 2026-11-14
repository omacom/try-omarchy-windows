// SPDX-License-Identifier: MIT
// Documented WDDM operations with explicit wire structures and owned handles.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <set>
#include <array>
#include "driver_wire.h"
#include "driver_coordinator.h"
#include "bounded_gpu_retirement_wait.h"
#include "checked_fence_completion.h"
#include "shared_resource_probe.h"
#include "shared_sync_handle.h"
#include "shared_resource_handle.h"
#include "shared_texture_consumer.h"
#include "native_texture_metadata.h"
#include "native_buffer_metadata.h"
#include "qemu_runtime_host.h"
#include "launcher_lifetime.h"
#include "launcher_session_stream.h"
#include "shared_texture_worker.h"
#include "compositor_readback.h"
#include "compositor_presenter.h"
#include "atomic_line_log.h"
#include "framed_write.h"
#include "aggregate_gpu_budget.h"
#include "deferred_residency.h"
#include "mapped_residency_admission.h"
using Microsoft::WRL::ComPtr;
using namespace driver_bridge;
class KmtDriver : public Driver {
    LUID luid{};
    DXGI_ADAPTER_DESC1 description{};
    struct Paging {
        volatile std::uint64_t* fence;
        std::uint32_t sync;
        std::optional<driver_qemu::FenceLease> lease;
        std::uint32_t device;
    };
    std::map<std::uint32_t, Paging> pagingFences;
    struct Context { std::uint32_t device, flags; };
    std::map<std::uint32_t, Context> contextOwners;
    struct HwQueue {
        std::uint32_t context, device, sync;
        volatile std::uint64_t* fence;
        std::uint64_t gpuAddress;
        std::optional<driver_qemu::FenceLease> lease;
        std::uint64_t submittedFence = 0;
    };
    std::map<std::uint32_t, HwQueue> hwQueues;
    bool submitEnabled = false;
    bool asyncSubmitEnabled = false;
    bool openglEnabled = false;
    std::uint32_t openGlProfileVersion=0;
    std::size_t peakSharedResourceHandles=0,peakSharedSyncHandles=0;
    std::size_t sharedResourceLimit() const { return coordinatedObjectsEnabled && coordinatedObjectCapacity.version>=5 ? MaxCoordinatedSharedReferences : MaxSharedResourceHandles; }
    std::size_t sharedSyncLimit() const { return coordinatedObjectsEnabled && coordinatedObjectCapacity.version>=5 ? MaxCoordinatedSharedReferences : MaxSharedSyncHandles; }
    bool openglFencesEnabled = false;
    bool openglCompositorEnabled = false;
    bool openglRenderEnabled = false;
    bool openglDesktopEnabled = false;
    bool openglDesktop4kEnabled = false;
    bool openglDesktop4kCpuEnabled = false;
    bool openglDesktopSparseEnabled = false;
    bool openglDesktopLargeMapEnabled = false, openglDesktopPresentationEnabled = false;
    bool coordinatedObjectsEnabled=false;
    CoordinatedObjectCapacity coordinatedObjectCapacity{};
    std::size_t peakHwQueues=0,peakSyncObjects=0;
    std::uint64_t hwQueueQuotaRejections=0,syncQuotaRejections=0;
    std::size_t hwQueueLimit() const { return coordinatedObjectsEnabled ? coordinatedObjectCapacity.hwQueues : openglFencesEnabled ? OpenGlFenceHwQueues : MaxHwQueues; }
    unsigned compositorCaptureAttempts = 0, completedCompositorCaptures = 0, failedCompositorCaptures = 0;
    std::unique_ptr<driver_shared::CompositorReadback> compositorPresenter;
    std::unique_ptr<driver_shared::CompositorPresenter> compositorOutput;
    std::uint64_t completedCompositorFrames = 0, failedCompositorFrames = 0, compositorBackpressure = 0;
    std::size_t reservationLimit() const { return coordinatedObjectsEnabled ? CoordinatedObjectCapacity{}.reservations : openglDesktopEnabled ? GuestOpenGlDesktopProfile.reservationLimit : openglRenderEnabled ? GuestOpenGlCompositorRenderProfile.reservationLimit : openglEnabled ? GuestOpenGlProfile.reservationLimit : MaxGpuReservations; }
    std::uint64_t reservationByteLimit() const { return coordinatedObjectsEnabled ? CoordinatedObjectCapacity{}.reservedGpuBytes : MaxGpuReservedBytes; }
    std::uint64_t reservationQuotaRejections=0;
    std::size_t peakReservations=0;
    std::size_t syncObjectLimit() const { return coordinatedObjectsEnabled ? coordinatedObjectCapacity.syncObjects : openglCompositorEnabled ? OpenGlCompositorSyncObjects : MaxSyncObjects; }
    std::size_t vendorObjectLimit() const { return coordinatedObjectsEnabled && coordinatedObjectCapacity.version>=4 ? coordinatedObjectCapacity.vendorAllocations : openglDesktopEnabled ? OpenGlDesktopVendorAllocations : openglCompositorEnabled ? OpenGlCompositorVendorAllocations : openglFencesEnabled ? OpenGlFenceVendorAllocations : vendorAllocationObjectLimit(capabilities().flags); }
    bool vendorObjectAdmission() const {
        const bool allowed=vendorAllocations.size()<vendorObjectLimit();
        if(!allowed) std::cerr<<"{\"nativeVendorObjectAdmissionRejected\":true,\"live\":"<<vendorAllocations.size()<<",\"limit\":"<<vendorObjectLimit()<<"}\n";
        return allowed;
    }
    std::uint32_t openGlCommandLimit() const { return openglDesktopEnabled ? 0 : openglCompositorEnabled ? GuestOpenGlCompositorProfile.commandLimit : GuestOpenGlProfile.commandLimit; }
    std::uint32_t hwSignalLimit() const { return openglDesktopEnabled ? 0 : openglCompositorEnabled ? OpenGlCompositorHwSignals : openglFencesEnabled ? OpenGlFenceHwSignals : MaxHwQueueSignals; }
    struct PendingSubmission { std::uint64_t index; std::uint32_t queue, bytes, privateBytes; std::uint64_t target; };
    std::vector<PendingSubmission> pendingSubmissions;
    std::uint64_t acceptedAsyncSubmissions = 0; unsigned peakPendingSubmissions = 0;
    struct PendingHwSignal {
        std::uint64_t index; std::uint32_t sync, flags, count; std::uint64_t target;
        std::array<std::uint32_t,MaxHwQueues> queues;
    };
    std::vector<PendingHwSignal> pendingHwSignals;
    struct PendingHwWait {
        std::uint64_t index; std::uint32_t queue, count;
        bool contextStream;
        std::array<std::uint32_t, MaxHwQueueWaitObjects> syncs;
        std::array<std::uint64_t, MaxHwQueueWaitObjects> targets;
    };
    std::vector<PendingHwWait> pendingHwWaits;
    struct PendingContextSignal { std::uint64_t index; std::uint32_t context, sync; std::uint64_t target; };
    std::vector<PendingContextSignal> pendingContextSignals;
    std::uint64_t acceptedContextSignals = 0, retiredAsyncContextSignals = 0; unsigned peakPendingContextSignals = 0;
    std::uint64_t acceptedHwQueueWaits = 0, satisfiedHwQueueWaits = 0, failedHwQueueWaits = 0; unsigned peakPendingHwQueueWaits = 0;
    std::uint64_t hwQueueSignalAttempts = 0, acceptedHwQueueSignals = 0, completedHwQueueSignals = 0, failedHwQueueSignals = 0;
    unsigned peakPendingHwQueueSignals = 0;
    bool asyncRetirementFailed = false;
    std::uint64_t submissionAttempts = 0, completedSubmissions = 0, failedSubmissions = 0;
    std::uint64_t submissionTimeouts = 0;
    std::uint64_t submittedCommandBytes = 0, acceptedCommandBytes = 0;
    bool waitFence(std::uint32_t handle,std::uint32_t& device,volatile std::uint64_t*& word) const {
        const auto sync=syncObjects.find(handle);
        if(sync!=syncObjects.end() && sync->second.type==5 && sync->second.lease && sync->second.fence) {
            device=sync->second.device;word=sync->second.fence;return true;
        }
        if(coordinatedObjectsEnabled) for(const auto& entry:pagingFences) {
            if(entry.second.sync==handle && entry.second.lease && entry.second.fence) {
                device=entry.second.device;word=entry.second.fence;return true;
            }
        }
        if(coordinatedObjectsEnabled) for(const auto& entry:hwQueues) {
            if(entry.second.sync==handle && entry.second.lease && entry.second.fence) {
                device=entry.second.device;word=entry.second.fence;return true;
            }
        }
        return false;
    }
    bool collectRetiredSubmissions() {
        if (asyncRetirementFailed) return false;
        for (auto it = pendingSubmissions.begin(); it != pendingSubmissions.end();) {
            const auto queue = hwQueues.find(it->queue);
            if (queue == hwQueues.end() || !queue->second.fence) {
                asyncRetirementFailed = true; ++failedSubmissions;
                std::cerr << "{\"nativeAsyncRetirementFailed\":true,\"reason\":\"owned queue absent\"}\n";
                return false;
            }
            MemoryBarrier(); const auto observed = *queue->second.fence;
            if (observed == UINT64_MAX) {
                asyncRetirementFailed = true; ++failedSubmissions;
                std::cerr << "{\"nativeAsyncRetirementFailed\":true,\"reason\":\"device loss\"}\n";
                return false;
            }
            if (observed < it->target) { ++it; continue; }
            ++completedSubmissions; submittedCommandBytes += it->bytes;
            std::cerr << "{\"nativeAsyncSubmissionRetired\":true,\"index\":" << it->index
                      << ",\"bytes\":" << it->bytes << ",\"privateBytes\":" << it->privateBytes
                      << ",\"target\":" << it->target << ",\"observed\":" << observed << "}\n";
            it = pendingSubmissions.erase(it);
        }
        for (auto it = pendingHwSignals.begin(); it != pendingHwSignals.end();) {
            const auto sync = syncObjects.find(it->sync);
            if (sync == syncObjects.end() || !sync->second.fence) {
                asyncRetirementFailed = true; ++failedHwQueueSignals;
                std::cerr << "{\"nativeHwQueueSignalRetirementFailed\":true,\"reason\":\"owned sync absent\"}\n";
                return false;
            }
            MemoryBarrier(); const auto observed = *sync->second.fence;
            if (observed == UINT64_MAX) {
                asyncRetirementFailed = true; ++failedHwQueueSignals;
                std::cerr << "{\"nativeHwQueueSignalRetirementFailed\":true,\"reason\":\"device loss\"}\n";
                return false;
            }
            if (observed < it->target) { ++it; continue; }
            ++completedHwQueueSignals;
            std::cerr << "{\"nativeHwQueueSignalRetired\":true,\"index\":" << it->index << ",\"flags\":" << it->flags
                      << ",\"queues\":" << it->count << ",\"target\":" << it->target << ",\"observed\":" << observed
                      << ",\"noGpuAccess\":true,\"cpuValueWrittenByBridge\":false}\n";
            it = pendingHwSignals.erase(it);
        }
        for (auto it = pendingContextSignals.begin(); it != pendingContextSignals.end();) {
            const auto sync = syncObjects.find(it->sync);
            if (sync == syncObjects.end() || !sync->second.fence) {
                asyncRetirementFailed = true; ++failedContextSignals; return false;
            }
            MemoryBarrier(); const auto observed = *sync->second.fence;
            if (observed == UINT64_MAX) { asyncRetirementFailed = true; ++failedContextSignals; return false; }
            if (observed < it->target) { ++it; continue; }
            ++completedContextSignals; ++retiredAsyncContextSignals;
            std::cerr << "{\"nativeAsyncContextSignalRetired\":true,\"index\":" << it->index
                      << ",\"target\":" << it->target << ",\"observed\":" << observed
                      << ",\"noGpuAccess\":true,\"cpuValueWrittenByBridge\":false}\n";
            it = pendingContextSignals.erase(it);
        }
        for (auto it = pendingHwWaits.begin(); it != pendingHwWaits.end();) {
            bool satisfied = true;
            for (std::uint32_t n = 0; n < it->count; ++n) {
                std::uint32_t device=0;volatile std::uint64_t* word=nullptr;
                if (!waitFence(it->syncs[n],device,word)) {
                    asyncRetirementFailed = true; ++failedHwQueueWaits; return false;
                }
                MemoryBarrier(); const auto observed = *word;
                if (observed == UINT64_MAX) { asyncRetirementFailed = true; ++failedHwQueueWaits; return false; }
                if (observed < it->targets[n]) satisfied = false;
            }
            if (!satisfied) { ++it; continue; }
            ++satisfiedHwQueueWaits;
            std::cerr << "{\"nativeGpuQueueWaitDependenciesSatisfied\":true,\"index\":" << it->index
                      << ",\"contextStream\":" << (it->contextStream ? "true" : "false")
                      << ",\"objects\":" << it->count << ",\"queueRetirementVerified\":false}\n";
            it = pendingHwWaits.erase(it);
        }
        return true;
    }
    bool hwQueuesEnabled = false;
    bool retirementEnabled = false;
    unsigned vendorDestructionsWithHwQueues = 0;
    unsigned vendorCpuSlotQuotaRejections = 0;
    unsigned completedHwQueues = 0, destroyedHwQueues = 0, failedHwQueues = 0, hwQueuesReleasedAfterVmExit = 0;
    unsigned completedNoBroadcastSignalHwQueues = 0;
    unsigned completedNoBroadcastWaitHwQueues = 0;
    struct Synchronization {
        std::uint32_t device, type, flags;
        volatile std::uint64_t* fence;
        std::uint64_t gpuAddress;
        std::optional<driver_qemu::FenceLease> lease;
        std::uint64_t lastSignal = 0;
        bool shared = false;
    };
    std::map<std::uint32_t, Synchronization> syncObjects;
    std::map<std::uint32_t, driver_shared::SyncHandle> sharedSyncHandles;
    std::uint32_t nextSharedSync = 1;
    bool syncSharingEnabled = false;
    unsigned exportedSyncHandles = 0, closedSyncHandles = 0, importedSyncObjects = 0;
    bool syncEnabled = false;
    unsigned completedSyncs = 0, destroyedSyncs = 0, failedSyncs = 0, monitoredSyncs = 0, mutexSyncs = 0;
    unsigned syncsReleasedAfterVmExit = 0;
    unsigned noGpuAccessSyncs = 0, noSignalMaxValueOnTdrSyncs = 0;
    std::uint64_t contextSignalAttempts = 0, completedContextSignals = 0, failedContextSignals = 0, contextSignalTimeouts = 0;
    driver_qemu::Runtime* runtime = nullptr;
    struct Allocation {
        std::uint32_t device, resource, size;
        void* memory;
        std::uint64_t gpuAddress = 0;
        bool resident = false;
        bool pagingFailed = false;
        bool owned = true;
    };
    std::map<std::uint32_t, Allocation> allocations;
    // Vendor video-memory allocations have no host CPU backing or public
    // size field. Never mix these with the byte-counted standard allocations.
    struct VendorAllocation {
        std::uint32_t device;
        std::uint32_t resource = 0;
        bool openedSharedResource = false;
        std::uint64_t address = 0, pages = 0;
        bool reservationBackedMapping = false;
        std::uint32_t residencyAttempts = 0;
        std::uint64_t offsetPages = 0;
        bool locked = false;
        bool resident = false;
        void* cpuData = nullptr;
        driver_residency::References residency;
        std::uint32_t cpuBytes = 0;
        std::uint64_t initialCpuFingerprint = 0;
        std::uint64_t firstWord = 0, lastWord = 0;
        std::optional<driver_qemu::AllocationLease> cpuLease;
    };
    std::map<std::uint32_t, VendorAllocation> vendorAllocations;
    std::map<std::uint32_t, std::vector<std::uint8_t>> resourceGuestRuntime;
    struct SharedResource { driver_shared::ResourceHandle handle; std::vector<std::uint8_t> guestRuntime; };
    std::map<std::uint32_t, SharedResource> sharedResourceHandles;
    std::uint32_t nextSharedResource = 1;
    bool resourceSharingEnabled = false;
    unsigned exportedResourceHandles = 0, closedResourceHandles = 0, importedResourceFdObjects = 0;
    unsigned completedVendorResources = 0, destroyedVendorResources = 0;
    unsigned sharedResourceProbes = 0, importedSharedResources = 0, failedSharedResourceProbes = 0;
    unsigned sharedBufferProbes = 0, importedSharedBuffers = 0, failedSharedBufferProbes = 0;
    bool probeSharedResource(std::uint32_t resource, std::uint32_t runtimeBytes, const NativeGpuBufferDesc* expectedBuffer = nullptr) {
        const auto result = driver_shared::probe(resource, luid);
        const auto& d = result.description;
        if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            ++sharedBufferProbes;
            const bool success = result.shareStatus == 0 && result.deviceResult == S_OK && result.importResult == S_OK &&
                result.handleClosed && result.importedReleased && driver_shared::validBufferDescription(d) &&
                (!expectedBuffer || (d.Width == expectedBuffer->bytes && static_cast<std::uint32_t>(d.Flags) == expectedBuffer->flags));
            if (success) ++importedSharedBuffers; else ++failedSharedBufferProbes;
            if (result.shareStatus == 0 && !result.handleClosed) ++cleanupFailures;
            std::cerr << "{\"nativeSharedBufferImportProbe\":true,\"verified\":" << (success ? "true" : "false")
                      << ",\"ntstatus\":" << result.shareStatus << ",\"deviceHresult\":" << result.deviceResult
                      << ",\"importHresult\":" << result.importResult << ",\"runtimeBytes\":" << runtimeBytes
                      << ",\"dimension\":" << d.Dimension << ",\"alignment\":" << d.Alignment << ",\"width\":" << d.Width
                      << ",\"height\":" << d.Height << ",\"layers\":" << d.DepthOrArraySize << ",\"levels\":" << d.MipLevels
                      << ",\"format\":" << d.Format << ",\"samples\":" << d.SampleDesc.Count << ",\"quality\":" << d.SampleDesc.Quality
                      << ",\"layout\":" << d.Layout << ",\"flags\":" << d.Flags
                      << ",\"sharedNtHandleClosed\":" << (result.handleClosed ? "true" : "false")
                      << ",\"importedResourceReleased\":" << (result.importedReleased ? "true" : "false")
                      << ",\"gpuCopy\":false,\"presented\":false}\n";
            return success;
        }
        ++sharedResourceProbes;
        const bool success = result.shareStatus == 0 && result.deviceResult == S_OK && result.importResult == S_OK &&
            result.handleClosed && result.importedReleased && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            d.Width && d.Width <= 4096 && d.Height && d.Height <= 4096 && d.DepthOrArraySize == 1 && d.MipLevels == 1 &&
            (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
             d.Format == DXGI_FORMAT_B8G8R8X8_UNORM) && d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0 &&
            d.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (success) ++importedSharedResources; else ++failedSharedResourceProbes;
        if (result.shareStatus == 0 && !result.handleClosed) ++cleanupFailures;
        std::cerr << "{\"nativeSharedResourceImportProbe\":true,\"verified\":" << (success ? "true" : "false")
                  << ",\"ntstatus\":" << result.shareStatus << ",\"deviceHresult\":" << result.deviceResult
                  << ",\"importHresult\":" << result.importResult << ",\"runtimeBytes\":" << runtimeBytes
                  << ",\"heapHresult\":" << result.heapResult << ",\"heapBytes\":" << result.heapBytes
                  << ",\"width\":" << d.Width << ",\"height\":" << d.Height << ",\"format\":" << d.Format
                  << ",\"dimension\":" << d.Dimension << ",\"flags\":" << d.Flags
                  << ",\"sharedNtHandleClosed\":" << (result.handleClosed ? "true" : "false")
                  << ",\"importedResourceReleased\":" << (result.importedReleased ? "true" : "false")
                  << ",\"gpuCopy\":false,\"presented\":false}\n";
        return false;
    }
    unsigned completedVendorDriverProtectionMaps = 0;
    ComPtr<IDXGIAdapter3> allocationBudgetAdapter;
    bool consumeSharedEnabled = false;
    bool consumeSharedProcessEnabled = false;
    driver_worker::Counters nativeWorkers;
    bool presentSharedEnabled = false;
    bool nativeMetadataEnabled = false;
    bool streamingEnabled = false;
    std::set<std::uint32_t> sharedResourceIds;
    struct ConsumerLease {
        std::unique_ptr<driver_shared::TextureConsumer> gpu;
        std::unique_ptr<driver_worker::TextureWorker> worker;
        std::uint32_t sync; std::uint64_t lastFence = 0;
    };
    std::map<std::uint32_t, ConsumerLease> textureConsumers;
    unsigned completedSharedConsumes = 0, failedSharedConsumes = 0, openedTextureConsumers = 0, releasedTextureConsumers = 0;
    bool sharedResourcesEnabled = false;
    bool allocationsEnabled = false;
    bool gpuVaEnabled = false;
    bool residencyEnabled = false;
    bool deferredResidencyEnabled = false;
    bool mappedResidencyAdmissionEnabled=false;
    bool applicationMemoryPermitted=false,applicationMemoryEnabled=false;
    bool applicationObjectsPermitted=false;
    std::uint64_t retainedResidencyReferences=0,peakRetainedResidencyReferences=0;
    std::uint64_t deferredEvictionRequests=0,reusedResidencyReferences=0,reclaimedResidencyReferences=0;
    std::uint64_t destroyedRetainedResidencyReferences=0,gpuVaQuotaRejections=0;
    bool cpuEnabled = false;
    std::uint32_t vendorCpuBytes = 0, peakVendorCpuBytes = 0;
    unsigned vendorCpuByteQuotaRejections=0,gpuStateRetirementWaits=0,gpuStateRetirementTimeouts=0;
    unsigned ntReferencesClosedDuringGpuWork=0;
    unsigned completedVendorLocks = 0, completedVendorUnlocks = 0, failedVendorLocks = 0, failedVendorUnlocks = 0;
    unsigned completedVendorSubrangeLocks = 0;
    unsigned vendorCpuContentsChanged = 0;
    bool cpuStoreTest = false;
    unsigned completedCpuStoreTests = 0, failedCpuStoreTests = 0;
    unsigned cpuLocksReleasedAfterVmExit = 0;
    std::uint64_t cpuAllocationFingerprintCalls=0,cpuAllocationFingerprintBytes=0;
    std::uint64_t fingerprint(void* memory, std::uint32_t bytes) {
        if(!cpuStoreTest) throw std::runtime_error("CPU allocation fingerprint requires the explicit store diagnostic");
        ++cpuAllocationFingerprintCalls;cpuAllocationFingerprintBytes+=bytes;
        const auto data = static_cast<volatile const unsigned char*>(memory);
        std::uint64_t hash = 14695981039346656037ull;
        for (std::uint32_t n = 0; n < bytes; ++n) hash = (hash ^ data[n]) * 1099511628211ull;
        return hash;
    }
    std::uint32_t vendorResidencyAttempts = 0, peakVendorResidencyAttempts = 0;
    unsigned completedVendorResidency = 0, failedVendorResidency = 0, completedVendorResidencyWaits = 0;
    unsigned completedMemoryBudgetQueries = 0, failedMemoryBudgetQueries = 0;
    unsigned attemptedVendorEvictions = 0, completedVendorEvictions = 0, failedVendorEvictions = 0;
    std::uint64_t vendorAllocationsMadeResident = 0;
    std::uint32_t vendorMappedPages = 0, peakVendorMappedPages = 0;
    std::uint32_t peakStandaloneMappedPages=0;
    unsigned completedVendorMaps = 0, failedVendorMaps = 0, completedVendorMapWaits = 0;
    unsigned completedVendorAllocations = 0, destroyedVendorAllocations = 0, failedVendorAllocations = 0, uninitializedSourceAllocations = 0;
    unsigned nativeGpuBufferMetadataMemoryRefusals = 0;
    unsigned nativeGpuBufferAdmissionQueries = 0, nativeGpuBufferAdmissionRefusals = 0, failedNativeGpuBufferAdmissionQueries = 0;
    std::size_t peakVendorAllocationObjects = 0;
    std::uint64_t peakReportedGpuUsage = 0;
    std::mutex reportedUsageGuard;
    std::uint64_t reportedGpuUsageLimit() const {
        if(applicationMemoryEnabled) return GuestApplicationMemoryProfile.reportedBytes;
        return openglDesktopPresentationEnabled ? OpenGlDesktopPresentationReportedBytes : openglDesktop4kEnabled ? OpenGlDesktop4kReportedGpuBytes : gpu_diagnostic::ReportedGpuBytes;
    }
    std::uint32_t allocatedBytes = 0;
    unsigned activeAdapters = 0, activeDevices = 0, cleanupFailures = 0;
    std::uint64_t completedDeviceExecutionQueries=0,failedDeviceExecutionQueries=0;
    std::map<std::uint32_t,std::uint32_t> deviceExecutionStates;
    std::map<std::uint32_t, std::uint32_t> deviceAdapters;
    std::set<std::uint32_t> ownedAdapters;
    struct GpuReservation { std::uint32_t adapter; std::uint64_t address, bytes; };
    std::map<std::uint32_t, GpuReservation> gpuReservations;
    std::uint32_t nextGpuReservation = 0;
    std::uint64_t gpuReservedBytes = 0, peakGpuReservedBytes = 0;
    bool reservationEnabled = false;
    bool gpuStateEnabled = false;
    GpuStateRanges gpuStates;
    unsigned completedGpuStateMaps = 0, failedGpuStateMaps = 0, completedGpuStateWaits = 0;
    unsigned gpuStateReplacedAllocationMappings = 0;
    unsigned completedReservations = 0, freedReservations = 0, failedReservations = 0;
    unsigned reservationsReleasedAfterVmExit = 0;
    bool translationEnabled = false;
    unsigned completedTranslations = 0, failedTranslations = 0;
    std::map<std::uint32_t, std::uint32_t> translatedAllocations;
    unsigned activeContexts = 0;
    unsigned completedContextPriorities = 0, failedContextPriorities = 0;
    bool contextsEnabled = false;
    bool queriesEnabled = false;
    unsigned completedQueries = 0, failedQueries = 0;
    std::string guestSectionName;
    std::uint32_t guestBytes = 0;
    HANDLE guestSection = nullptr;
    void* guestMemory = nullptr;
    ComPtr<ID3D12Device3> copyDevice;
    ComPtr<ID3D12Heap> copyHeap;
    ComPtr<ID3D12CommandQueue> copyQueue;
    ComPtr<ID3D12Fence> copyFence;
    HANDLE copyEvent = nullptr;
    std::uint64_t copyFenceValue = 0, completedCopies = 0, gpuCopiedBytes = 0;
    HRESULT lastCopyError = S_OK;
    static constexpr auto Invalid = static_cast<std::int32_t>(0xc000000du);
    static constexpr auto PagingTimeout = static_cast<std::int32_t>(0xc00000b5u);
    bool reportedGpuUsageAcceptable(std::uint64_t additionalBytes=0,const char* presentationStage=nullptr,std::uint64_t* observedUsage=nullptr,bool* observedUsageKnown=nullptr) {
        std::lock_guard<std::mutex> lock(reportedUsageGuard);
        // This is a reported-usage guard, not an allocation-size decoder or
        // a pre-allocation hard quota. Nonresident allocations and transient
        // growth can escape CurrentUsage; the endpoint stays diagnostic-only.
        if(observedUsageKnown) *observedUsageKnown=false;
        if (!allocationBudgetAdapter) return false;
        std::uint64_t total = 0;
        std::array<std::uint64_t,2> segmentUsage{},segmentBudget{};std::size_t segmentIndex=0;
        for (const auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (FAILED(allocationBudgetAdapter->QueryVideoMemoryInfo(0, group, &info)) ||
                info.CurrentUsage > UINT64_MAX - total) return false;
            segmentUsage[segmentIndex]=info.CurrentUsage;segmentBudget[segmentIndex]=info.Budget;++segmentIndex;
            total += info.CurrentUsage;
        }
        peakReportedGpuUsage = (std::max)(peakReportedGpuUsage, total);
        if(observedUsage) *observedUsage=total;
        if(observedUsageKnown) *observedUsageKnown=true;
        const bool fits=reportedGpuBudgetFits(total,additionalBytes,reportedGpuUsageLimit());
        if(!fits) std::cerr<<"{\"nativeReportedGpuUsageRejected\":true,\"usageBytes\":"<<total
            <<",\"additionalBytes\":"<<additionalBytes<<",\"limitBytes\":"<<reportedGpuUsageLimit()
            <<",\"localUsageBytes\":"<<segmentUsage[0]<<",\"nonlocalUsageBytes\":"<<segmentUsage[1]
            <<",\"localNativeBudgetBytes\":"<<segmentBudget[0]<<",\"nonlocalNativeBudgetBytes\":"<<segmentBudget[1]
            <<",\"currentProcessOnly\":true,\"policyChanged\":false,\"hardAllocationQuota\":false}\n";
        if(presentationStage) std::cerr<<"{\"nativePresentationMemoryGuard\":true,\"stage\":\""<<presentationStage
            <<"\",\"usageBytes\":"<<total<<",\"additionalBytes\":"<<additionalBytes
            <<",\"limitBytes\":"<<reportedGpuUsageLimit()<<",\"accepted\":"<<(fits?"true":"false")<<"}\n";
        return fits;
    }
    Result copyFailure(HRESULT error) {
        lastCopyError = error; return {static_cast<std::int32_t>(0xc0000001u), 0, 0};
    }
    bool waitPaging(std::uint32_t queue, std::uint64_t target) const {
        const auto fence = pagingFences.find(queue);
        if (fence == pagingFences.end() || !fence->second.fence) return false;
        const auto deadline = GetTickCount64() + 5000;
        while (*fence->second.fence < target && GetTickCount64() < deadline) Sleep(1);
        MemoryBarrier(); return *fence->second.fence >= target;
    }
    static bool rangeValid(const Allocation& a, Range range) {
        return range.size && range.size <= MaxChunk && range.size <= a.size && range.offset <= a.size - range.size;
    }
    Result createBackedAllocation(std::uint32_t device, std::uint32_t size, void* memory, bool owned) {
        D3DKMT_CREATESTANDARDALLOCATION standard{};
        standard.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP; standard.ExistingHeapData.Size = size;
        D3DDDI_ALLOCATIONINFO2 info{}; info.pSystemMem = memory;
        D3DKMT_CREATEALLOCATION a{}; a.hDevice = device; a.pStandardAllocation = &standard;
        a.NumAllocations = 1; a.pAllocationInfo2 = &info;
        a.Flags.StandardAllocation = 1; a.Flags.ExistingSysMem = 1;
        a.Flags.CreateShared = 1; a.Flags.CreateResource = 1; a.Flags.CrossAdapter = 1; a.Flags.NtSecuritySharing = 1;
        const auto status = D3DKMTCreateAllocation2(&a);
        if (status < 0) {
            if (owned && !VirtualFree(memory, 0, MEM_RELEASE)) ++cleanupFailures;
            return {status, 0, 0};
        }
        allocations.emplace(info.hAllocation, Allocation{device, a.hResource, size, memory, 0, false, false, owned});
        allocatedBytes += size; return {status, info.hAllocation, size};
    }
public:
    KmtDriver(std::string sectionName, std::uint32_t sectionBytes, bool enableContexts, bool enableQueries,
              driver_qemu::Runtime* ownedRuntime = nullptr, bool enableAllocations = false, bool enableGpuVa = false, bool enableResidency = false, bool enableCpu = false, bool testCpuStores = false, bool enableTranslation = false, bool enableHwQueues = false, bool enableSync = false, bool enableSubmit = false, bool enableRetirement = false, bool enableReservation = false, bool enableGpuState = false, bool enableAsyncSubmit = false, bool enableSharedResources = false, bool enableConsumeShared = false, bool enablePresentShared = false, bool enableNativeMetadata = false, bool enableConsumeProcess = false)
        : runtime(ownedRuntime), allocationsEnabled(enableAllocations), gpuVaEnabled(enableGpuVa), residencyEnabled(enableResidency), cpuEnabled(enableCpu), cpuStoreTest(testCpuStores), contextsEnabled(enableContexts), queriesEnabled(enableQueries),
          guestSectionName(std::move(sectionName)), guestBytes(sectionBytes) {
        translationEnabled = enableTranslation;
        hwQueuesEnabled = enableHwQueues;
        syncEnabled = enableSync;
        submitEnabled = enableSubmit;
        asyncSubmitEnabled = enableAsyncSubmit;
        sharedResourcesEnabled = enableSharedResources;
        consumeSharedEnabled = enableConsumeShared;
        consumeSharedProcessEnabled = enableConsumeProcess;
        presentSharedEnabled = enablePresentShared;
        nativeMetadataEnabled = enableNativeMetadata;
        pendingSubmissions.reserve(MaxHwSubmissions);
        pendingHwSignals.reserve(MaxHwQueueSignals);
        pendingHwWaits.reserve(MaxPendingHwQueueWaits);
        pendingContextSignals.reserve(MaxContextSignals);
        retirementEnabled = enableRetirement;
        reservationEnabled = enableReservation;
        gpuStateEnabled = enableGpuState;
        const std::string prefix = "Local\\7Wdev-WDDM-";
        if (!guestSectionName.empty()) {
            if (guestSectionName.size() != prefix.size() + 32 || guestSectionName.compare(0, prefix.size(), prefix) ||
                guestSectionName.find_first_not_of("0123456789abcdef", prefix.size()) != std::string::npos ||
                !guestBytes || guestBytes > 1024u * 1024u * 1024u || guestBytes % 4096)
                throw std::runtime_error("Invalid configured guest RAM section");
        } else if (guestBytes) throw std::runtime_error("Missing guest RAM section name");
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("CreateDXGIFactory1 failed");
        bool found = false;
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const auto result = factory->EnumAdapters1(i, &adapter);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result) || FAILED(adapter->GetDesc1(&description))) throw std::runtime_error("Adapter enumeration failed");
            if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                luid = description.AdapterLuid; found = true;
                if (allocationsEnabled && FAILED(adapter.As(&allocationBudgetAdapter)))
                    throw std::runtime_error("NVIDIA video-memory accounting unavailable");
                if (!guestSectionName.empty()) {
                    // Device3 provides the documented section-backed heap
                    // import. Refuse to advertise GPU copy without this API.
                    const auto hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&copyDevice));
                    if (FAILED(hr)) throw std::runtime_error("NVIDIA D3D12 Device3 unavailable for section import");
                }
                break;
            }
        }
        if (!found) throw std::runtime_error("No hardware NVIDIA adapter; software fallback refused");
    }
    ~KmtDriver() override {
        // Retain the view for process teardown if native destruction failed;
        // do not unmap backing that is still secured by an allocation.
        releaseGuestMemory();
    }
    native_gpu::Capabilities capabilities() const override {
        // Bit 0: adapter/device lifecycle. Bit 1: paging queue/fence query.
        // Bit 2: bounded host-backed allocations. Bit 3: configured section
        // imports. Bit 4: bounded, synchronous GPU buffer copy between shared
        // allocations. No arbitrary guest command stream or scanout support.
        return {Version, (guestSectionName.empty() ? 7u : 31u) | (contextsEnabled ? ContextCapability : 0u) |
                (queriesEnabled ? QueryCapability : 0u) | (runtime ? GuestPagingCapability : 0u) |
                (allocationsEnabled ? VendorAllocationCapability : 0u) | (gpuVaEnabled ? VendorGpuVaCapability : 0u) |
                (residencyEnabled ? VendorResidencyCapability | VendorMemoryManagementCapability : 0u) | (cpuEnabled ? VendorCpuCapability : 0u) |
                (cpuEnabled && runtime && runtime->allocationSlotLimit() >= ExpandedVendorCpuSlots ? ExpandedVendorCpuCapability : 0u) |
                (gpuVaEnabled && runtime && runtime->allocationSlotLimit() >= ExpandedVendorCpuSlots ? ExpandedVendorGpuCapability : 0u) |
                (allocationsEnabled && runtime && runtime->allocationSlotLimit() >= ExpandedVendorCpuSlots ? ExpandedVendorObjectsCapability : 0u) |
                (translationEnabled ? VendorTranslationCapability : 0u) | (hwQueuesEnabled ? HwQueueCapability : 0u) |
                (hwQueuesEnabled ? NoBroadcastSignalHwQueueCapability : 0u) |
                (hwQueuesEnabled ? NoBroadcastWaitHwQueueCapability : 0u) |
                (syncEnabled ? SyncCapability : 0u) | (submitEnabled ? HwSubmitCapability : 0u) |
                (asyncSubmitEnabled ? AsyncHwSubmitCapability : 0u) |
                (asyncSubmitEnabled && syncEnabled ? HwQueueSignalCapability : 0u) |
                (allocationsEnabled ? VendorResourceCapability : 0u) |
                (sharedResourcesEnabled ? SharedVendorResourceCapability : 0u) |
                (consumeSharedEnabled ? SharedTextureConsumeCapability : 0u) |
                (nativeMetadataEnabled ? NativeSharedTextureCapability | gpu_diagnostic::Capability : 0u) |
                (submitEnabled && syncEnabled ? ContextSignalCapability : 0u) |
                (retirementEnabled ? VendorRetirementCapability : 0u) | (reservationEnabled ? GpuReservationCapability : 0u) |
                (gpuStateEnabled ? GpuStateCapability : 0u),
                description.VendorId, description.DeviceId};
    }
    Result openAdapter() override {
        D3DKMT_OPENADAPTERFROMLUID a{}; a.AdapterLuid = luid;
        const auto status = D3DKMTOpenAdapterFromLuid(&a);
        if (status >= 0) {
            if (!a.hAdapter || ownedAdapters.count(a.hAdapter)) throw std::runtime_error("Unexpected native adapter identity");
            try {
                ownedAdapters.emplace(a.hAdapter);
            } catch (...) {
                D3DKMT_CLOSEADAPTER release{}; release.hAdapter = a.hAdapter;
                if (a.hAdapter && D3DKMTCloseAdapter(&release) < 0) ++cleanupFailures;
                throw;
            }
            ++activeAdapters;
        }
        return {status, a.hAdapter, 0};
    }
    bool samePhysicalAdapter(std::uint32_t first, std::uint32_t second) const override {
        // Every owned adapter is opened above using this worker's immutable,
        // selected LUID. No guest-supplied LUID or foreign native handle enters it.
        return ownedAdapters.count(first) && ownedAdapters.count(second);
    }
    Result queryVersion(std::uint32_t adapter) override {
        D3DKMT_DRIVERVERSION version{};
        D3DKMT_QUERYADAPTERINFO a{}; a.hAdapter = adapter; a.Type = KMTQAITYPE_DRIVERVERSION;
        a.pPrivateDriverData = &version; a.PrivateDriverDataSize = sizeof version;
        const auto status = D3DKMTQueryAdapterInfo(&a); return {status, 0, static_cast<std::uint64_t>(version)};
    }
    Result createDevice(std::uint32_t adapter) override {
        D3DKMT_CREATEDEVICE a{}; a.hAdapter = adapter;
        // Match the Linux D3D12 runtime's documented RequestVSync flag when
        // context support is enabled. GPU timeout protection stays enabled.
        a.Flags.RequestVSync = contextsEnabled ? 1u : 0u;
        const auto status = D3DKMTCreateDevice(&a);
        if (status >= 0) {
            try { deviceAdapters.emplace(a.hDevice, adapter); }
            catch (...) {
                D3DKMT_DESTROYDEVICE release{}; release.hDevice = a.hDevice;
                if (D3DKMTDestroyDevice(&release) < 0) ++cleanupFailures;
                throw;
            }
            ++activeDevices;
        }
        return {status, a.hDevice, 0};
    }
    Result queryDeviceExecutionState(std::uint32_t device) override {
        if(!deviceAdapters.count(device)) return {Invalid,0,0};
        D3DKMT_GETDEVICESTATE state{};state.hDevice=device;state.StateType=D3DKMT_DEVICESTATE_EXECUTION;
        const auto status=D3DKMTGetDeviceState(&state);
        if(status!=0) {++failedDeviceExecutionQueries;return {status>0?Invalid:status,0,0};}
        const auto value=static_cast<std::uint32_t>(state.ExecutionState);
        if(value<1 || value>7) {++failedDeviceExecutionQueries;return {Invalid,0,0};}
        ++completedDeviceExecutionQueries;
        const auto previous=deviceExecutionStates.find(device);
        if(previous==deviceExecutionStates.end() || previous->second!=value) {
            std::cerr<<"{\"nativeDeviceExecutionState\":true,\"device\":"<<device<<",\"state\":"<<value
                     <<",\"nativeCall\":true,\"cachedReply\":false}\n";
            deviceExecutionStates[device]=value;
        }
        return {0,0,value};
    }
    Result queryAdapter(std::uint32_t adapter, QueryDesc desc, std::vector<std::uint8_t>& data) override {
        if (!queriesEnabled || !validQuery(desc) || data.size() != desc.bytes) return {Invalid, 0, 0};
        D3DKMT_QUERYADAPTERINFO query{}; query.hAdapter = adapter;
        query.Type = static_cast<KMTQUERYADAPTERINFOTYPE>(desc.type);
        query.pPrivateDriverData = data.data(); query.PrivateDriverDataSize = desc.bytes;
        const auto status = D3DKMTQueryAdapterInfo(&query);
        if (status >= 0) ++completedQueries; else ++failedQueries;
        // Native data is returned unchanged. Linux UMD filename selection,
        // UTF-16 conversion and virtual adapter flags belong to the guest API.
        return {status, 0, desc.bytes};
    }
    Result createContext(std::uint32_t device, ContextDesc desc, std::vector<std::uint8_t>& data) override {
        if (!contextsEnabled || !validContext(desc) || data.size() != desc.privateBytes) return {Invalid, 0, 0};
        D3DKMT_CREATECONTEXTVIRTUAL a{}; a.hDevice = device;
        a.NodeOrdinal = desc.node; a.EngineAffinity = desc.engine; a.Flags.Value = desc.flags;
        a.ClientHint = static_cast<D3DKMT_CLIENTHINT>(desc.clientHint);
        a.PrivateDriverDataSize = desc.privateBytes; a.pPrivateDriverData = data.empty() ? nullptr : data.data();
        const auto status = D3DKMTCreateContextVirtual(&a);
        if (status >= 0) {
            try {
                if (!a.hContext || !contextOwners.emplace(a.hContext, Context{device, desc.flags}).second)
                    throw std::runtime_error("Unexpected native context identity");
            } catch (...) {
                D3DKMT_DESTROYCONTEXT cleanup{}; cleanup.hContext = a.hContext;
                if (D3DKMTDestroyContext(&cleanup) < 0) ++cleanupFailures;
                throw;
            }
            ++activeContexts;
        }
        return {status, a.hContext, 0};
    }
    Result setContextInProcessPriority(std::uint32_t context, std::int32_t priority) override {
        const auto owner = contextOwners.find(context);
        if (!contextsEnabled || !validContextPriority(priority) || owner == contextOwners.end() || owner->second.flags != 16)
            return {Invalid, 0, 0};
        D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY setting{}; setting.hContext = context; setting.Priority = priority;
        const auto status = D3DKMTSetContextInProcessSchedulingPriority(&setting);
        if (status != 0) { ++failedContextPriorities; return {status, 0, 0}; }
        D3DKMT_GETCONTEXTINPROCESSSCHEDULINGPRIORITY observed{}; observed.hContext = context;
        const auto readStatus = D3DKMTGetContextInProcessSchedulingPriority(&observed);
        if (readStatus != 0 || observed.Priority != priority) {
            ++failedContextPriorities;
            return {readStatus < 0 ? readStatus : Invalid, 0, 0};
        }
        ++completedContextPriorities;
        std::cerr << "{\"nativeContextPriorityVerified\":true,\"priority\":" << priority << ",\"inProcessOnly\":true}\n";
        return {0, 0, 0};
    }
    HwQueueResult createHwQueue(std::uint32_t context, HwQueueDesc desc, std::vector<std::uint8_t>& data) override {
        const auto parent = contextOwners.find(context);
        if (!hwQueuesEnabled || !runtime || !validHwQueue(desc) || data.size() != desc.privateBytes ||
            parent == contextOwners.end() || parent->second.flags != 16)
            return {{Invalid, 0, 0}, 0, 0, 0};
        if(hwQueues.size()>=hwQueueLimit()) {
            ++hwQueueQuotaRejections;
            std::cerr<<"{\"nativeObjectAdmissionRejected\":true,\"kind\":\"hwQueue\",\"live\":"<<hwQueues.size()<<",\"limit\":"<<hwQueueLimit()<<"}\n";
            return {{static_cast<std::int32_t>(0xc0000017u),0,0},0,0,0};
        }
        if (!runtime->canMapFence() || !reportedGpuUsageAcceptable()) return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, 0, 0, 0};
        D3DKMT_CREATEHWQUEUE a{}; a.hHwContext = context; a.Flags.Value = desc.flags;
        D3DDDI_CREATEHWQUEUEFLAGS layout{}; layout.NoBroadcastSignal = 1;
        if (layout.Value != NoBroadcastSignalHwQueueFlag) throw std::runtime_error("Native NoBroadcastSignal bit layout mismatch");
        layout.Value = 0; layout.NoBroadcastWait = 1;
        if (layout.Value != NoBroadcastWaitHwQueueFlag) throw std::runtime_error("Native NoBroadcastWait bit layout mismatch");
        a.pPrivateDriverData = data.data(); a.PrivateDriverDataSize = desc.privateBytes;
        // The live UMD constructs this private payload using its translated
        // allocation alias. No captured payload or vendor offset patch is used.
        const auto status = D3DKMTCreateHwQueue(&a);
        if (status < 0) { ++failedHwQueues; return {{status, 0, 0}, 0, 0, 0}; }
        try {
            if (!a.hHwQueue || !hwQueues.emplace(a.hHwQueue, HwQueue{context, parent->second.device, a.hHwQueueProgressFence,
                    static_cast<volatile std::uint64_t*>(a.HwQueueProgressFenceCPUVirtualAddress),
                    a.HwQueueProgressFenceGPUVirtualAddress, std::nullopt}).second)
                throw std::runtime_error("Unexpected native hardware queue identity");
        } catch (...) {
            runtime->stop(0);
            D3DKMT_DESTROYHWQUEUE cleanup{}; cleanup.hHwQueue = a.hHwQueue;
            if (D3DKMTDestroyHwQueue(&cleanup) < 0) ++cleanupFailures;
            throw;
        }
        auto& owned = hwQueues.at(a.hHwQueue);
        try {
            if (status != 0 || a.Flags.Value != desc.flags || a.PrivateDriverDataSize != desc.privateBytes || !owned.sync || owned.sync == a.hHwQueue ||
                !owned.fence || !owned.gpuAddress || owned.gpuAddress % 8 || owned.gpuAddress >= MaxGpuAddress)
                throw std::runtime_error("Unexpected native hardware queue output");
            owned.lease = runtime->map(owned.fence);
            ++completedHwQueues;
            peakHwQueues=(std::max)(peakHwQueues,hwQueues.size());
            if (desc.flags & NoBroadcastSignalHwQueueFlag) ++completedNoBroadcastSignalHwQueues;
            if (desc.flags & NoBroadcastWaitHwQueueFlag) ++completedNoBroadcastWaitHwQueues;
            std::cerr << "{\"nativeHardwareQueueFlagsVerified\":true,\"flags\":" << desc.flags
                      << ",\"unchanged\":true,\"ntstatus\":" << status << "}\n";
            return {{status, a.hHwQueue, 0}, owned.sync, owned.lease->offset, owned.gpuAddress};
        } catch (...) {
            ++failedHwQueues;
            runtime->stop(0); destroy(Kind::HwQueue, a.hHwQueue);
            throw;
        }
    }
    Result submitHwQueue(std::uint32_t handle, HwSubmitDesc desc, const std::vector<std::uint8_t>& data) override {
        return submitNativeHwCommand(handle,desc,data,false);
    }
    Result queueHwCommand(std::uint32_t handle, HwSubmitDesc desc, const std::vector<std::uint8_t>& data) override {
        if (!asyncSubmitEnabled) return {Invalid,0,0};
        return submitNativeHwCommand(handle,desc,data,true);
    }
    bool supportsLiveRetirement() const override {
        return coordinatedObjectsEnabled && coordinatedObjectCapacity.version>=5 && (openGlProfileVersion==13 || openGlProfileVersion==14) &&
            runtime && !runtime->hasStopped() && retirementEnabled && asyncSubmitEnabled;
    }
    bool prepareLiveRetirement() override {
        if(!supportsLiveRetirement() || !collectRetiredSubmissions() || !pendingSubmissions.empty() || !pendingHwSignals.empty() ||
            !pendingHwWaits.empty() || !pendingContextSignals.empty()) return false;
        for(const auto& item:hwQueues) {
            const auto& queue=item.second;
            if(!queue.fence) return false;
            MemoryBarrier();const auto observed=*queue.fence;
            if(observed==UINT64_MAX || observed<queue.submittedFence) return false;
        }
        return true;
    }
    PresentationStopProgress pollCoordinatedPresentationStop() override {
        if(!supportsLiveRetirement()) return PresentationStopProgress::Failed;
        if(!compositorOutput) return PresentationStopProgress::Complete;
        const auto state=compositorOutput->pollStop();
        if(state==driver_shared::ThreadCompletion::Pending) return PresentationStopProgress::Pending;
        if(state!=driver_shared::ThreadCompletion::Complete || !compositorOutput->stop())
            return PresentationStopProgress::Failed;
        compositorOutput.reset();
        return PresentationStopProgress::Complete;
    }
    bool stopCoordinatedPresentation() override {
        if(!supportsLiveRetirement()) return false;
        // Stop joins the UI thread and retires its real input/output fences
        // before releasing imported source references or presentation heaps.
        if(compositorOutput) {
            if(!compositorOutput->stop()) return false;
            compositorOutput.reset();
        }
        return true;
    }
    bool beginCoordinatedObjects(CoordinatedObjectCapacity value) override {
        if(!validCoordinatedObjectCapacity(value) || coordinatedObjectsEnabled || !openglDesktopPresentationEnabled || !runtime || runtime->hasStopped() ||
            !ownedAdapters.empty() || !deviceAdapters.empty() || !hwQueues.empty() || !syncObjects.empty()) return false;
        if(value.version==10 && (!applicationObjectsPermitted || !applicationMemoryEnabled)) return false;
        if(openGlProfileVersion==14 && (!deferredResidencyEnabled || (value.version!=9 && value.version!=10))) return false;
        const auto expected=openGlProfileVersion==14 ? 9u : openGlProfileVersion==13 ? 5u : runtime->fenceSlotLimit()==512 ? 4u : 3u;
        if(value.version!=expected && !(value.version==10 && expected==9 && applicationObjectsPermitted) &&
           !((value.version>=6 && value.version<=8) && expected==5 && deferredResidencyEnabled)) return false;
        coordinatedObjectCapacity=value;coordinatedObjectsEnabled=true;
        std::cerr<<"{\"nativeCoordinatedObjectCapacity\":true,\"version\":"<<value.version<<",\"vendorAllocationObjects\":"<<vendorObjectLimit()<<",\"hwQueues\":"<<hwQueueLimit()<<",\"syncObjects\":"<<syncObjectLimit()
                 <<",\"reservedAddressBytes\":"<<reservationByteLimit()<<",\"reservationCount\":"<<reservationLimit()
                 <<",\"sharedResourceReferences\":"<<sharedResourceLimit()<<",\"sharedSyncReferences\":"<<sharedSyncLimit()
                 <<",\"kernelLeaseLimit\":"<<value.leaseLimit<<",\"fenceSlots\":"<<runtime->fenceSlotLimit()<<",\"cpuBytes\":"<<vendorCpuByteLimit(capabilities().flags,openglDesktop4kCpuEnabled,openGlProfileVersion==14)<<",\"gpuVaBytes\":1073741824,\"reportedGpuBytes\":"<<reportedGpuUsageLimit()<<",\"memoryBudgetsMultiplied\":false,\"reservationMappingsSeparated\":"<<(value.version>=6?"true":"false")<<"}\n";
        return true;
    }
    StreamProfile beginStreaming() override {
        if (!collectRetiredSubmissions()) return {};
        if (!gpu_diagnostic::Streaming || streamingEnabled || openglEnabled || !nativeMetadataEnabled || !consumeSharedEnabled || !asyncSubmitEnabled ||
            !runtime || runtime->hasStopped() || !pendingSubmissions.empty() || !pendingHwSignals.empty() || !sharedResourceIds.empty()) return {};
        streamingEnabled = true;
        std::cerr << "{\"guestStreamNegotiated\":true,\"version\":1,\"frames\":64,\"pendingCommands\":16,\"pendingSignals\":16,\"totalCommands\":1024,\"totalSignals\":256,\"requestLimit\":50000}\n";
        return GuestStreamProfile;
    }
    OpenGlProfile beginOpenGl(std::uint32_t version = 1) override {
        if(version==14 && !deferredResidencyEnabled) return {};
        if (version>=6 && !(capabilities().flags & ExpandedVendorGpuCapability)) return {};
        if (version>=7 && (!(capabilities().flags & ExpandedVendorCpuCapability) || !runtime ||
            runtime->allocationSlotLimit()!=(version>=12 ? DesktopVendorCpuSlots : version>=8 ? SparseVendorCpuSlots : MaxVendorCpuSlots) || runtime->fenceSlotLimit()!=(version>=11 ? 512u : 256u))) return {};
        if (runtime && runtime->allocationSlotLimit()==SparseVendorCpuSlots && version!=8 && version!=9 && version!=10 && version!=11) return {};
        if (runtime && runtime->allocationSlotLimit()==DesktopVendorCpuSlots && version!=12 && version!=13 && version!=14) return {};
        if (!gpu_diagnostic::HighResolution || openglEnabled || streamingEnabled || !asyncSubmitEnabled || !syncEnabled || !reservationEnabled ||
            !runtime || runtime->hasStopped() || !ownedAdapters.empty() || !deviceAdapters.empty() ||
            !pendingSubmissions.empty() || !pendingHwSignals.empty() || (version != 1 && version != 2 && version != 3 && version != 4 && version != 5 && version != 6 && version != 7 && version != 8 && version != 9 && version != 10 && version!=11 && version!=12 && version!=13 && version!=14)) return {};
        openglEnabled = true;openGlProfileVersion=version;
        openglFencesEnabled = version >= 2;
        openglCompositorEnabled = version >= 3;
        openglRenderEnabled = version >= 4;
        openglDesktopEnabled = version >= 5;
        openglDesktop4kEnabled = version >= 6;
        openglDesktop4kCpuEnabled = version >= 7;
        openglDesktopSparseEnabled = version >= 8;
        openglDesktopLargeMapEnabled = version >= 9;
        openglDesktopPresentationEnabled = version >= 10;
        const auto profile = version==14 ? GuestOpenGlMultiappProfile : version==13 ? GuestOpenGlDesktopReferenceProfile : version==12 ? GuestOpenGlDesktopSlotProfile : version==11 ? GuestOpenGlDesktopFenceProfile : version == 10 ? GuestOpenGlDesktopPresentationProfile : version == 9 ? GuestOpenGlDesktopLargeMapProfile : version == 8 ? GuestOpenGlDesktopSparseProfile : version == 7 ? GuestOpenGlDesktop4kCpuProfile : version == 6 ? GuestOpenGlDesktop4kProfile : version == 5 ? GuestOpenGlDesktopProfile : version == 4 ? GuestOpenGlCompositorRenderProfile : version == 3 ? GuestOpenGlCompositorProfile : version == 2 ? GuestOpenGlFenceProfile : GuestOpenGlProfile;
        std::cerr << "{\"openGlProfileNegotiated\":true,\"version\":" << version << ",\"frames\":" << profile.frames << ",\"commandLimit\":" << profile.commandLimit << ",\"pendingCommands\":16,\"reservationLimit\":" << profile.reservationLimit << ",\"reservedByteLimit\":17179869184,\"requestLimit\":" << profile.requestLimit;
        if (version >= 2) std::cerr << ",\"objectLimit\":" << profile.objectLimit
            << ",\"hwQueueLimit\":" << OpenGlFenceHwQueues << ",\"vendorObjectLimit\":" << vendorObjectLimit() << ",\"syncObjectLimit\":" << syncObjectLimit();
        if (version >= 6) std::cerr << ",\"gpuMappedByteLimit\":" << vendorGpuPageLimit(capabilities().flags,true,openglDesktopLargeMapEnabled)*4096ull
            << ",\"reportedGpuUsageLimitBytes\":" << reportedGpuUsageLimit();
        if (version>=7) std::cerr << ",\"cpuMappedByteLimit\":" << vendorCpuByteLimit(capabilities().flags,openglDesktop4kCpuEnabled,openGlProfileVersion==14)
            << ",\"kernelCapacityRequired\":true,\"cpuApertureSlots\":" << runtime->allocationSlotLimit();
        std::cerr << "}\n";
        return profile;
    }
    Result submitNativeHwCommand(std::uint32_t handle, HwSubmitDesc desc, const std::vector<std::uint8_t>& data, bool asynchronous) {
        if (!collectRetiredSubmissions()) throw std::runtime_error("Native asynchronous retirement failed");
        const auto queue = hwQueues.find(handle);
        const auto reject = [](unsigned reason) {
            std::cerr << "{\"nativeSubmissionRejected\":true,\"preflightReason\":" << reason << "}\n";
            return Result{Invalid, 0, 0};
        };
        if (!submitEnabled || !runtime || runtime->hasStopped() || !validHwSubmit(desc) || data.size() != desc.privateBytes ||
            queue == hwQueues.end() || !queue->second.lease || desc.fence <= queue->second.submittedFence ||
            !lifetimeBudgetAvailable(submissionAttempts,streamingEnabled ? GuestStreamProfile.totalCommands : openglEnabled ? openGlCommandLimit() : MaxHwSubmissions) ||
            desc.bytes>UINT64_MAX-acceptedCommandBytes ||
            pendingSubmissions.size() >= MaxHwSubmissions) return reject(1);
        unsigned owners = 0;
        for (const auto& item : vendorAllocations) {
            const auto& allocation = item.second;
            if (allocation.device != queue->second.device) continue;
            if (!allocation.pages || desc.address < allocation.address) continue;
            const auto offset = desc.address - allocation.address;
            if (offset <= allocation.pages * 4096 && desc.bytes <= allocation.pages * 4096 - offset) {
                if (!allocation.resident) return reject(2);
                if (!allocation.locked || !allocation.cpuLease || offset > allocation.cpuBytes || desc.bytes > allocation.cpuBytes - offset)
                    return reject(3);
                ++owners;
            }
        }
        if (owners != 1) return reject(4);
        MemoryBarrier();
        const auto initial = *queue->second.fence;
        std::cerr << "{\"nativeSubmissionPreflight\":true,\"initialFence\":" << initial << ",\"targetFence\":" << desc.fence << "}\n";
        if (initial >= desc.fence) return reject(5);
        D3DKMT_SUBMITCOMMANDTOHWQUEUE a{}; a.hHwQueue = handle; a.HwQueueProgressFenceId = desc.fence;
        a.CommandBuffer = desc.address; a.CommandLength = desc.bytes; a.PrivateDriverDataSize = desc.privateBytes;
        a.pPrivateDriverData = data.empty() ? nullptr : const_cast<std::uint8_t*>(data.data());
        ++submissionAttempts;
        const auto status = D3DKMTSubmitCommandToHwQueue(&a);
        MemoryBarrier(); const auto returnedFence = *queue->second.fence;
        std::cerr << "{\"nativeSubmissionReturned\":true,\"ntstatus\":" << status
                  << ",\"target\":" << desc.fence << ",\"observed\":" << returnedFence
                  << ",\"async\":" << (asynchronous ? "true" : "false") << "}\n";
        if (status < 0) { ++failedSubmissions; return {status, 0, 0}; }
        queue->second.submittedFence = desc.fence;
        acceptedCommandBytes += desc.bytes;
        if (status != 0) { ++failedSubmissions; throw std::runtime_error("Unexpected native submission status"); }
        if (asynchronous) {
            // The KMT call accepts work; a progress fence proves completion.
            // Do not block the UMD from queuing a later dependency or signal.
            pendingSubmissions.push_back({++acceptedAsyncSubmissions, handle, desc.bytes, desc.privateBytes, desc.fence});
            peakPendingSubmissions = (std::max)(peakPendingSubmissions, static_cast<unsigned>(pendingSubmissions.size()));
            std::cerr << "{\"nativeAsyncSubmissionAccepted\":true,\"index\":" << acceptedAsyncSubmissions
                      << ",\"bytes\":" << desc.bytes << ",\"privateBytes\":" << desc.privateBytes
                      << ",\"target\":" << desc.fence << ",\"observed\":" << returnedFence << "}\n";
            if (!collectRetiredSubmissions()) throw std::runtime_error("Native asynchronous retirement failed");
            return {status, 0, returnedFence};
        }
        const auto deadline = GetTickCount64() + 5000;
        std::uint64_t observed = 0;
        do {
            MemoryBarrier(); observed = *queue->second.fence;
            if (observed >= desc.fence && observed != UINT64_MAX) {
                ++completedSubmissions; submittedCommandBytes += desc.bytes;
                return {status, 0, observed};
            }
            if (observed == UINT64_MAX) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        ++failedSubmissions; ++submissionTimeouts;
        std::cerr << "{\"nativeSubmissionDeadlineExceeded\":true,\"target\":" << desc.fence << ",\"observed\":" << observed << "}\n";
        // Keep the queue, all allocations and sync objects owned. The outer
        // server reaps its VM before attempting native queue destruction.
        throw std::runtime_error("Native command progress fence deadline exceeded");
    }
    Result queueHwQueueSignal(std::uint32_t sync, const std::vector<std::uint32_t>& queues, HwQueueSignalDesc desc) override {
        if (!collectRetiredSubmissions()) return {Invalid,0,0};
        const auto object = syncObjects.find(sync);
        if (!asyncSubmitEnabled || !syncEnabled || !runtime || runtime->hasStopped() || !validHwQueueSignal(desc) ||
            queues.size() != desc.count || object == syncObjects.end() || object->second.type != 5 ||
            !packetSignalSyncFlags(object->second.flags) || !object->second.lease || !object->second.fence ||
            desc.fence <= object->second.lastSignal || (streamingEnabled ? hwQueueSignalAttempts + contextSignalAttempts >= GuestStreamProfile.totalSignals :
                                                                        !lifetimeBudgetAvailable(hwQueueSignalAttempts,hwSignalLimit())) ||
            pendingHwSignals.size() >= MaxHwQueueSignals) return {Invalid,0,0};
        PendingHwSignal pending{}; pending.sync=sync; pending.flags=desc.flags; pending.count=desc.count; pending.target=desc.fence;
        for (std::size_t n=0;n<queues.size();++n) {
            const auto queue = hwQueues.find(queues[n]);
            if (queue == hwQueues.end() || queue->second.device != object->second.device || !queue->second.lease ||
                std::find(queues.begin(),queues.begin()+n,queues[n]) != queues.begin()+n) return {Invalid,0,0};
            pending.queues[n]=queues[n];
        }
        D3DDDICB_SIGNALFLAGS rewind{}; rewind.AllowFenceRewind=1;
        if (rewind.Value != 4) throw std::runtime_error("Native signal flag layout changed");
        D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE a{};
        a.Flags.Value=desc.flags; a.BroadcastHwQueueCount=desc.count; a.BroadcastHwQueueArray=queues.data();
        a.ObjectCount=1; a.ObjectHandleArray=&sync; a.FenceValueArray=&desc.fence;
        ++hwQueueSignalAttempts;
        const auto status=D3DKMTSubmitSignalSyncObjectsToHwQueue(&a);
        MemoryBarrier(); const auto observed=*object->second.fence;
        std::cerr << "{\"nativeHwQueueSignalReturned\":true,\"flags\":" << desc.flags << ",\"queues\":" << desc.count
                  << ",\"target\":" << desc.fence << ",\"observed\":" << observed << ",\"ntstatus\":" << status << "}\n";
        if (status < 0) { ++failedHwQueueSignals; return {status,0,0}; }
        if (status != 0) { ++failedHwQueueSignals; throw std::runtime_error("Unexpected native hardware queue signal status"); }
        object->second.lastSignal=desc.fence;
        pending.index=++acceptedHwQueueSignals; pendingHwSignals.push_back(pending);
        std::cerr << "{\"nativeHwQueueSignalAccepted\":true,\"index\":" << pending.index << ",\"flags\":" << desc.flags
                  << ",\"queues\":" << desc.count << ",\"target\":" << desc.fence << ",\"observed\":" << observed
                  << ",\"noGpuAccess\":true,\"cpuValueWrittenByBridge\":false}\n";
        peakPendingHwQueueSignals=(std::max)(peakPendingHwQueueSignals,static_cast<unsigned>(pendingHwSignals.size()));
        if (!collectRetiredSubmissions()) throw std::runtime_error("Native hardware queue signal retirement failed");
        return {status,0,observed};
    }
    Result queueHwQueueWait(std::uint32_t queueId, const std::vector<HwQueueWaitItem>& items) override {
        return queueGpuWait(queueId, items, false);
    }
    Result queueContextWait(std::uint32_t contextId, const std::vector<HwQueueWaitItem>& items) override {
        return queueGpuWait(contextId, items, true);
    }
    Result queueGpuWait(std::uint32_t queueId, const std::vector<HwQueueWaitItem>& items, bool contextStream) {
        if (!collectRetiredSubmissions()) return {Invalid, 0, 0};
        const auto queue = hwQueues.find(queueId);
        const auto context = contextOwners.find(queueId);
        if (!syncSharingEnabled || !asyncSubmitEnabled || !runtime || runtime->hasStopped() ||
            (contextStream ? context == contextOwners.end() || !contextCanWait(context->second.flags,coordinatedObjectsEnabled) : queue == hwQueues.end()) ||
            items.empty() || items.size() > MaxHwQueueWaitObjects || acceptedHwQueueWaits==UINT64_MAX) return {Invalid, 0, 0};
        if (pendingHwWaits.size() >= MaxPendingHwQueueWaits) return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        const auto device = contextStream ? context->second.device : queue->second.device;
        PendingHwWait pending{}; pending.queue = queueId; pending.count = static_cast<std::uint32_t>(items.size()); pending.contextStream = contextStream;
        unsigned borrowedPaging=0,borrowedHardware=0;
        for (std::size_t n = 0; n < items.size(); ++n) {
            std::uint32_t sourceDevice=0;volatile std::uint64_t* word=nullptr;
            if (items[n].reserved || items[n].fence == UINT64_MAX || !waitFence(items[n].sync,sourceDevice,word) || sourceDevice!=device ||
                std::find(pending.syncs.begin(), pending.syncs.begin() + n, items[n].sync) != pending.syncs.begin() + n) return {Invalid, 0, 0};
            if(!syncObjects.count(items[n].sync)) {
                const bool hardware=std::any_of(hwQueues.begin(),hwQueues.end(),[&](const auto& entry){return entry.second.sync==items[n].sync;});
                if(hardware) ++borrowedHardware;else ++borrowedPaging;
            }
            pending.syncs[n] = items[n].sync; pending.targets[n] = items[n].fence;
        }
        D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE wait{};
        wait.hHwQueue = queueId; wait.ObjectCount = pending.count;
        wait.ObjectHandleArray = pending.syncs.data(); wait.FenceValueArray = pending.targets.data();
        D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU contextWait{};
        contextWait.hContext = queueId; contextWait.ObjectCount = pending.count;
        contextWait.ObjectHandleArray = pending.syncs.data(); contextWait.MonitoredFenceValueArray = pending.targets.data();
        const auto before = GetTickCount64();
        const auto status = contextStream ? D3DKMTWaitForSynchronizationObjectFromGpu(&contextWait) :
                                            D3DKMTSubmitWaitForSyncObjectsToHwQueue(&wait);
        const auto elapsed = GetTickCount64() - before;
        if (status < 0) { ++failedHwQueueWaits; return {status, 0, 0}; }
        if (status != 0) { ++failedHwQueueWaits; throw std::runtime_error("Unexpected native GPU queue wait status"); }
        pending.index = ++acceptedHwQueueWaits; pendingHwWaits.push_back(pending);
        if(borrowedPaging || borrowedHardware) std::cerr<<"{\"nativeBorrowedQueueGpuWaitAccepted\":true,\"index\":"<<pending.index<<",\"paging\":"<<borrowedPaging
            <<",\"hardware\":"<<borrowedHardware<<",\"cpuValueWrittenByBridge\":false}\n";
        std::cerr << "{\"nativeGpuQueueWaitAccepted\":true,\"index\":" << pending.index << ",\"objects\":" << pending.count
                  << ",\"contextStream\":" << (contextStream ? "true" : "false")
                  << ",\"firstTarget\":" << pending.targets[0] << ",\"nativeCallMs\":" << elapsed << ",\"cpuWait\":false}\n";
        peakPendingHwQueueWaits = (std::max)(peakPendingHwQueueWaits, static_cast<unsigned>(pendingHwWaits.size()));
        if (!collectRetiredSubmissions()) throw std::runtime_error("Native GPU wait dependency failure");
        return {0, 0, 0};
    }
    Result reserveGpuAddress(std::uint32_t adapter, GpuReservationDesc desc) override {
        if (!reservationEnabled || !runtime || !ownedAdapters.count(adapter) || !validGpuReservation(desc) || nextGpuReservation == UINT32_MAX) return {Invalid,0,0};
        if(gpuReservations.size()>=reservationLimit() || gpuReservedBytes>reservationByteLimit() || desc.bytes>reservationByteLimit()-gpuReservedBytes) {
            ++reservationQuotaRejections;
            std::cerr<<"{\"nativeObjectAdmissionRejected\":true,\"kind\":\"gpuReservation\",\"live\":"<<gpuReservations.size()
                     <<",\"limit\":"<<reservationLimit()<<",\"reservedBytes\":"<<gpuReservedBytes<<",\"requestedBytes\":"<<desc.bytes<<",\"byteLimit\":"<<reservationByteLimit()<<"}\n";
            return {static_cast<std::int32_t>(0xc0000017u),0,0};
        }
        D3DDDI_RESERVEGPUVIRTUALADDRESS a{}; a.hAdapter = adapter;
        a.BaseAddress = desc.base; a.MinimumAddress = desc.minimum; a.MaximumAddress = desc.maximum; a.Size = desc.bytes;
        const auto status = D3DKMTReserveGpuVirtualAddress(&a);
        if (status < 0) { ++failedReservations; return {status, 0, 0}; }
        const auto id = ++nextGpuReservation;
        try { gpuReservations.emplace(id, GpuReservation{adapter, a.VirtualAddress, desc.bytes}); }
        catch (...) {
            D3DKMT_FREEGPUVIRTUALADDRESS release{}; release.hAdapter = adapter; release.BaseAddress = a.VirtualAddress; release.Size = desc.bytes;
            if (!a.VirtualAddress || D3DKMTFreeGpuVirtualAddress(&release) != 0) ++cleanupFailures;
            throw;
        }
        gpuReservedBytes += desc.bytes; peakGpuReservedBytes = (std::max)(peakGpuReservedBytes, gpuReservedBytes);
        peakReservations=(std::max)(peakReservations,gpuReservations.size());
        if (status != 0 || !validGpuReservationOutput(desc, a.VirtualAddress) || a.Reserved0 || a.Reserved1 || a.Reserved2) {
            ++failedReservations; runtime->stop(0); destroy(Kind::GpuReservation, id);
            throw std::runtime_error("Unexpected native GPU reservation output");
        }
        ++completedReservations; return {status, id, a.VirtualAddress};
    }
    GpuVaResult mapGpuState(std::uint32_t reservation, std::uint32_t queue, GpuVaDesc desc) override {
        const auto range = gpuReservations.find(reservation);
        const auto paging = pagingFences.find(queue);
        if (!gpuStateEnabled || !runtime || runtime->hasStopped() || !validGpuState(desc) || range == gpuReservations.end() ||
            paging == pagingFences.end() || !deviceAdapters.count(paging->second.device) ||
            deviceAdapters.at(paging->second.device) != range->second.adapter ||
            !gpuRangeContains(range->second.address, range->second.bytes, desc.base, desc.sizePages * 4096)) return {{Invalid, 0, 0}, 0};
        bool allocationOverlap=false;
        for(const auto& mapped:vendorAllocations) {
            if(!gpuRangesOverlap(desc.base,desc.sizePages*4096,mapped.second.address,mapped.second.pages*4096)) continue;
            if(mapped.second.device!=paging->second.device ||
                !gpuRangeContains(desc.base,desc.sizePages*4096,mapped.second.address,mapped.second.pages*4096)) return {{Invalid,0,0},0};
            allocationOverlap=true;
        }
        for (const auto& mapped : allocations)
            if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.second.gpuAddress, mapped.second.size)) return {{Invalid, 0, 0}, 0};
        GpuStateRanges::Plan plan;
        if (!gpuStates.prepare(reservation, desc.base, desc.sizePages * 4096, desc.protection, plan)) return {{Invalid, 0, 0}, 0};
        const auto retirementScope=gpuStates.retirementScope(reservation,desc.base,desc.sizePages*4096,desc.protection,allocationOverlap);
        const bool retirementRequired=retirementScope!=GpuStateRanges::RetirementScope::PagingFence;
        // A fresh or identical reserved state releases no allocation data.
        // WDDM orders it with the paging fence below. Allocation replacements
        // wait for their owning device; protection changes wait for all devices.
        const auto retired=[&] {
            for(const auto& item:hwQueues) {
                const auto& hardware=item.second;
                if(!GpuStateRanges::retirementCoversQueue(retirementScope,paging->second.device,hardware.device)) continue;
                if(!hardware.fence) return false;
                MemoryBarrier();const auto observed=*hardware.fence;
                if(observed==UINT64_MAX || observed<hardware.submittedFence) return false;
            }
            return true;
        };
        if(retirementRequired && !retired()) {
            const auto started=GetTickCount64();++gpuStateRetirementWaits;
            const bool complete=boundedGpuRetirementWait(retired,[]{return GetTickCount64();},[]{Sleep(1);});
            if(!complete) ++gpuStateRetirementTimeouts;
            std::cerr<<"{\"nativeGpuStateRetirementWait\":true,\"complete\":"<<(complete ? "true" : "false")
                <<",\"elapsedMs\":"<<GetTickCount64()-started<<",\"limitMs\":250,\"cpuFenceWritten\":false"
                <<",\"mappingDevice\":"<<paging->second.device<<",\"allocationOverlap\":"<<(allocationOverlap ? "true" : "false")
                <<",\"base\":"<<desc.base<<",\"bytes\":"<<desc.sizePages*4096<<",\"busyQueues\":[";
            bool first=true;
            for(const auto& item:hwQueues) {
                const auto& hardware=item.second;MemoryBarrier();
                if(!GpuStateRanges::retirementCoversQueue(retirementScope,paging->second.device,hardware.device)) continue;
                const auto observed=hardware.fence ? *hardware.fence : UINT64_MAX;
                if(observed!=UINT64_MAX && observed>=hardware.submittedFence) continue;
                if(!first) std::cerr<<',';
                first=false;
                std::cerr<<"{\"device\":"<<hardware.device<<",\"submitted\":"<<hardware.submittedFence<<",\"observed\":"<<observed<<'}';
            }
            std::cerr<<"]}\n";
            if(!complete) return {{Invalid,0,0},0};
        }
        D3DDDI_MAPGPUVIRTUALADDRESS a{}; a.hPagingQueue = queue; // NULL allocation is required for Zero/NoAccess.
        a.BaseAddress = desc.base; a.MinimumAddress = desc.minimum; a.MaximumAddress = desc.maximum;
        a.SizeInPages = desc.sizePages; a.Protection.Value = desc.protection;
        const auto status = D3DKMTMapGpuVirtualAddress(&a);
        if (status < 0) { ++failedGpuStateMaps; return {{status, 0, 0}, 0}; }
        for (auto& item : vendorAllocations) {
            auto& mapped = item.second;
            if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.address, mapped.pages * 4096)) {
                vendorMappedPages -= static_cast<std::uint32_t>(mapped.pages); mapped.pages = 0; mapped.address = 0; mapped.offsetPages = 0;
                ++gpuStateReplacedAllocationMappings;
            }
        }
        gpuStates.commit(plan); // Retain ownership if validation or paging retirement fails.
        if ((status != 0 && status != 259) || a.VirtualAddress != desc.base || a.Reserved0 || a.Reserved1 ||
            !waitPaging(queue, a.PagingFenceValue)) {
            ++failedGpuStateMaps; throw std::runtime_error("Native GPU address-state mapping or paging retirement failed");
        }
        ++completedGpuStateMaps; ++completedGpuStateWaits;
        std::cerr<<"{\"nativeGpuStateMappingPolicy\":true,\"retirementRequired\":"<<(retirementRequired ? "true" : "false")
            <<",\"allocationOverlap\":"<<(allocationOverlap ? "true" : "false")
            <<",\"retirementScope\":\""<<(retirementScope==GpuStateRanges::RetirementScope::PagingFence ? "pagingFence" :
                retirementScope==GpuStateRanges::RetirementScope::OwningDevice ? "owningDevice" : "allDevices")
            <<"\",\"pagingFenceRetired\":true,\"cpuFenceWritten\":false}\n";
        return {{status, 0, a.VirtualAddress}, a.PagingFenceValue};
    }
    ResourceSharingProfile beginResourceSharing() override {
        if (!nativeMetadataEnabled || !sharedResourcesEnabled || !runtime || runtime->hasStopped() || resourceSharingEnabled) return {};
        resourceSharingEnabled = true;
        std::cerr << "{\"nativeResourceSharingNegotiated\":true,\"handleLimit\":" << MaxSharedResourceHandles << "}\n";
        return GuestResourceSharingProfile;
    }
    Result exportResource(std::uint32_t resource) override {
        const auto source = resourceGuestRuntime.find(resource);
        if (!resourceSharingEnabled || !runtime || runtime->hasStopped() || source == resourceGuestRuntime.end() ||
            !sharedResourceIds.count(resource) || sharedResourceHandles.size() >= sharedResourceLimit() ||
            nextSharedResource == UINT32_MAX) return {Invalid, 0, 0};
        SharedResource shared;
        shared.guestRuntime = source->second;
        const auto status = shared.handle.share(resource);
        if (status) return {status, 0, 0};
        const auto ticket = nextSharedResource++;
        if (!sharedResourceHandles.emplace(ticket, std::move(shared)).second) throw std::runtime_error("Shared resource ticket collision");
        ++exportedResourceHandles;peakSharedResourceHandles=(std::max)(peakSharedResourceHandles,sharedResourceHandles.size());
        std::cerr << "{\"nativeResourceHandleExported\":true,\"rawHandleExposed\":false}\n";
        return {0, ticket, 0};
    }
    Result querySharedResourceInfo(std::uint32_t ticket, std::uint32_t device, SharedResourceInfo& info, std::vector<std::uint8_t>& guestRuntime) override {
        const auto shared = sharedResourceHandles.find(ticket);
        if (!resourceSharingEnabled || !runtime || runtime->hasStopped() || shared == sharedResourceHandles.end() || !deviceAdapters.count(device))
            return {Invalid, 0, 0};
        D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query{}; query.hDevice = device; query.hNtHandle = shared->second.handle.get();
        const auto status = D3DKMTQueryResourceInfoFromNtHandle(&query);
        if (status < 0) return {status, 0, 0};
        SharedResourceInfo output{static_cast<std::uint32_t>(shared->second.guestRuntime.size()),
            query.TotalPrivateDriverDataSize, query.ResourcePrivateDriverDataSize, query.NumAllocations};
        if (status != 0 || !validSharedResourceInfo(output)) return {Invalid, 0, 0};
        // Windows runtime metadata describes the host D3D12 ABI. Return the
        // original Linux runtime metadata, while keeping native driver sizes.
        guestRuntime = shared->second.guestRuntime; info = output;
        std::cerr << "{\"nativeResourceInfoQueried\":true,\"guestRuntimeBytes\":" << output.runtimeBytes
                  << ",\"hostRuntimeBytes\":" << query.PrivateRuntimeDataSize << ",\"driverBytes\":" << output.driverBytes
                  << ",\"resourceDriverBytes\":" << output.resourceDriverBytes << ",\"allocations\":" << output.allocations << "}\n";
        return {0, 0, 0};
    }
    Result duplicateSharedReference(Kind kind,std::uint32_t source) override {
        if(!runtime || runtime->hasStopped()) return {Invalid,0,0};
        if(kind==Kind::SharedResource) {
            const auto found=sharedResourceHandles.find(source);
            if(!resourceSharingEnabled || found==sharedResourceHandles.end() || sharedResourceHandles.size()>=sharedResourceLimit() ||
                nextSharedResource==UINT32_MAX) {
                std::cerr<<"{\"nativeSharedAliasAdmissionRejected\":true,\"stage\":\"driver\",\"kind\":\"resource\",\"liveReferences\":"<<sharedResourceHandles.size()
                    <<",\"referenceLimit\":"<<sharedResourceLimit()<<",\"sourceOwned\":"<<(found!=sharedResourceHandles.end() ? "true" : "false")
                    <<",\"enabled\":"<<(resourceSharingEnabled ? "true" : "false")<<",\"idExhausted\":"<<(nextSharedResource==UINT32_MAX ? "true" : "false")
                    <<",\"exported\":"<<exportedResourceHandles<<",\"closed\":"<<closedResourceHandles<<"}\n";
                return {Invalid,0,0};
            }
            SharedResource alias;alias.guestRuntime=found->second.guestRuntime;
            const auto status=alias.handle.duplicate(found->second.handle.get());
            if(status) { std::cerr<<"{\"nativeSharedAliasDuplicationFailed\":true,\"kind\":\"resource\",\"ntstatus\":"<<status<<"}\n";return {status,0,0}; }
            const auto ticket=nextSharedResource++;
            if(!sharedResourceHandles.emplace(ticket,std::move(alias)).second) throw std::runtime_error("Resource alias ticket collision");
            ++exportedResourceHandles;peakSharedResourceHandles=(std::max)(peakSharedResourceHandles,sharedResourceHandles.size());
            std::cerr<<"{\"nativeSharedReferenceAliased\":true,\"kind\":\"resource\",\"independentNtReference\":true,\"inheritable\":false,\"rawHandleExposed\":false}\n";
            return {0,ticket,0};
        }
        if(kind==Kind::SharedSync) {
            const auto found=sharedSyncHandles.find(source);
            if(!syncSharingEnabled || found==sharedSyncHandles.end() || sharedSyncHandles.size()>=sharedSyncLimit() ||
                nextSharedSync==UINT32_MAX) {
                std::cerr<<"{\"nativeSharedAliasAdmissionRejected\":true,\"stage\":\"driver\",\"kind\":\"sync\",\"liveReferences\":"<<sharedSyncHandles.size()
                    <<",\"referenceLimit\":"<<sharedSyncLimit()<<",\"sourceOwned\":"<<(found!=sharedSyncHandles.end() ? "true" : "false")
                    <<",\"enabled\":"<<(syncSharingEnabled ? "true" : "false")<<",\"idExhausted\":"<<(nextSharedSync==UINT32_MAX ? "true" : "false")<<"}\n";
                return {Invalid,0,0};
            }
            driver_shared::SyncHandle alias;
            const auto status=alias.duplicate(found->second.get());
            if(status) { std::cerr<<"{\"nativeSharedAliasDuplicationFailed\":true,\"kind\":\"sync\",\"ntstatus\":"<<status<<"}\n";return {status,0,0}; }
            const auto ticket=nextSharedSync++;
            if(!sharedSyncHandles.emplace(ticket,std::move(alias)).second) throw std::runtime_error("Sync alias ticket collision");
            ++exportedSyncHandles;peakSharedSyncHandles=(std::max)(peakSharedSyncHandles,sharedSyncHandles.size());
            std::cerr<<"{\"nativeSharedReferenceAliased\":true,\"kind\":\"sync\",\"independentNtReference\":true,\"inheritable\":false,\"rawHandleExposed\":false}\n";
            return {0,ticket,0};
        }
        return {Invalid,0,0};
    }
    SharedResourceOpenResult importResource(std::uint32_t ticket, SharedResourceOpenDesc desc, std::vector<std::uint8_t>& data) override {
        std::cerr << "{\"nativeResourceFdOpenBegin\":true,\"runtimeBytes\":" << desc.runtimeBytes << ",\"driverBytes\":" << desc.driverBytes << "}\n";
        const auto shared = sharedResourceHandles.find(ticket);
        if (!resourceSharingEnabled || !runtime || runtime->hasStopped() || shared == sharedResourceHandles.end() ||
            !deviceAdapters.count(desc.device) || !validSharedResourceOpen(desc) || !reportedGpuUsageAcceptable() ||
            !vendorObjectAdmission())
            return {{Invalid, 0, 0}, {}};
        D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query{}; query.hDevice = desc.device; query.hNtHandle = shared->second.handle.get();
        auto status = D3DKMTQueryResourceInfoFromNtHandle(&query);
        if (status < 0) return {{status, 0, 0}, {}};
        if (status || query.NumAllocations != 1 || query.PrivateRuntimeDataSize > MaxVendorPrivateBytes ||
            desc.runtimeBytes != shared->second.guestRuntime.size() || desc.driverBytes != query.TotalPrivateDriverDataSize ||
            desc.resourceDriverBytes != query.ResourcePrivateDriverDataSize || data.size() != desc.runtimeBytes + desc.driverBytes + desc.resourceDriverBytes)
            return {{Invalid, 0, 0}, {}};
        // Native D3D12 runtime metadata is consumed on Windows only. The Linux
        // runtime receives the bytes captured from its original creation call.
        std::vector<std::uint8_t> hostRuntime(query.PrivateRuntimeDataSize), resourceData(desc.resourceDriverBytes), driverData(desc.driverBytes);
        D3DDDI_OPENALLOCATIONINFO2 allocation{};
        D3DKMT_OPENRESOURCEFROMNTHANDLE open{};
        open.hDevice = desc.device; open.hNtHandle = shared->second.handle.get(); open.NumAllocations = 1; open.pOpenAllocationInfo2 = &allocation;
        open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize; open.pPrivateRuntimeData = hostRuntime.data();
        open.ResourcePrivateDriverDataSize = desc.resourceDriverBytes; open.pResourcePrivateDriverData = resourceData.data();
        open.TotalPrivateDriverDataBufferSize = desc.driverBytes; open.pTotalPrivateDriverDataBuffer = driverData.data();
        status = D3DKMTOpenResourceFromNtHandle(&open);
        std::cerr << "{\"nativeResourceFdOpenReturned\":true,\"ntstatus\":" << status
                  << ",\"hasResource\":" << (open.hResource ? "true" : "false") << ",\"hasAllocation\":" << (allocation.hAllocation ? "true" : "false")
                  << ",\"hasKeyedMutex\":" << (open.hKeyedMutex ? "true" : "false") << ",\"hasSync\":" << (open.hSyncObject ? "true" : "false")
                  << ",\"runtimeBytes\":" << open.PrivateRuntimeDataSize << ",\"driverBytes\":" << open.TotalPrivateDriverDataBufferSize
                  << ",\"allocationDriverBytes\":" << allocation.PrivateDriverDataSize << ",\"resourceDriverBytes\":" << open.ResourcePrivateDriverDataSize << "}\n";
        if (status < 0) return {{status, 0, 0}, {}};
        const auto rollback = [&] {
            if (open.hKeyedMutex) {
                D3DKMT_DESTROYKEYEDMUTEX close{}; close.hKeyedMutex = open.hKeyedMutex;
                if (D3DKMTDestroyKeyedMutex(&close) != 0) ++cleanupFailures;
            }
            if (open.hSyncObject) {
                D3DKMT_DESTROYSYNCHRONIZATIONOBJECT close{}; close.hSyncObject = open.hSyncObject;
                if (D3DKMTDestroySynchronizationObject(&close) != 0) ++cleanupFailures;
            }
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = desc.device; release.hResource = open.hResource;
            release.Flags.SynchronousDestroy = 1;
            if (!release.hResource) { release.phAllocationList = &allocation.hAllocation; release.AllocationCount = 1; }
            if (D3DKMTDestroyAllocation2(&release) != 0) ++cleanupFailures;
        };
        const auto pointer = reinterpret_cast<std::uintptr_t>(allocation.pPrivateDriverData);
        const auto start = reinterpret_cast<std::uintptr_t>(driverData.data());
        const auto offset = pointer >= start ? pointer - start : UINTPTR_MAX;
        if (status || !open.hResource || !allocation.hAllocation || open.hResource == allocation.hAllocation ||
            open.hKeyedMutex || open.hSyncObject || open.NumAllocations != 1 || open.PrivateRuntimeDataSize != query.PrivateRuntimeDataSize ||
            open.ResourcePrivateDriverDataSize != desc.resourceDriverBytes || open.TotalPrivateDriverDataBufferSize != desc.driverBytes ||
            allocation.GpuVirtualAddress % 4096 || allocation.GpuVirtualAddress >= MaxGpuAddress ||
            !allocation.PrivateDriverDataSize || offset > driverData.size() || allocation.PrivateDriverDataSize > driverData.size() - offset) {
            rollback(); return {{Invalid, 0, 0}, {}};
        }
        if (vendorAllocations.count(allocation.hAllocation) || sharedResourceIds.count(open.hResource) || resourceGuestRuntime.count(open.hResource)) {
            // VidMm must return a separate opened resource instance; never
            // overwrite the source allocation's ownership bookkeeping.
            rollback(); throw std::runtime_error("Native resource import reused an owned identity");
        }
        try {
            VendorAllocation owned{}; owned.device = desc.device; owned.resource = open.hResource; owned.address = allocation.GpuVirtualAddress;
            owned.openedSharedResource = true;
            vendorAllocations.emplace(allocation.hAllocation, owned);
            resourceGuestRuntime.emplace(open.hResource, shared->second.guestRuntime);
            sharedResourceIds.insert(open.hResource);
            auto output = data.begin();
            output = std::copy(shared->second.guestRuntime.begin(), shared->second.guestRuntime.end(), output);
            output = std::copy(resourceData.begin(), resourceData.end(), output);
            std::copy(driverData.begin(), driverData.end(), output);
        } catch (...) {
            vendorAllocations.erase(allocation.hAllocation); resourceGuestRuntime.erase(open.hResource); sharedResourceIds.erase(open.hResource);
            rollback(); throw;
        }
        ++completedVendorAllocations; ++completedVendorResources;
        ++importedResourceFdObjects;
        peakVendorAllocationObjects = (std::max)(peakVendorAllocationObjects, vendorAllocations.size());
        std::cerr << "{\"nativeResourceFdImported\":true,\"allocations\":1,\"guestRuntimeBytes\":" << desc.runtimeBytes
                  << ",\"driverBytes\":" << desc.driverBytes << ",\"rawHandleExposed\":false}\n";
        return {{0, allocation.hAllocation, allocation.GpuVirtualAddress},
            {open.hResource, allocation.PrivateDriverDataSize, static_cast<std::uint32_t>(offset), 0}};
    }
    SyncSharingProfile beginSyncSharing() override {
        if (!syncEnabled || !runtime || runtime->hasStopped() || syncSharingEnabled) return {};
        syncSharingEnabled = true;
        std::cerr << "{\"nativeSyncSharingNegotiated\":true,\"handleLimit\":" << MaxSharedSyncHandles << "}\n";
        return GuestSyncSharingProfile;
    }
    Result exportSync(std::uint32_t sync) override {
        const auto source = syncObjects.find(sync);
        if (!syncSharingEnabled || !runtime || runtime->hasStopped() || source == syncObjects.end() ||
            source->second.type != 5 || !source->second.shared ||
            sharedSyncHandles.size() >= sharedSyncLimit() || nextSharedSync == UINT32_MAX) return {Invalid, 0, 0};
        driver_shared::SyncHandle shared;
        const auto status = shared.share(sync);
        if (status) return {status, 0, 0};
        const auto ticket = nextSharedSync++;
        if (!sharedSyncHandles.emplace(ticket, std::move(shared)).second) throw std::runtime_error("Shared sync ticket collision");
        ++exportedSyncHandles;peakSharedSyncHandles=(std::max)(peakSharedSyncHandles,sharedSyncHandles.size());
        std::cerr << "{\"nativeSyncHandleExported\":true,\"rawHandleExposed\":false}\n";
        return {0, ticket, 0};
    }
    Result querySharedSyncInfo(std::uint32_t ticket, std::uint32_t device) override {
        const auto shared = sharedSyncHandles.find(ticket);
        if (!syncSharingEnabled || !runtime || runtime->hasStopped() ||
            shared == sharedSyncHandles.end() || !deviceAdapters.count(device)) return {Invalid, 0, 0};
        D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE info{};
        info.hDevice = device; info.hNtHandle = shared->second.get();
        const auto status = D3DKMTQueryResourceInfoFromNtHandle(&info);
        std::cerr << "{\"nativeSyncResourceInfoQueried\":true,\"ntstatus\":" << status << "}\n";
        if (status >= 0 || info.PrivateRuntimeDataSize || info.TotalPrivateDriverDataSize ||
            info.ResourcePrivateDriverDataSize || info.NumAllocations)
            throw std::runtime_error("Native fence unexpectedly described as a resource");
        return {status, 0, 0};
    }
    SyncResult importSync(std::uint32_t ticket, SharedSyncOpenDesc desc) override {
        const auto shared = sharedSyncHandles.find(ticket);
        if (!syncSharingEnabled || !runtime || runtime->hasStopped() || !validSharedSyncOpen(desc) ||
            shared == sharedSyncHandles.end() || !deviceAdapters.count(desc.device) || syncObjects.size() >= syncObjectLimit())
            return {{Invalid, 0, 0}, {0, 0}};
        if (!runtime->canMapFence()) return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0}};
        D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 a{};
        const auto status = shared->second.open(desc.device, desc.flags, desc.affinity, a);
        if (status) return {{status, 0, 0}, {0, 0}};
        Synchronization owned{desc.device, 5, desc.flags,
            static_cast<volatile std::uint64_t*>(a.MonitoredFence.FenceValueCPUVirtualAddress),
            a.MonitoredFence.FenceValueGPUVirtualAddress, std::nullopt};
        owned.shared = true;
        try {
            if (!a.hSyncObject || syncObjects.count(a.hSyncObject) || !owned.fence ||
                !validSyncGpuAddress({5, desc.flags, desc.affinity, 0, 0}, owned.gpuAddress))
                throw std::runtime_error("Unexpected imported native synchronization output");
            owned.lease = runtime->map(owned.fence);
            if (!syncObjects.emplace(a.hSyncObject, owned).second) throw std::runtime_error("Imported sync identity collision");
            peakSyncObjects=(std::max)(peakSyncObjects,syncObjects.size());
        } catch (...) {
            if (owned.lease) { try { runtime->unmap(*owned.lease); } catch (...) { runtime->stop(0); } }
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT cleanup{}; cleanup.hSyncObject = a.hSyncObject;
            if (D3DKMTDestroySynchronizationObject(&cleanup) < 0) ++cleanupFailures;
            throw;
        }
        ++completedSyncs; ++monitoredSyncs; ++importedSyncObjects;
        if (desc.flags & NoGpuAccessSyncFlag) ++noGpuAccessSyncs;
        std::cerr << "{\"nativeSyncHandleImported\":true,\"gpuMapped\":" << (owned.gpuAddress ? "true" : "false") << "}\n";
        return {{0, a.hSyncObject, 0}, {owned.lease->offset, owned.gpuAddress}};
    }
    SyncResult createSync(std::uint32_t device, SyncDesc desc) override {
        if (!syncEnabled || !runtime || !validSync(desc) || !deviceAdapters.count(device))
            return {{Invalid, 0, 0}, {0, 0}};
        if(syncObjects.size()>=syncObjectLimit()) {
            ++syncQuotaRejections;
            std::cerr<<"{\"nativeObjectAdmissionRejected\":true,\"kind\":\"sync\",\"live\":"<<syncObjects.size()<<",\"limit\":"<<syncObjectLimit()<<"}\n";
            return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0}};
        }
        if (desc.flags == NtSharedGpuSyncFlags && !syncSharingEnabled) return {{Invalid, 0, 0}, {0, 0}};
        if (desc.type == 5 && !runtime->canMapFence())
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0}};
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 a{}; a.hDevice = device;
        D3DDDI_SYNCHRONIZATIONOBJECT_FLAGS noMaxFlag{}; noMaxFlag.NoSignalMaxValueOnTdr = 1;
        if (noMaxFlag.Value != NoSignalMaxValueOnTdrSyncFlag) throw std::runtime_error("Native synchronization flag layout changed");
        D3DDDI_SYNCHRONIZATIONOBJECT_FLAGS sharedFlags{};
        sharedFlags.Shared = sharedFlags.NtSecuritySharing = sharedFlags.NoGPUAccess = 1;
        if (sharedFlags.Value != NtSharedNoGpuAccessSyncFlags) throw std::runtime_error("Native NT sharing flag layout changed");
        sharedFlags.UnwaitCpuWaitersOnlyOnDestroy = 1;
        if (sharedFlags.Value != (NtSharedNoGpuAccessSyncFlags | UnwaitCpuWaitersOnlyOnDestroySyncFlag))
            throw std::runtime_error("Native shared fence lifetime flag layout changed");
        a.Info.Type = static_cast<D3DDDI_SYNCHRONIZATIONOBJECT_TYPE>(desc.type);
        a.Info.Flags.Value = desc.flags;
        if (desc.type == 1) a.Info.SynchronizationMutex.InitialState = static_cast<BOOL>(desc.initial);
        else { a.Info.MonitoredFence.InitialFenceValue = desc.initial; a.Info.MonitoredFence.EngineAffinity = desc.affinity; }
        const auto status = D3DKMTCreateSynchronizationObject2(&a);
        if (status < 0) { ++failedSyncs; return {{status, 0, 0}, {0, 0}}; }
        Synchronization owned{device, desc.type, desc.flags, nullptr, 0, std::nullopt};
        owned.lastSignal = desc.initial;
        owned.shared = (desc.flags & 3) == 3;
        if (desc.type == 5) {
            owned.fence = static_cast<volatile std::uint64_t*>(a.Info.MonitoredFence.FenceValueCPUVirtualAddress);
            owned.gpuAddress = a.Info.MonitoredFence.FenceValueGPUVirtualAddress;
        }
        try {
            if (!a.hSyncObject || !syncObjects.emplace(a.hSyncObject, owned).second)
                throw std::runtime_error("Unexpected native synchronization identity");
            peakSyncObjects=(std::max)(peakSyncObjects,syncObjects.size());
        } catch (...) {
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT cleanup{}; cleanup.hSyncObject = a.hSyncObject;
            if (D3DKMTDestroySynchronizationObject(&cleanup) < 0) ++cleanupFailures;
            throw;
        }
        auto& retained = syncObjects.at(a.hSyncObject);
        try {
            if (status != 0 || a.Info.Type != static_cast<D3DDDI_SYNCHRONIZATIONOBJECT_TYPE>(desc.type) ||
                a.Info.Flags.Value != retained.flags || a.Info.SharedHandle ||
                (desc.type == 5 && (!retained.fence || !validSyncGpuAddress(desc, retained.gpuAddress))))
                throw std::runtime_error("Unexpected native synchronization output");
            if (desc.type == 5) retained.lease = runtime->map(retained.fence);
            ++completedSyncs;
            if (desc.flags & NoGpuAccessSyncFlag) ++noGpuAccessSyncs;
            if (desc.flags == NoSignalMaxValueOnTdrSyncFlag) ++noSignalMaxValueOnTdrSyncs;
            if (desc.type == 5) ++monitoredSyncs; else ++mutexSyncs;
            std::cerr << "{\"nativeSynchronizationCreated\":true,\"type\":" << desc.type
                      << ",\"flags\":" << desc.flags << ",\"globalSharedHandle\":false}\n";
            return {{status, a.hSyncObject, 0}, {retained.lease ? retained.lease->offset : 0, retained.gpuAddress}};
        } catch (...) {
            ++failedSyncs; runtime->stop(0); destroy(Kind::Sync, a.hSyncObject); throw;
        }
    }
    Result signalContextSync(std::uint32_t context, std::uint32_t sync, ContextSignalDesc desc) override {
        return signalContextSyncImpl(context, sync, desc, false);
    }
    Result queueContextSync(std::uint32_t context, std::uint32_t sync, ContextSignalDesc desc) override {
        return signalContextSyncImpl(context, sync, desc, true);
    }
    Result signalContextSyncImpl(std::uint32_t context, std::uint32_t sync, ContextSignalDesc desc, bool asynchronous) {
        if (!collectRetiredSubmissions()) return {Invalid, 0, 0};
        if (asynchronous && (!asyncSubmitEnabled || !syncSharingEnabled || pendingContextSignals.size() >= MaxContextSignals))
            return {Invalid, 0, 0};
        const auto owner = contextOwners.find(context); const auto object = syncObjects.find(sync);
        if (!submitEnabled || !syncEnabled || !runtime || runtime->hasStopped() || !validContextSignal(desc) ||
            owner == contextOwners.end() || !contextCanSignal(owner->second.flags) || object == syncObjects.end() ||
            object->second.device != owner->second.device || object->second.type != 5 || !packetSignalSyncFlags(object->second.flags) ||
            !object->second.lease || !object->second.fence || desc.fence <= object->second.lastSignal ||
            (streamingEnabled ? contextSignalAttempts + hwQueueSignalAttempts >= GuestStreamProfile.totalSignals :
                                !lifetimeBudgetAvailable(contextSignalAttempts,openglDesktopEnabled ? 0 : MaxContextSignals)))
            return {Invalid,0,0};
        D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 a{};
        a.ObjectCount = 1; a.ObjectHandleArray = &sync; a.Flags.Value = desc.flags;
        a.BroadcastContextCount = 1; a.BroadcastContextArray = &context; a.MonitoredFenceValueArray = &desc.fence;
        ++contextSignalAttempts;
        const auto status = D3DKMTSignalSynchronizationObjectFromGpu2(&a);
        std::cerr << "{\"nativeContextSignalStatus\":true,\"flags\":" << desc.flags << ",\"target\":" << desc.fence << ",\"ntstatus\":" << status
                  << ",\"contextFlags\":" << owner->second.flags << ",\"synchronizationOnly\":" << (owner->second.flags==8?"true":"false") << "}\n";
        if (status < 0) { ++failedContextSignals; return {status,0,0}; }
        object->second.lastSignal = desc.fence;
        if (status != 0) { ++failedContextSignals; throw std::runtime_error("Unexpected native context signal status"); }
        if (asynchronous) {
            pendingContextSignals.push_back({++acceptedContextSignals, context, sync, desc.fence});
            peakPendingContextSignals = (std::max)(peakPendingContextSignals, static_cast<unsigned>(pendingContextSignals.size()));
            MemoryBarrier(); const auto observed = *object->second.fence;
            std::cerr << "{\"nativeAsyncContextSignalAccepted\":true,\"index\":" << acceptedContextSignals
                      << ",\"target\":" << desc.fence << ",\"observed\":" << observed << ",\"cpuWait\":false}\n";
            if (!collectRetiredSubmissions()) throw std::runtime_error("Native context signal retirement failed");
            return {0, 0, observed};
        }
        const auto deadline = GetTickCount64() + 5000;
        std::uint64_t observed = 0;
        do {
            MemoryBarrier(); observed = *object->second.fence;
            if (observed >= desc.fence && observed != UINT64_MAX) {
                ++completedContextSignals;
                std::cerr << "{\"nativeContextSignalRetired\":true,\"target\":" << desc.fence << ",\"observed\":" << observed
                          << ",\"noGpuAccess\":true,\"cpuValueWrittenByBridge\":false}\n";
                return {0,0,observed};
            }
            if (observed == UINT64_MAX) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        ++failedContextSignals; ++contextSignalTimeouts;
        throw std::runtime_error("Native context signal retirement deadline exceeded");
    }
    Result createVendorAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        return createVendorAllocationImpl(device, desc, data, false);
    }
    VendorResourceResult createVendorResourceAllocation(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        const auto result = createVendorAllocationImpl(device, desc, data, true);
        return {result, result.ntstatus == 0 ? vendorAllocations.at(result.nativeHandle).resource : 0};
    }
    VendorResourceResult createSharedVendorResourceAllocation(std::uint32_t device, SharedVendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        if (!sharedResourcesEnabled || !validSharedVendorAllocation(desc)) return {{Invalid, 0, 0}, 0};
        const auto result = createVendorAllocationImpl(device, desc.allocation, data, true, desc.runtimeBytes);
        return {result, result.ntstatus == 0 ? vendorAllocations.at(result.nativeHandle).resource : 0};
    }
    NativeInteropCaps nativeInteropCaps() const override {
        return gpu_diagnostic::HighResolution && nativeMetadataEnabled && sharedResourcesEnabled &&
               allocationsEnabled && runtime && !runtime->hasStopped() ? NativeInteropProfile : NativeInteropCaps{};
    }
    NativeGpuBufferCaps nativeGpuBufferCaps() const override {
        return gpu_diagnostic::HighResolution && nativeMetadataEnabled && sharedResourcesEnabled &&
               allocationsEnabled && runtime && !runtime->hasStopped() ? NativeGpuBufferProfile : NativeGpuBufferCaps{};
    }
    Result queryNativeGpuBufferAdmission(NativeGpuBufferDesc desc) override {
        if (!validNativeGpuBufferCaps(nativeGpuBufferCaps()) || !validNativeGpuBuffer(desc)) return {Invalid, 0, 0};
        ++nativeGpuBufferAdmissionQueries;
        bool known = false; std::uint64_t usage = 0;
        // A necessary lower-bound check before the guest runtime starts its
        // allocation. It reserves no capacity; native callback guards remain
        // authoritative for actual requirements and concurrent usage changes.
        const bool fits = reportedGpuUsageAcceptable(desc.bytes, nullptr, &usage, &known);
        const auto status = !known ? static_cast<std::int32_t>(0xc0000001u) : fits ? 0 : static_cast<std::int32_t>(0xc0000017u);
        if (!known) ++failedNativeGpuBufferAdmissionQueries;
        else if (!fits) ++nativeGpuBufferAdmissionRefusals;
        std::cerr << "{\"nativeGpuBufferAdmissionQueried\":true,\"bytes\":" << desc.bytes << ",\"flags\":" << desc.flags
            << ",\"usageKnown\":" << (known ? "true" : "false") << ",\"usageBytes\":" << usage
            << ",\"limitBytes\":" << reportedGpuUsageLimit() << ",\"accepted\":" << (known && fits ? "true" : "false")
            << ",\"ntstatus\":" << status << ",\"advisoryOnly\":true,\"capacityReserved\":false,\"gpuObjectCreated\":false}\n";
        return {status, 0, 0};
    }
    VendorResourceResult createNativeSharedGpuBufferAllocation(std::uint32_t device, NativeSharedGpuBufferAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        if (!validNativeGpuBufferCaps(nativeGpuBufferCaps()) || !validNativeSharedGpuBufferAllocation(desc)) return {{Invalid, 0, 0}, 0};
        const auto result = createVendorAllocationImpl(device, desc.shared.allocation, data, true, desc.shared.runtimeBytes, nullptr, false, &desc.buffer);
        return {result, result.ntstatus == 0 ? vendorAllocations.at(result.nativeHandle).resource : 0};
    }
    VendorResourceResult createNativeInteropResource(std::uint32_t device, NativeSharedVendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        if(!validNativeInteropCaps(nativeInteropCaps()) || !validNativeInteropAllocation(desc)) return {{Invalid,0,0},0};
        const auto result=createVendorAllocationImpl(device,desc.shared.allocation,data,true,desc.shared.runtimeBytes,&desc.texture,true);
        return {result,result.ntstatus==0 ? vendorAllocations.at(result.nativeHandle).resource : 0};
    }
    VendorResourceResult createNativeSharedVendorResourceAllocation(std::uint32_t device, NativeSharedVendorAllocationDesc desc, std::vector<std::uint8_t>& data) override {
        if (!nativeMetadataEnabled || !validNativeSharedVendorAllocation(desc)) return {{Invalid, 0, 0}, 0};
        const auto result = createVendorAllocationImpl(device, desc.shared.allocation, data, true, desc.shared.runtimeBytes, &desc.texture);
        return {result, result.ntstatus == 0 ? vendorAllocations.at(result.nativeHandle).resource : 0};
    }
    Result createVendorAllocationImpl(std::uint32_t device, VendorAllocationDesc desc, std::vector<std::uint8_t>& data, bool withResource, std::uint32_t runtimeBytes = 0, const NativeTextureDesc* texture = nullptr, bool interop=false, const NativeGpuBufferDesc* bufferProfile = nullptr) {
        if (!allocationsEnabled || !validVendorAllocation(desc) || data.size() != desc.privateBytes + runtimeBytes ||
            (runtimeBytes && (!withResource || !sharedResourcesEnabled || !validSharedVendorAllocation({desc, runtimeBytes, 0})))) return {Invalid, 0, 0};
        if (!vendorObjectAdmission() || !reportedGpuUsageAcceptable())
            return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        D3DDDI_ALLOCATIONINFO2 info{}; info.Flags.Value = desc.flags; info.Priority = desc.priority;
        static_assert(UninitializedDisplaySource == D3DDDI_ID_UNINITIALIZED && D3DDDI_ID_NOTAPPLICABLE == 0,
                      "Display source sentinel contract changed");
        // Primary is forbidden by validVendorAllocation. VidPnSourceId only
        // identifies a source for primary surfaces; an unset guest value is
        // normalized to the documented not-applicable value here.
        info.VidPnSourceId = D3DDDI_ID_NOTAPPLICABLE;
        info.pPrivateDriverData = data.data(); info.PrivateDriverDataSize = desc.privateBytes;
        D3DKMT_CREATEALLOCATION a{}; a.hDevice = device; a.NumAllocations = 1; a.pAllocationInfo2 = &info;
        a.Flags.CreateResource = withResource ? 1u : 0u;
        if (runtimeBytes) {
            a.Flags.CreateShared = 1; a.Flags.NonSecure = 1; a.Flags.NtSecuritySharing = 1;
            a.pPrivateRuntimeData = data.data() + desc.privateBytes; a.PrivateRuntimeDataSize = runtimeBytes;
        }
        std::vector<std::uint8_t> nativeRuntime;
        // Keep the guest runtime bytes for a future FD import, independently
        // of the native allocation and of any Windows texture metadata.
        std::vector<std::uint8_t> guestRuntime;
        if (runtimeBytes) guestRuntime.assign(data.begin() + desc.privateBytes, data.end());
        if (texture) {
            if (!runtimeBytes || !nativeMetadataEnabled || (interop ? !validNativeInteropTexture(*texture) : !validNativeTexture(*texture))) return {Invalid, 0, 0};
            nativeRuntime = driver_metadata::generate(luid, *texture,interop);
            a.pPrivateRuntimeData = nativeRuntime.data(); a.PrivateRuntimeDataSize = static_cast<UINT>(nativeRuntime.size());
        }
        if (bufferProfile) {
            if (texture || !runtimeBytes || !validNativeGpuBufferCaps(nativeGpuBufferCaps()) || !validNativeGpuBuffer(*bufferProfile)) return {Invalid, 0, 0};
            auto attempt = driver_metadata::attemptBufferMetadata([&] {
                return driver_metadata::generateBuffer(luid, *bufferProfile, [this](std::uint64_t bytes) {
                    bool known = false;
                    const bool fits = reportedGpuUsageAcceptable(bytes, nullptr, nullptr, &known);
                    return !known ? driver_metadata::BudgetDecision::Unavailable : fits ?
                        driver_metadata::BudgetDecision::Accepted : driver_metadata::BudgetDecision::QuotaRefused;
                });
            });
            if (attempt.failure != driver_metadata::MemoryFailure::None) {
                ++nativeGpuBufferMetadataMemoryRefusals;
                std::cerr << "{\"nativeGpuBufferMetadataMemoryRefused\":true,\"reason\":\"" << driver_metadata::memoryFailureName(attempt.failure)
                    << "\",\"bytes\":" << bufferProfile->bytes << ",\"flags\":" << bufferProfile->flags
                    << ",\"originalNativeCode\":" << attempt.nativeCode
                    << ",\"ntstatus\":" << static_cast<std::int32_t>(0xc0000017u)
                    << ",\"temporaryOwnershipUnwound\":true,\"allocationPublished\":false,\"guestRuntimePreserved\":true}\n";
                return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
            }
            nativeRuntime = std::move(attempt.bytes);
            a.pPrivateRuntimeData = nativeRuntime.data(); a.PrivateRuntimeDataSize = static_cast<UINT>(nativeRuntime.size());
        }
        auto status = D3DKMTCreateAllocation2(&a);
        if (bufferProfile) std::cerr << "{\"nativeGpuBufferMetadataApplied\":true,\"bytes\":" << bufferProfile->bytes
            << ",\"flags\":" << bufferProfile->flags << ",\"guestRuntimeBytes\":" << runtimeBytes
            << ",\"nativeRuntimeBytes\":" << nativeRuntime.size() << ",\"guestRuntimePreserved\":true,\"driverPrivateDataReused\":true,\"ntstatus\":" << status << "}\n";
        if (texture) std::cerr << "{\"nativeTextureMetadataApplied\":true,\"typedTexture\":true,\"guestRuntimeBytes\":" << runtimeBytes
            << ",\"nativeRuntimeBytes\":" << nativeRuntime.size() << ",\"guestRuntimePreserved\":true,\"driverPrivateDataReused\":true,\"ntstatus\":" << status << "}\n";
        if (desc.source == UninitializedDisplaySource)
            std::cerr << "{\"vendorAllocationSourceNormalization\":true,\"inputSource\":" << desc.source
                      << ",\"nativeSource\":" << info.VidPnSourceId << ",\"primary\":false,\"ntstatus\":" << status << "}\n";
        if (status < 0) { ++failedVendorAllocations; return {status, 0, 0}; }
        if (!info.hAllocation || status != 0 || (a.hResource != 0) != withResource || a.hResource == info.hAllocation ||
            a.hGlobalShare || info.GpuVirtualAddress % 4096 || info.PrivateDriverDataSize != desc.privateBytes) {
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device;
            release.hResource = a.hResource; release.Flags.SynchronousDestroy = 1;
            if (!a.hResource) { release.phAllocationList = &info.hAllocation; release.AllocationCount = 1; }
            if (D3DKMTDestroyAllocation2(&release) < 0) ++cleanupFailures;
            throw std::runtime_error("Unexpected native vendor-allocation result");
        }
        if (bufferProfile && !probeSharedResource(a.hResource, a.PrivateRuntimeDataSize, bufferProfile)) {
            // This allocation has never been returned or submitted. Retire it
            // synchronously before publishing any guest/native ownership record.
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device; release.hResource = a.hResource;
            release.Flags.SynchronousDestroy = 1;
            const auto releaseStatus = D3DKMTDestroyAllocation2(&release);
            std::cerr << "{\"nativeGpuBufferAdmissionRejected\":true,\"unpublished\":true,\"destroyNtstatus\":" << releaseStatus << "}\n";
            if (releaseStatus) { ++cleanupFailures; throw std::runtime_error("Cannot retire rejected typed GPU buffer"); }
            ++failedVendorAllocations; return {Invalid, 0, 0};
        }
        try {
            VendorAllocation owned{}; owned.device = device; owned.address = info.GpuVirtualAddress; owned.resource = a.hResource;
            vendorAllocations.emplace(info.hAllocation, owned);
            if (runtimeBytes) resourceGuestRuntime.emplace(a.hResource, std::move(guestRuntime));
        }
        catch (...) {
            vendorAllocations.erase(info.hAllocation);
            resourceGuestRuntime.erase(a.hResource);
            D3DKMT_DESTROYALLOCATION2 release{}; release.hDevice = device; release.phAllocationList = &info.hAllocation;
            release.AllocationCount = 1; release.Flags.SynchronousDestroy = 1;
            if (a.hResource) { release.hResource = a.hResource; release.phAllocationList = nullptr; release.AllocationCount = 0; }
            if (D3DKMTDestroyAllocation2(&release) < 0) ++cleanupFailures;
            throw;
        }
        if (!reportedGpuUsageAcceptable()) {
            const auto released = destroyVendorAllocations(device, {info.hAllocation});
            if (released.ntstatus < 0) throw std::runtime_error("Cannot release vendor allocation after reported-usage limit");
            ++failedVendorAllocations; return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        }
        ++completedVendorAllocations;
        if (withResource) {
            ++completedVendorResources;
            if (runtimeBytes) {
                const bool buffer = bufferProfile || probeSharedResource(a.hResource, a.PrivateRuntimeDataSize);
                if (texture || buffer) sharedResourceIds.insert(a.hResource);
            }
            std::cerr << "{\"nativeVendorResourceCreated\":true,\"allocationCount\":1,\"shared\":" << (runtimeBytes ? "true" : "false")
                      << ",\"systemMemory\":false,\"ntstatus\":" << status << "}\n";
        }
        if (desc.source == UninitializedDisplaySource) ++uninitializedSourceAllocations;
        peakVendorAllocationObjects = (std::max)(peakVendorAllocationObjects, vendorAllocations.size());
        return {status, info.hAllocation, info.GpuVirtualAddress};
    }
    Result destroyVendorAllocations(std::uint32_t device, const std::vector<std::uint32_t>& handles) override {
        if (!allocationsEnabled || handles.empty() || handles.size() > MaxVendorAllocations) return {Invalid, 0, 0};
        unsigned ownedQueues = 0;
        for (const auto& item : hwQueues) {
            const auto& queue = item.second;
            if (queue.device != device) continue;
            // EOF cleanup must retain allocations if queue destruction failed.
            if (!retirementEnabled || !runtime || runtime->hasStopped() || !queue.fence) return {Invalid, 0, 0};
            MemoryBarrier();
            const auto observed = *queue.fence;
            if (observed == UINT64_MAX || observed < queue.submittedFence) return {Invalid, 0, 0};
            ++ownedQueues;
        }
        for (std::size_t n = 0; n < handles.size(); ++n) {
            const auto found = vendorAllocations.find(handles[n]);
            if (found == vendorAllocations.end() || found->second.device != device ||
                std::find(handles.begin(), handles.begin() + n, handles[n]) != handles.begin() + n) return {Invalid, 0, 0};
            if (found->second.resource && handles.size() != 1) return {Invalid, 0, 0};
        }
        // Explicit protocol destruction rejects locked objects. EOF cleanup
        // reaches this path only after the server has reaped its owned VM.
        for (const auto handle : handles) {
            if (vendorAllocations.at(handle).locked) {
                const auto released = unlockVendorAllocation(handle, device);
                if (released.ntstatus < 0) return released;
            }
        }
        D3DKMT_DESTROYALLOCATION2 a{}; a.hDevice = device; a.phAllocationList = handles.data();
        // AssumeNotInUse remains zero: VidMm retains memory needed by earlier
        // commands. SynchronousDestroy confirms reclamation before reuse.
        a.AllocationCount = static_cast<UINT>(handles.size()); a.Flags.SynchronousDestroy = 1;
        const auto resource = vendorAllocations.at(handles.front()).resource;
        if (resource) { a.hResource = resource; a.phAllocationList = nullptr; a.AllocationCount = 0; }
        const auto consumer=textureConsumers.find(resource);
        if (resource && consumer!=textureConsumers.end()) {
            const bool retired=!consumer->second.worker || consumer->second.worker->stop();
            textureConsumers.erase(consumer); ++releasedTextureConsumers;
            std::cerr << "{\"nativeSharedTextureConsumerReleased\":true,\"gpuWorkRetired\":"<<(retired?"true":"false")<<"}\n";
        }
        const auto status = D3DKMTDestroyAllocation2(&a);
        if (status > 0) throw std::runtime_error("Unexpected native allocation-destruction status");
        if (status == 0) {
            if (resource) { ++destroyedVendorResources; sharedResourceIds.erase(resource); resourceGuestRuntime.erase(resource); }
            for (const auto handle : handles) {
                vendorMappedPages -= static_cast<std::uint32_t>(vendorAllocations.at(handle).pages);
                vendorResidencyAttempts -= vendorAllocations.at(handle).residencyAttempts;
                if(deferredResidencyEnabled && vendorAllocations.at(handle).residency.retained) {
                    const auto references=vendorAllocations.at(handle).residency.retained;
                    std::cerr<<"{\"nativeDeferredResidencyDestroyed\":true,\"references\":"<<references
                             <<",\"retainedBefore\":"<<retainedResidencyReferences<<",\"retainedAfter\":"<<(retainedResidencyReferences-references)
                             <<",\"synchronousNativeDestroy\":true}\n";
                    destroyedRetainedResidencyReferences+=references;
                }
                retainedResidencyReferences -= vendorAllocations.at(handle).residency.retained;
                translatedAllocations.erase(handle);
                vendorAllocations.erase(handle);
            }
            destroyedVendorAllocations += static_cast<unsigned>(handles.size());
            if (ownedQueues) vendorDestructionsWithHwQueues += static_cast<unsigned>(handles.size());
        } else ++cleanupFailures;
        return {status, 0, 0};
    }
    Result captureCompositorResource(std::uint32_t resource, std::uint32_t sync, CompositorCaptureDesc desc) override {
        const auto source = sharedResourceHandles.find(resource);
        const auto fence = sharedSyncHandles.find(sync);
        if (compositorOutput || !consumeSharedEnabled || !openglRenderEnabled || !runtime || runtime->hasStopped() ||
            !validNativeInteropCaps(nativeInteropCaps()) || !validCompositorCapture(desc) || desc.syncTicket != sync ||
            source == sharedResourceHandles.end() || fence == sharedSyncHandles.end() || compositorCaptureAttempts >= 2)
            return {Invalid,0,0};
        const auto frame = ++compositorCaptureAttempts;
        std::uint64_t hash = 0; HRESULT hr = E_FAIL;
        if (presentSharedEnabled) {
            if (!compositorPresenter) compositorPresenter = std::make_unique<driver_shared::CompositorReadback>();
            hr = compositorPresenter->capture(source->second.handle.get(), fence->second.get(), luid, desc, frame, hash, true);
        } else {
            driver_shared::CompositorReadback capture;
            hr = capture.capture(source->second.handle.get(), fence->second.get(), luid, desc, frame, hash);
        }
        if (FAILED(hr)) {
            ++failedCompositorCaptures;
            std::cerr << "{\"nativeCompositorCaptureFailed\":true,\"frame\":" << frame << ",\"hresult\":" << hr << "}\n";
            return {static_cast<std::int32_t>(0xc0000001u),0,0};
        }
        ++completedCompositorCaptures;
        return {0,0,hash};
    }
    Result queueCompositorFrame(std::uint32_t resource,std::uint32_t sync,CompositorCaptureDesc desc) override {
        const auto source=sharedResourceHandles.find(resource);
        const auto fence=sharedSyncHandles.find(sync);
        if (compositorCaptureAttempts || !consumeSharedEnabled || !presentSharedEnabled || !openglRenderEnabled ||
            !runtime || runtime->hasStopped() || !validNativeInteropCaps(nativeInteropCaps()) ||
            !validCompositorFrame(desc) || desc.syncTicket!=sync || source==sharedResourceHandles.end() || fence==sharedSyncHandles.end())
            return {Invalid,0,0};
        HRESULT hr=S_OK; std::uint64_t frame=0;
        if (!compositorOutput) {
            compositorOutput=std::make_unique<driver_shared::CompositorPresenter>();
            hr=compositorOutput->initialize(luid,desc,[this](const char* stage,std::uint64_t bytes) {
                return reportedGpuUsageAcceptable(bytes,stage);
            },coordinatedObjectsEnabled && coordinatedObjectCapacity.version>=5 && (openGlProfileVersion==13 || openGlProfileVersion==14),runtime->inputChannel(),runtime->inputSelfTest(),runtime->sdlControl());
        }
        if (SUCCEEDED(hr)) hr=compositorOutput->submit(resource,source->second.handle.get(),fence->second.get(),desc,frame);
        if (FAILED(hr)) {
            const bool retry=hr==HRESULT_FROM_WIN32(ERROR_RETRY);
            if (retry) ++compositorBackpressure; else ++failedCompositorFrames;
            std::cerr<<"{\"nativeCompositorQueueRejected\":true,\"hresult\":"<<hr<<",\"retryable\":"<<(retry?"true":"false")<<"}\n";
            return {static_cast<std::int32_t>(retry?0xc000022du:0xc0000001u),0,0};
        }
        ++completedCompositorFrames;
        return {0,0,frame};
    }
    Result consumeSharedTexture(std::uint32_t resource, std::uint32_t sync, SharedTextureConsumeDesc desc) override {
        if (streamingEnabled) return {Invalid,0,0};
        return consumeTexture(resource, sync, desc, false);
    }
    Result consumeStreamingTexture(std::uint32_t resource, std::uint32_t sync, SharedTextureConsumeDesc desc) override {
        if (!streamingEnabled || desc.fence != completedSharedConsumes + 1) return {Invalid,0,0};
        return consumeTexture(resource, sync, desc, true);
    }
    Result consumeTexture(std::uint32_t resource, std::uint32_t sync, SharedTextureConsumeDesc desc, bool streaming) {
        if (!collectRetiredSubmissions()) return {Invalid,0,0};
        const auto signal = syncObjects.find(sync);
        const auto allocation = std::find_if(vendorAllocations.begin(), vendorAllocations.end(),
            [&](const auto& item) { return item.second.resource == resource; });
        if (!consumeSharedEnabled || !runtime || runtime->hasStopped() || !validSharedTextureConsume(desc) || desc.sync != sync ||
            !sharedResourceIds.count(resource) || allocation == vendorAllocations.end() || signal == syncObjects.end() ||
            signal->second.device != allocation->second.device || signal->second.type != 5 ||
            signal->second.flags != NoGpuAccessSyncFlag || !signal->second.fence || signal->second.lastSignal != desc.fence ||
            !pendingSubmissions.empty() || !pendingHwSignals.empty() || completedSharedConsumes >= (streaming ? GuestStreamProfile.frames : 2)) return {Invalid,0,0};
        MemoryBarrier(); const auto observed = *signal->second.fence;
        if (observed < desc.fence || observed == UINT64_MAX) return {Invalid,0,0};
        auto entry = textureConsumers.find(resource);
        if (entry == textureConsumers.end()) {
            ConsumerLease consumer{}; consumer.sync=sync;
            HRESULT hr=E_FAIL;
            if (consumeSharedProcessEnabled) {
                consumer.worker=std::make_unique<driver_worker::TextureWorker>(nativeWorkers);
                hr=consumer.worker->initialize(resource,luid,presentSharedEnabled);
            } else {
                consumer.gpu=std::make_unique<driver_shared::TextureConsumer>();
                hr=consumer.gpu->initialize(resource,luid,presentSharedEnabled);
            }
            if (FAILED(hr)) {
                ++failedSharedConsumes;
                std::cerr << "{\"nativeSharedTextureConsumeFailed\":true,\"stage\":\"initialize\",\"hresult\":" << hr << "}\n";
                return {static_cast<std::int32_t>(0xc0000001u),0,0};
            }
            entry = textureConsumers.emplace(resource,std::move(consumer)).first;
            ++openedTextureConsumers;
        }
        if (entry->second.sync != sync || desc.fence <= entry->second.lastFence) return {Invalid,0,0};
        std::uint64_t hash = 0;
        const auto hr = entry->second.worker ? entry->second.worker->consume(hash,streaming) : entry->second.gpu->consume(hash,streaming);
        if (FAILED(hr)) { ++failedSharedConsumes; return {static_cast<std::int32_t>(0xc0000001u),0,0}; }
        entry->second.lastFence = desc.fence; ++completedSharedConsumes;
        std::cerr << "{\"nativeSharedTextureHandoff\":true,\"target\":" << desc.fence << ",\"observed\":" << observed
                  << ",\"nativeSignalAccepted\":true,\"sourceState\":\"COMMON\",\"returnedState\":\"COMMON\",\"pendingGuestCommands\":0}\n";
        return {0,0,hash};
    }
    void configureDeferredResidency(bool enabled) {
        if(enabled && (!runtime || !residencyEnabled || !retirementEnabled || !asyncSubmitEnabled))
            throw std::runtime_error("Deferred residency native prerequisites absent");
        deferredResidencyEnabled=enabled;
    }
    void configureMappedResidencyAdmission(bool enabled) {
        if(enabled && !deferredResidencyEnabled) throw std::runtime_error("Mapped admission requires retaining residency");
        mappedResidencyAdmissionEnabled=enabled;
    }
    void configureApplicationMemory(bool permitted) {
        if(permitted && !mappedResidencyAdmissionEnabled)
            throw std::runtime_error("Application memory requires mapped residency admission");
        applicationMemoryPermitted=permitted;
        std::cerr<<"{\"nativeApplicationMemoryOwnerConfiguration\":true,\"permitted\":"<<(permitted?"true":"false")
            <<",\"selected\":false,\"legacyReportedBytes\":"<<OpenGlDesktopPresentationReportedBytes
            <<",\"ordinaryClientCanSelect\":false}\n";
    }
    void configureApplicationObjects(bool permitted) {
        if(permitted && !applicationMemoryPermitted)
            throw std::runtime_error("Application objects require application memory permission");
        applicationObjectsPermitted=permitted;
    }
    bool beginApplicationMemory(ApplicationMemoryProfile profile) override {
        static_assert(ApplicationPresentationReserve==driver_shared::ResidencyStagingReserve,"Shared presentation headroom");
        if(!applicationMemoryPermitted || applicationMemoryEnabled || !mappedResidencyAdmissionEnabled ||
           !deferredResidencyEnabled || !runtime || openGlProfileVersion || !ownedAdapters.empty() ||
           !validApplicationMemoryProfile(profile) || !allocationBudgetAdapter) return false;
        std::array<std::uint64_t,2> budgets{},usage{};
        for(unsigned n=0;n<2;++n) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if(FAILED(allocationBudgetAdapter->QueryVideoMemoryInfo(0,static_cast<DXGI_MEMORY_SEGMENT_GROUP>(n),&info))) return false;
            budgets[n]=info.Budget;usage[n]=info.CurrentUsage;
        }
        const bool available=applicationMemoryNativeBudgetAvailable(budgets,usage);
        const auto shares=applicationMemoryPartition();
        std::cerr<<"{\"nativeApplicationMemoryNativeBudgetAdmission\":true,\"accepted\":"<<(available?"true":"false")
            <<",\"localNativeBudgetBytes\":"<<budgets[0]<<",\"nonlocalNativeBudgetBytes\":"<<budgets[1]
            <<",\"localUsageBytes\":"<<usage[0]<<",\"nonlocalUsageBytes\":"<<usage[1]
            <<",\"localApplicationShareBytes\":"<<shares.bytes[0]<<",\"nonlocalApplicationShareBytes\":"<<shares.bytes[1]
            <<",\"presentationReserveBytes\":"<<ApplicationPresentationReserve<<",\"aggregateReportedBytes\":"<<profile.reportedBytes
            <<",\"requiresAtMostHalfNativeAvailableBudget\":true,\"physicalPartition\":false}\n";
        if(!available) return false;
        applicationMemoryEnabled=true;return true;
    }
    bool retainsMappedResidencyOnEvict() const override { return deferredResidencyEnabled; }
    bool allowsReservationBackedMappings() const override {
        return deferredResidencyEnabled && coordinatedObjectsEnabled && coordinatedObjectCapacity.version>=6;
    }
    void reclaimDeferredResidency(const std::vector<std::uint32_t>& keep={}) {
        if(!deferredResidencyEnabled || !retainedResidencyReferences) return;
        if(!collectRetiredSubmissions()) throw std::runtime_error("Deferred residency cannot establish GPU retirement");
        if(!pendingSubmissions.empty() || !pendingHwSignals.empty() || !pendingHwWaits.empty() || !pendingContextSignals.empty()) return;
        for(;;) {
            std::vector<std::uint32_t> handles;
            std::uint32_t device=0;
            for(const auto& item:vendorAllocations) {
                const auto& allocation=item.second;
                if(!allocation.residency.canRelease(allocation.locked || allocation.cpuLease.has_value() || allocation.cpuBytes,true) ||
                    std::find(keep.begin(),keep.end(),item.first)!=keep.end() || (device && device!=allocation.device)) continue;
                device=allocation.device;handles.push_back(item.first);
                if(handles.size()==MaxVendorAllocations) break;
            }
            if(handles.empty()) return;
            D3DKMT_EVICT value{};value.hDevice=device;value.NumAllocations=static_cast<UINT>(handles.size());
            value.AllocationList=handles.data(); // Unconditional reclamation of already relinquished guest references.
            ++attemptedVendorEvictions;
            const auto status=D3DKMTEvict(&value);
            std::cerr<<"{\"nativeDeferredResidencyReclaimed\":true,\"count\":"<<handles.size()<<",\"ntstatus\":"<<status
                     <<",\"cpuMapped\":false,\"allGpuWorkRetired\":true,\"nativeCall\":true"
                     <<",\"retainedBefore\":"<<retainedResidencyReferences
                     <<",\"retainedAfter\":"<<(status==0?retainedResidencyReferences-handles.size():retainedResidencyReferences)<<"}\n";
            if(status!=0) {
                ++failedVendorEvictions;
                for(const auto handle:handles) vendorAllocations.at(handle).residency.uncertain();
                throw std::runtime_error("Deferred native eviction has uncertain ownership; retain until VM exit");
            }
            ++completedVendorEvictions;
            for(const auto handle:handles) {
                if(!vendorAllocations.at(handle).residency.release(false,true) || !retainedResidencyReferences)
                    throw std::runtime_error("Deferred residency ownership accounting mismatch");
                --retainedResidencyReferences;++reclaimedResidencyReferences;
                auto& allocation=vendorAllocations.at(handle);
                if(!allocation.residencyAttempts || !vendorResidencyAttempts)
                    throw std::runtime_error("Deferred residency native reference charge mismatch");
                --allocation.residencyAttempts;--vendorResidencyAttempts;
            }
        }
    }
    ResidentResult makeDeferredVendorResident(std::uint32_t queue,ResidentDesc desc,
            const std::vector<std::uint32_t>& handles,const std::vector<std::uint32_t>& priorities) {
        std::vector<std::uint32_t> native,nativePriorities;
        for(std::size_t n=0;n<handles.size();++n) {
            const auto& refs=vendorAllocations.at(handles[n]).residency;
            if(!refs.known || (!refs.canReuse() && (!refs.canAdmit() || refs.native()>=MaxVendorResidencyAttempts)))
                return {{Invalid,0,0},{0,0,0}};
            if(!refs.canReuse()) {
                native.push_back(handles[n]);
                if(!priorities.empty()) nativePriorities.push_back(priorities[n]);
            }
        }
        // Preserve requested retained references so repeated MakeResident can
        // transfer them back without adding a native reference or paging work.
        reclaimDeferredResidency(handles);
        std::uint64_t usageBefore=0;
        if(!reportedGpuUsageAcceptable(0,nullptr,&usageBefore)) return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0,0}};
        const auto reclaimTarget=driver_shared::stagingPressureTarget(usageBefore,reportedGpuUsageLimit());
        if(compositorOutput && reclaimTarget) {
            std::uint64_t released=0;
            if(FAILED(compositorOutput->reclaimStagingForResidency(reclaimTarget,released)))
                throw std::runtime_error("Cannot reclaim presentation staging before deferred residency");
        }
        if(mappedResidencyAdmissionEnabled) {
            if(!coordinatedObjectsEnabled || (coordinatedObjectCapacity.version!=9 && coordinatedObjectCapacity.version!=10) || !openglDesktopPresentationEnabled)
                throw std::runtime_error("Mapped admission requires the aggregate application desktop");
            // Recheck after possible staging reclamation. This is still before
            // native reference increments, paging submission or logical commit.
            if(!reportedGpuUsageAcceptable(0,nullptr,&usageBefore))
                return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0,0}};
            std::vector<driver_residency::MappedCandidate> candidates;
            candidates.reserve(native.size());
            for(const auto handle:native) {
                const auto& allocation=vendorAllocations.at(handle);
                candidates.push_back({allocation.pages,allocation.residency.native(),
                    allocation.locked || allocation.cpuLease.has_value() || allocation.cpuBytes!=0});
            }
            const auto plan=driver_residency::mappedAdmission(usageBefore,reportedGpuUsageLimit(),candidates.data(),candidates.size());
            if(!plan.valid) return {{Invalid,0,0},{0,0,0}};
            if(!plan.fits) {
                ++failedVendorResidency;
                std::cerr<<"{\"nativeMappedResidencyAdmissionRefused\":true,\"usageBytes\":"<<usageBefore
                    <<",\"limitBytes\":"<<reportedGpuUsageLimit()<<",\"mappedExtentEstimateBytes\":"<<plan.estimateBytes
                    <<",\"bytesToTrimHint\":"<<plan.bytesToTrim<<",\"requestedAllocations\":"<<handles.size()
                    <<",\"nativeCandidates\":"<<native.size()<<",\"estimatedCandidates\":"<<plan.estimated
                    <<",\"alreadyReferencedCandidates\":"<<plan.alreadyReferenced<<",\"unknownCandidates\":"<<plan.unknown
                    <<",\"cpuMappedCandidates\":"<<plan.cpuMapped<<",\"requestFlags\":"<<desc.flags
                    <<",\"nativeMakeResidentCalled\":false,\"requestedLogicalOwnershipChanged\":false,\"requestedNativeReferencesChanged\":false"
                    <<",\"mappingExtentIsPhysicalSize\":false,\"ownerOptIn\":true}\n";
                return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0,plan.bytesToTrim}};
            }
        }
        D3DDDI_MAKERESIDENT value{};std::int32_t status=0;
        if(!native.empty()) {
            value.hPagingQueue=queue;value.NumAllocations=static_cast<UINT>(native.size());
            value.AllocationList=native.data();value.PriorityList=nativePriorities.empty()?nullptr:nativePriorities.data();value.Flags.Value=desc.flags;
            for(const auto handle:native) ++vendorAllocations.at(handle).residencyAttempts;
            vendorResidencyAttempts+=static_cast<std::uint32_t>(native.size());
            peakVendorResidencyAttempts=(std::max)(peakVendorResidencyAttempts,vendorResidencyAttempts);
            status=D3DKMTMakeResident(&value);
            if((status!=0 && status!=0x103) || value.NumAllocations!=native.size()) {
                ++failedVendorResidency;
                for(const auto handle:native) vendorAllocations.at(handle).residency.uncertain();
                std::cerr<<"{\"nativeDeferredResidencyAdmissionRejected\":true,\"requested\":"<<native.size()
                         <<",\"returned\":"<<value.NumAllocations<<",\"ntstatus\":"<<status<<",\"ownershipUncertain\":true}\n";
                throw std::runtime_error("Deferred residency native list did not complete; retain until VM exit");
            }
            if(!waitPaging(queue,value.PagingFenceValue)) throw std::runtime_error("Deferred residency paging deadline exceeded");
            ++completedVendorResidencyWaits;
        }
        std::uint64_t usageAfter=0;bool usageAfterKnown=false;
        if(!reportedGpuUsageAcceptable(0,nullptr,&usageAfter,&usageAfterKnown)) {
            std::uint64_t requestedMappedPages=0,nativeMappedPages=0,guestReferences=0,retainedReferences=0;
            unsigned cpuMapped=0,mapped=0,unmapped=0;
            for(const auto handle:handles) {
                const auto& allocation=vendorAllocations.at(handle);
                requestedMappedPages+=allocation.pages;guestReferences+=allocation.residency.guest;
                retainedReferences+=allocation.residency.retained;
                if(allocation.pages) ++mapped;else ++unmapped;
                if(allocation.locked || allocation.cpuLease.has_value() || allocation.cpuBytes) ++cpuMapped;
            }
            for(const auto handle:native) nativeMappedPages+=vendorAllocations.at(handle).pages;
            std::cerr<<"{\"nativeDeferredResidencyBudgetObservation\":true,\"usageBeforeBytes\":"<<usageBefore
                <<",\"usageAfterBytes\":"<<usageAfter<<",\"usageAfterKnown\":"<<(usageAfterKnown?"true":"false")<<",\"limitBytes\":"<<reportedGpuUsageLimit()
                <<",\"requestedAllocations\":"<<handles.size()<<",\"nativeAllocations\":"<<native.size()
                <<",\"reusedAllocations\":"<<(handles.size()-native.size())<<",\"nativeReturnedAllocations\":"<<value.NumAllocations
                <<",\"nativeStatus\":"<<status<<",\"pagingFence\":"<<value.PagingFenceValue
                <<",\"nativePagingWaitCompleted\":"<<(!native.empty()?"true":"false")
                <<",\"requestedMappedPages\":"<<requestedMappedPages<<",\"nativeMappedPages\":"<<nativeMappedPages
                <<",\"mappedAllocations\":"<<mapped<<",\"unmappedAllocations\":"<<unmapped<<",\"cpuMappedAllocations\":"<<cpuMapped
                <<",\"guestReferencesBeforeCommit\":"<<guestReferences<<",\"retainedReferencesBeforeCommit\":"<<retainedReferences
                <<",\"logicalOwnershipCommitted\":false,\"mappingExtentIsPhysicalSize\":false,\"policyChanged\":false}\n";
            throw std::runtime_error("Reported GPU usage limit after deferred residency");
        }
        for(const auto handle:handles) {
            auto& allocation=vendorAllocations.at(handle);
            if(allocation.residency.canReuse()) {
                if(!allocation.residency.reuse() || !retainedResidencyReferences) throw std::runtime_error("Deferred residency reuse mismatch");
                --retainedResidencyReferences;++reusedResidencyReferences;
            } else if(!allocation.residency.admit()) throw std::runtime_error("Deferred residency admission mismatch");
            allocation.resident=true;
        }
        ++completedVendorResidency;vendorAllocationsMadeResident+=handles.size();
        std::cerr<<"{\"nativeDeferredResidencyAdmitted\":true,\"requested\":"<<handles.size()<<",\"nativeCount\":"<<native.size()
                 <<",\"reusedCount\":"<<(handles.size()-native.size())<<",\"retainedReferences\":"<<retainedResidencyReferences<<"}\n";
        return {{status,0,value.PagingFenceValue},{desc.count,0,0}};
    }
    MemoryBudgetResult queryMemoryBudget(std::uint32_t adapter, MemoryBudgetDesc desc) override {
        if (!residencyEnabled || !ownedAdapters.count(adapter) || !validMemoryBudget(desc)) return {{Invalid,0,0},{}};
        reclaimDeferredResidency();
        D3DKMT_QUERYVIDEOMEMORYINFO value{}; value.hAdapter=adapter;
        value.MemorySegmentGroup=static_cast<D3DKMT_MEMORY_SEGMENT_GROUP>(desc.group);
        value.PhysicalAdapterIndex=desc.physicalAdapter;
        // NULL selects this owned Windows worker. No guest process HANDLE
        // or process identifier is forwarded across the boundary.
        const auto status=D3DKMTQueryVideoMemoryInfo(&value);
        if (status!=0) { ++failedMemoryBudgetQueries; return {{status>0?Invalid:status,0,0},{}}; }
        auto segmentLimit=reportedGpuUsageLimit();
        driver_budget::Partition shares{};
        if(deferredResidencyEnabled) {
            shares=applicationMemoryEnabled ? applicationMemoryPartition() :
                driver_budget::partition(description.DedicatedVideoMemory,description.SharedSystemMemory,
                    reportedGpuUsageLimit(),driver_shared::ResidencyStagingReserve);
            if(!shares.valid) return {{Invalid,0,0},{}};
            segmentLimit=shares.bytes[desc.group];
        }
        const auto budget=(std::min)(value.Budget,segmentLimit);
        const MemoryBudgetReply info{budget,value.CurrentUsage,value.CurrentReservation,
            (std::min)(value.AvailableForReservation,budget)};
        ++completedMemoryBudgetQueries;
        std::cerr<<"{\"nativeMemoryBudgetQueried\":true,\"group\":"<<desc.group<<",\"nativeBudget\":"<<value.Budget
                 <<",\"budget\":"<<info.budget<<",\"usage\":"<<info.usage<<",\"reservation\":"<<info.reservation
                 <<",\"available\":"<<info.available<<",\"currentProcessOnly\":true,\"physicalAdapter\":0"
                 <<",\"sharedAggregateBudget\":"<<(deferredResidencyEnabled?"true":"false");
        if(deferredResidencyEnabled) std::cerr<<",\"aggregateLimitBytes\":"<<reportedGpuUsageLimit()
                 <<",\"presentationReserveBytes\":"<<driver_shared::ResidencyStagingReserve
                 <<",\"localBudgetShareBytes\":"<<shares.bytes[0]<<",\"nonlocalBudgetShareBytes\":"<<shares.bytes[1];
        std::cerr<<"}\n";
        return {{0,0,0},info};
    }
    EvictResult evictVendorAllocations(std::uint32_t device, EvictDesc desc, const std::vector<std::uint32_t>& handles) override {
        if (!residencyEnabled || !deviceAdapters.count(device) || !validEvict(desc) || handles.size()!=desc.count)
            return {{Invalid,0,0},false};
        for (std::size_t n=0;n<handles.size();++n) {
            const auto found=vendorAllocations.find(handles[n]);
            if (found==vendorAllocations.end() || found->second.device!=device ||
                std::find(handles.begin(),handles.begin()+n,handles[n])!=handles.begin()+n ||
                (!deferredResidencyEnabled && found->second.locked && found->second.residency.guest<=1) ||
                (deferredResidencyEnabled && !found->second.residency.canDefer())) return {{Invalid,0,0},false};
        }
        if(deferredResidencyEnabled) {
            for(const auto handle:handles) {
                auto& allocation=vendorAllocations.at(handle);
                if(!allocation.residency.defer()) throw std::runtime_error("Deferred eviction ownership mismatch");
                allocation.resident=allocation.residency.guest!=0;
                ++retainedResidencyReferences;
            }
            peakRetainedResidencyReferences=(std::max)(peakRetainedResidencyReferences,retainedResidencyReferences);
            ++deferredEvictionRequests;
            std::cerr<<"{\"nativeGuestEvictionDeferred\":true,\"count\":"<<desc.count<<",\"flags\":"<<desc.flags
                     <<",\"retainedReferences\":"<<retainedResidencyReferences<<",\"nativeCall\":false,\"cpuLeasePreserved\":true}\n";
            reclaimDeferredResidency();
            return {{0,0,0},true};
        }
        if (!collectRetiredSubmissions() || !pendingSubmissions.empty() || !pendingHwSignals.empty() || !pendingHwWaits.empty())
            return {{static_cast<std::int32_t>(0x80000011u),0,0},false};
        D3DKMT_EVICT value{}; value.hDevice=device; value.NumAllocations=desc.count;
        value.AllocationList=handles.data(); value.Flags.Value=desc.flags;
        ++attemptedVendorEvictions;
        const auto status=D3DKMTEvict(&value);
        for (const auto handle:handles) {
            auto& allocation=vendorAllocations.at(handle);
            if (allocation.residency.guest) --allocation.residency.guest;
            allocation.resident=allocation.residency.guest!=0;
        }
        if (status==0) ++completedVendorEvictions; else ++failedVendorEvictions;
        std::cerr<<"{\"nativeVendorEviction\":true,\"count\":"<<desc.count<<",\"flags\":"<<desc.flags
                 <<",\"ntstatus\":"<<status<<",\"bytesToTrim\":"<<(status==0?value.NumBytesToTrim:0)
                 <<",\"pendingCommands\":0,\"pendingSignals\":0,\"pendingWaits\":0,\"nativeCall\":true}\n";
        return {{status>0?Invalid:status,0,status==0?value.NumBytesToTrim:0},true};
    }
    ResidentResult makeVendorResident(std::uint32_t queue, ResidentDesc desc, const std::vector<std::uint32_t>& handles,
                                     const std::vector<std::uint32_t>& priorities) override {
        const auto paging = pagingFences.find(queue);
        if (!residencyEnabled || !validResident(desc) || paging == pagingFences.end() || handles.size() != desc.count ||
            priorities.size() != (desc.priorities ? desc.count : 0)) return {{Invalid, 0, 0}, {0, 0, 0}};
        for (std::size_t n = 0; n < handles.size(); ++n) {
            const auto allocation = vendorAllocations.find(handles[n]);
            if (allocation == vendorAllocations.end() || allocation->second.device != paging->second.device ||
                std::find(handles.begin(), handles.begin() + n, handles[n]) != handles.begin() + n ||
                (!deferredResidencyEnabled && allocation->second.residencyAttempts >= MaxVendorResidencyAttempts))
                return {{Invalid, 0, 0}, {0, 0, 0}};
        }
        if(deferredResidencyEnabled) return makeDeferredVendorResident(queue,desc,handles,priorities);
        std::uint64_t usageBefore=0;
        if (!reportedGpuUsageAcceptable(0,nullptr,&usageBefore)) {
            ++failedVendorResidency;
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0, 0}};
        }
        const auto reclaimTarget=driver_shared::stagingPressureTarget(usageBefore,reportedGpuUsageLimit());
        if(compositorOutput && reclaimTarget) {
            std::uint64_t released=0,usageAfter=0;
            if(FAILED(compositorOutput->reclaimStagingForResidency(reclaimTarget,released))) {
                ++failedVendorResidency;
                throw std::runtime_error("Cannot safely reclaim idle presentation staging before residency");
            }
            const bool fits=reportedGpuUsageAcceptable(0,nullptr,&usageAfter);
            std::cerr<<"{\"nativeResidencyStagingPressure\":true,\"usageBefore\":"<<usageBefore
                <<",\"usageAfter\":"<<usageAfter<<",\"targetBytes\":"<<reclaimTarget<<",\"releasedBytes\":"<<released
                <<",\"reserveBytes\":"<<driver_shared::ResidencyStagingReserve<<",\"limitBytes\":"<<reportedGpuUsageLimit()
                <<",\"beforeNativeResidency\":true,\"accepted\":"<<(fits?"true":"false")<<"}\n";
            if(!fits) { ++failedVendorResidency;return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0,0}}; }
        }
        D3DDDI_MAKERESIDENT a{}; a.hPagingQueue = queue; a.NumAllocations = desc.count;
        a.AllocationList = handles.data(); a.PriorityList = priorities.empty() ? nullptr : priorities.data();
        a.Flags.Value = desc.flags;
        // Even a failing list operation may affect a subset. Bound every
        // attempt until synchronous allocation destruction releases ownership.
        for (const auto handle : handles) ++vendorAllocations.at(handle).residencyAttempts;
        vendorResidencyAttempts += desc.count;
        peakVendorResidencyAttempts = (std::max)(peakVendorResidencyAttempts, vendorResidencyAttempts);
        const auto status = D3DKMTMakeResident(&a);
        if (a.NumAllocations > desc.count || (status >= 0 && status != 0 && status != 0x103))
            throw std::runtime_error("Unexpected native residency output");
        if (status >= 0 && !waitPaging(queue, a.PagingFenceValue)) {
            ++failedVendorResidency;
            throw std::runtime_error("Native residency paging deadline exceeded");
        }
        if (!reportedGpuUsageAcceptable()) {
            ++failedVendorResidency;
            // Keep all allocations owned. The server stops its VM before
            // session destruction, including after partial native failure.
            throw std::runtime_error("Reported GPU usage limit after residency");
        }
        if (status >= 0) {
            ++completedVendorResidency; ++completedVendorResidencyWaits;
            vendorAllocationsMadeResident += a.NumAllocations;
            if (a.NumAllocations == desc.count)
                for (const auto handle : handles) { auto& allocation=vendorAllocations.at(handle); allocation.resident=true; ++allocation.residency.guest; }
        } else ++failedVendorResidency;
        return {{status, 0, a.PagingFenceValue}, {a.NumAllocations, 0, a.NumBytesToTrim}};
    }
    GpuVaResult mapVendorAllocation(std::uint32_t allocation, std::uint32_t queue, GpuVaDesc desc) override {
        const auto entry = vendorAllocations.find(allocation);
        if (!gpuVaEnabled || entry == vendorAllocations.end() || !validImportedGpuVa(desc, entry->second.openedSharedResource) || !pagingFences.count(queue) ||
            entry->second.device != pagingFences.at(queue).device ||
            (desc.driverProtection && !entry->second.resource) || entry->second.pages || entry->second.address)
            return {{Invalid,0,0},0};
        const bool managed=allowsReservationBackedMappings();
        bool covered=false;std::uint32_t reservedPages=0;
        if(managed) {
            const auto adapter=deviceAdapters.at(entry->second.device);
            covered=desc.base && std::any_of(gpuReservations.begin(),gpuReservations.end(),[&](const auto& item) {
                return item.second.adapter==adapter && gpuRangeContains(item.second.address,item.second.bytes,desc.base,desc.sizePages*4096);
            });
            for(const auto& item:vendorAllocations) if(item.second.reservationBackedMapping)
                reservedPages+=static_cast<std::uint32_t>(item.second.pages);
        }
        const auto standaloneLimit=vendorGpuPageLimit(capabilities().flags,openglDesktop4kEnabled,openglDesktopLargeMapEnabled);
        const bool fits=managed ? driver_mapping::fits(vendorMappedPages,reservedPages,desc.sizePages,standaloneLimit,
            reservationByteLimit()/4096,covered) : vendorGpuBudgetFits(vendorMappedPages,desc.sizePages,capabilities().flags,openglDesktop4kEnabled,openglDesktopLargeMapEnabled);
        if(!fits) {
            ++gpuVaQuotaRejections;
            std::cerr<<"{\"nativeGpuVaAdmissionRejected\":true,\"reason\":\"aggregate mapped address budget\",\"usedPages\":"<<vendorMappedPages
                     <<",\"requestedPages\":"<<desc.sizePages<<",\"limitPages\":"<<vendorGpuPageLimit(capabilities().flags,openglDesktop4kEnabled,openglDesktopLargeMapEnabled)
                     <<",\"nativeCall\":false,\"memoryGuardRaised\":false,\"reservationAccountingSeparated\":"<<(managed?"true":"false")<<"}\n";
            return {{managed?static_cast<std::int32_t>(0xc0000017u):Invalid,0,0},0};
        }
        if(managed && !reportedGpuUsageAcceptable()) return {{static_cast<std::int32_t>(0xc0000017u),0,0},0};
        if (desc.base) {
            const auto overlaps = [&](std::uint64_t address, std::uint64_t bytes) {
                return address && bytes && desc.base < address + bytes && address < desc.base + desc.sizePages * 4096;
            };
            for (const auto& mapped : vendorAllocations)
                if (overlaps(mapped.second.address, mapped.second.pages * 4096)) return {{Invalid, 0, 0}, 0};
            for (const auto& mapped : allocations)
                if (overlaps(mapped.second.gpuAddress, mapped.second.size)) return {{Invalid, 0, 0}, 0};
        }
        GpuStateRanges::Plan statePlan;
        if (desc.base && !gpuStates.prepareRemove(desc.base, desc.sizePages * 4096, statePlan)) return {{Invalid, 0, 0}, 0};
        D3DDDI_MAPGPUVIRTUALADDRESS a{}; a.hPagingQueue = queue; a.hAllocation = allocation;
        a.BaseAddress = desc.base; a.MinimumAddress = desc.minimum; a.MaximumAddress = desc.maximum;
        a.OffsetInPages = desc.offsetPages; a.SizeInPages = desc.sizePages;
        a.Protection.Value = desc.protection; a.DriverProtection = desc.driverProtection;
        const auto status = D3DKMTMapGpuVirtualAddress(&a);
        if (status < 0) { ++failedVendorMaps; return {{status, 0, 0}, 0}; }
        // Record ownership before any validation/wait can throw. Destruction
        // releases this VA even if paging fails; retries cannot create a leak.
        entry->second.address = a.VirtualAddress; entry->second.pages = desc.sizePages;
        entry->second.reservationBackedMapping=covered;
        entry->second.offsetPages = desc.offsetPages;
        vendorMappedPages += static_cast<std::uint32_t>(desc.sizePages);
        peakVendorMappedPages = (std::max)(peakVendorMappedPages, vendorMappedPages);
        const auto standalonePages=vendorMappedPages-reservedPages-(covered?static_cast<std::uint32_t>(desc.sizePages):0u);
        peakStandaloneMappedPages=(std::max)(peakStandaloneMappedPages,standalonePages);
        if(managed) std::cerr<<"{\"nativeGpuMappingAccounted\":true,\"reservationBacked\":"<<(covered?"true":"false")
            <<",\"totalPages\":"<<vendorMappedPages<<",\"standalonePages\":"<<standalonePages
            <<",\"standaloneLimitPages\":"<<standaloneLimit<<",\"reservationLimitPages\":"<<reservationByteLimit()/4096
            <<",\"residentUsageGuardBytes\":"<<reportedGpuUsageLimit()<<",\"ownedReservationChecked\":true}\n";
        if ((status != 0 && status != 0x103) || !validImportedGpuVaOutput(desc, a.VirtualAddress, entry->second.openedSharedResource))
            throw std::runtime_error("Unexpected native GPU-address mapping output");
        if (!desc.base && !gpuStates.prepareRemove(a.VirtualAddress, desc.sizePages * 4096, statePlan))
            throw std::runtime_error("Native GPU state tracking quota after allocation mapping");
        gpuStates.commit(statePlan);
        if (!waitPaging(queue, a.PagingFenceValue)) {
            ++failedVendorMaps;
            // Do not expose an incomplete mapping. The server first reaps its
            // owned VM, then destroys the allocation and its paging queue.
            throw std::runtime_error("Native GPU-address paging deadline exceeded");
        }
        if (!reportedGpuUsageAcceptable()) {
            ++failedVendorMaps;
            throw std::runtime_error("Reported GPU usage limit after address mapping");
        }
        ++completedVendorMapWaits; ++completedVendorMaps;
        if(managed && !reportedGpuUsageAcceptable()) throw std::runtime_error("Reported GPU usage limit after reservation-backed mapping");
        if (desc.driverProtection) {
            ++completedVendorDriverProtectionMaps;
            std::cerr << "{\"nativeVendorDriverProtectionMap\":true,\"driverProtection\":" << desc.driverProtection
                      << ",\"resourceOwned\":true,\"unchanged\":true,\"ntstatus\":" << status << "}\n";
        }
        if (desc.driverProtection == VendorImportedDriverProtection) {
            std::cerr << "{\"nativeImportedResourceGpuVaMapped\":true,\"driverProtection\":" << desc.driverProtection
                      << ",\"resourceOpened\":" << (entry->second.openedSharedResource ? "true" : "false")
                      << ",\"bytes\":" << desc.sizePages * 4096 << ",\"pagingFence\":" << a.PagingFenceValue
                      << ",\"ntstatus\":" << status << ",\"unchanged\":true,\"addressRangeValidated\":true}\n";
        }
        // Preserve STATUS_PENDING even though our native wait already retired
        // the fence. The Linux KMT thunk preserves positive ioctl statuses.
        return {{status, 0, a.VirtualAddress}, a.PagingFenceValue};
    }
    VendorCpuResult lockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        const auto entry = vendorAllocations.find(allocation);
        if (!cpuEnabled || !runtime || entry == vendorAllocations.end() || entry->second.device != device ||
            entry->second.locked || !entry->second.pages || entry->second.offsetPages)
            return {{Invalid, 0, 0}, {0, 0, 0}};
        if (runtime->allocationSlotLimit()>=SparseVendorCpuSlots && !openglDesktopSparseEnabled)
            return {{Invalid,0,0},{0,0,0}};
        auto& owned = entry->second;
        const auto bytes = static_cast<std::uint32_t>(owned.pages * 4096);
        if (!bytes || bytes > MaxVendorCpuMappingBytes)
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0, 0}};
        if(!vendorCpuBudgetFits(vendorCpuBytes,bytes,capabilities().flags,openglDesktop4kCpuEnabled,openGlProfileVersion==14)) {
            ++vendorCpuByteQuotaRejections;
            std::cerr<<"{\"nativeCpuByteAdmissionRejected\":true,\"usedBytes\":"<<vendorCpuBytes<<",\"requestedBytes\":"<<bytes
                <<",\"limitBytes\":"<<vendorCpuByteLimit(capabilities().flags,openglDesktop4kCpuEnabled,openGlProfileVersion==14)<<"}\n";
            return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0,0}};
        }
        if(!reportedGpuUsageAcceptable()) return {{static_cast<std::int32_t>(0xc0000017u),0,0},{0,0,0}};
        // Exhaustion is a native allocation failure that the live UMD can
        // recover from, not a QMP mapping exception after obtaining a lock.
        if (!runtime->canMapAllocation(bytes)) {
            ++vendorCpuSlotQuotaRejections;
            return {{static_cast<std::int32_t>(0xc0000017u), 0, 0}, {0, 0, 0}};
        }
        D3DKMT_LOCK2 a{}; a.hDevice = device; a.hAllocation = allocation;
        const auto status = D3DKMTLock2(&a);
        if (status < 0) {
            std::cerr << "{\"nativeCpuLockRejected\":true,\"ntstatus\":" << status << ",\"mappedBytes\":" << bytes << "}\n";
            ++failedVendorLocks; return {{status, 0, 0}, {0, 0, 0}};
        }
        // Retain the native lock before validation/control can fail. No host
        // CPU address leaves this process; the guest receives a BAR lease.
        owned.locked = true; owned.cpuData = a.pData;
        MEMORY_BASIC_INFORMATION region{}, next{};
        const auto source = reinterpret_cast<std::uintptr_t>(a.pData);
        const bool committedRegion = source && VirtualQuery(a.pData, &region, sizeof region) == sizeof region &&
            region.AllocationBase && driver_cpu::validNativeAllocationView(source, bytes,
                reinterpret_cast<std::uintptr_t>(region.BaseAddress), region.RegionSize) &&
            region.Type == MEM_PRIVATE && region.State == MEM_COMMIT &&
            (region.Protect == PAGE_READWRITE || region.Protect == (PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (committedRegion) VirtualQuery(reinterpret_cast<void*>(source + bytes), &next, sizeof next);
        std::cerr << "{\"vendorCpuRegionCandidate\":true,\"status\":" << status << ",\"expectedBytes\":" << bytes
                  << ",\"regionBytes\":" << region.RegionSize << ",\"type\":" << region.Type << ",\"state\":" << region.State
                  << ",\"protection\":" << region.Protect << ",\"baseMatches\":" << (region.BaseAddress == a.pData ? "true" : "false")
                  << ",\"allocationBaseMatches\":" << (region.AllocationBase == a.pData ? "true" : "false")
                  << ",\"regionBaseOffsetBytes\":" << (source >= reinterpret_cast<std::uintptr_t>(region.BaseAddress) ? source - reinterpret_cast<std::uintptr_t>(region.BaseAddress) : 0)
                  << ",\"exportedRangeContained\":" << (committedRegion ? "true" : "false")
                  << ",\"nextState\":" << next.State << ",\"nextAllocationBaseMatches\":" << (next.AllocationBase == a.pData ? "true" : "false") << "}\n";
        // One Windows reservation may contain several driver allocations.
        // Export only the native allocation's mapped byte range at offset zero,
        // never the full region or reservation. The native lock owns its lifetime;
        // the QEMU hub also rejects overlapping active leases.
        // VirtualQuery checks only this committed region's eligibility,
        // not a public decoder for the size of arbitrary vendor allocations.
        if (status != 0 || !committedRegion || !reportedGpuUsageAcceptable()) {
            ++failedVendorLocks;
            const auto released = unlockVendorAllocation(allocation, device);
            if (released.ntstatus < 0) throw std::runtime_error("Cannot release ineligible native CPU lock");
            return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0, 0}};
        }
        owned.cpuBytes = bytes; vendorCpuBytes += bytes;
        peakVendorCpuBytes = (std::max)(peakVendorCpuBytes, vendorCpuBytes);
        // Write-combined GPU memory must not be scanned for ordinary locking.
        // These reads exist only for the explicit CPU-store diagnostic; actual
        // desktop correctness uses the independent retired-backbuffer audits.
        if(cpuStoreTest) {
            owned.initialCpuFingerprint = fingerprint(owned.cpuData, bytes);
            owned.firstWord = *static_cast<volatile std::uint64_t*>(owned.cpuData);
            owned.lastWord = *reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(owned.cpuData) + bytes - 8);
        }
        owned.cpuLease = runtime->mapAllocation(a.pData, bytes);
        if (region.RegionSize != bytes || region.BaseAddress != a.pData) ++completedVendorSubrangeLocks;
        ++completedVendorLocks;
        return {{status, 0, owned.cpuLease->offset}, {bytes, 0, owned.cpuLease->generation}};
    }
    Result unlockVendorAllocation(std::uint32_t allocation, std::uint32_t device) override {
        const auto entry = vendorAllocations.find(allocation);
        if (!cpuEnabled || !runtime || entry == vendorAllocations.end() || entry->second.device != device || !entry->second.locked)
            return {Invalid, 0, 0};
        auto& owned = entry->second;
        const bool afterVmExit = owned.cpuLease.has_value() && runtime->hasStopped();
        if (owned.cpuLease) {
            // A control failure reaps QEMU before throwing. Retain native
            // ownership until that boundary is proved; never free live pages.
            runtime->unmapAllocation(*owned.cpuLease);
            owned.cpuLease.reset();
        }
        MemoryBarrier();
        const bool changed = cpuStoreTest && owned.cpuBytes && fingerprint(owned.cpuData, owned.cpuBytes) != owned.initialCpuFingerprint;
        if (cpuStoreTest && owned.cpuBytes) {
            auto first = static_cast<volatile std::uint64_t*>(owned.cpuData);
            auto last = reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(owned.cpuData) + owned.cpuBytes - 8);
            const bool matched = *first == CpuStoreFirstMarker && *last == CpuStoreLastMarker;
            *first = owned.firstWord; *last = owned.lastWord; MemoryBarrier();
            const bool restored = *first == owned.firstWord && *last == owned.lastWord;
            if (matched && restored) ++completedCpuStoreTests; else ++failedCpuStoreTests;
        }
        D3DKMT_UNLOCK2 a{}; a.hDevice = device; a.hAllocation = allocation;
        const auto status = D3DKMTUnlock2(&a);
        if (status < 0) { ++failedVendorUnlocks; return {status, 0, 0}; }
        if (status != 0) throw std::runtime_error("Unexpected native CPU unlock status");
        if (afterVmExit) ++cpuLocksReleasedAfterVmExit;
        if (changed) ++vendorCpuContentsChanged;
        vendorCpuBytes -= owned.cpuBytes; owned.cpuBytes = 0;
        owned.cpuData = nullptr; owned.locked = false; ++completedVendorUnlocks;
        if(!runtime->hasStopped()) reclaimDeferredResidency();
        return {status, 0, 0};
    }
    Result translateVendorAllocation(std::uint32_t allocation, std::uint32_t device, std::uint32_t adapter) override {
        const auto owned = vendorAllocations.find(allocation);
        const auto parent = deviceAdapters.find(device);
        if (!translationEnabled || owned == vendorAllocations.end() || owned->second.device != device ||
            parent == deviceAdapters.end() || parent->second != adapter) return {Invalid, 0, 0};
        const auto cached = translatedAllocations.find(allocation);
        if (cached != translatedAllocations.end()) return {0, 0, cached->second};
        D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE data{};
        data.EscapeType = D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE; data.hAllocation = allocation;
        D3DKMT_ESCAPE escape{}; escape.hAdapter = adapter; escape.hDevice = device;
        escape.Type = D3DKMT_ESCAPE_DRIVERPRIVATE; escape.Flags.DriverKnownEscape = 1;
        escape.pPrivateDriverData = &data; escape.PrivateDriverDataSize = sizeof data;
        const auto status = D3DKMTEscape(&escape);
        if (status < 0) { ++failedTranslations; return {status, 0, 0}; }
        if (status != 0 || data.EscapeType != D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE || !data.hAllocation)
            throw std::runtime_error("Unexpected native allocation translation output");
        for (const auto& token : translatedAllocations)
            if (token.second == data.hAllocation) throw std::runtime_error("Duplicate native allocation driver token");
        translatedAllocations.emplace(allocation, data.hAllocation); ++completedTranslations;
        return {status, 0, data.hAllocation};
    }
    Result createPagingQueue(std::uint32_t device) override {
        D3DKMT_CREATEPAGINGQUEUE a{}; a.hDevice = device; a.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        const auto status = D3DKMTCreatePagingQueue(&a);
        if (status >= 0) {
            try { pagingFences.emplace(a.hPagingQueue, Paging{
                static_cast<volatile std::uint64_t*>(a.FenceValueCPUVirtualAddress), a.hSyncObject, std::nullopt, device}); }
            catch (...) {
                D3DDDI_DESTROYPAGINGQUEUE cleanup{}; cleanup.hPagingQueue = a.hPagingQueue;
                if (D3DKMTDestroyPagingQueue(&cleanup) < 0) ++cleanupFailures;
                throw;
            }
        }
        return {status, a.hPagingQueue, 0};
    }
    GuestPagingResult createGuestPagingQueue(std::uint32_t device) override {
        if (!runtime) return Driver::createGuestPagingQueue(device);
        if (!runtime->canMapFence()) return {{static_cast<std::int32_t>(0xc0000017u),0,0},0,0};
        const auto result = createPagingQueue(device);
        if (result.ntstatus < 0) return {result, 0, 0};
        auto& queue = pagingFences.at(result.nativeHandle);
        try {
            if (!queue.sync) throw std::runtime_error("Native paging synchronization object absent");
            queue.lease = runtime->map(queue.fence);
            return {result, queue.sync, queue.lease->offset};
        } catch (...) {
            // map() stops its owned VM before returning a control failure.
            // Also stop on a malformed native result before releasing pages.
            runtime->stop(0);
            destroy(Kind::PagingQueue, result.nativeHandle);
            throw;
        }
    }
    Result readPagingFence(std::uint32_t queue) override {
        const auto entry = pagingFences.find(queue);
        if (entry == pagingFences.end() || !entry->second.fence) return {static_cast<std::int32_t>(0xc0000008u), 0, 0};
        // x64 aligned read from the KMT-owned read-only mapping, never exported.
        const auto value = *entry->second.fence; MemoryBarrier(); return {0, 0, value};
    }
    Result createAllocation(std::uint32_t device, std::uint32_t size) override {
        if (!size || size % 4096 || size > MaxAllocation || size > MaxAllocatedBytes - allocatedBytes)
            return {Invalid, 0, 0};
        auto memory = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!memory) return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        return createBackedAllocation(device, size, memory, true);
    }
    Result createSharedAllocation(std::uint32_t device, GuestRange range) override {
        if (guestSectionName.empty()) return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
        if (range.reserved || !range.size || range.size % 4096 || range.offset % 4096 || range.size > MaxAllocation ||
            range.size > guestBytes || range.offset > guestBytes - range.size || range.size > MaxAllocatedBytes - allocatedBytes)
            return {Invalid, 0, 0};
        if (!guestSection) {
            const std::wstring name(guestSectionName.begin(), guestSectionName.end());
            // D3D12's existing-heap import also queries the section object.
            guestSection = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
            if (!guestSection) return {static_cast<std::int32_t>(0xc0000008u), 0, 0};
        }
        if (!guestMemory) {
            guestMemory = MapViewOfFile(guestSection, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, guestBytes);
            if (!guestMemory) return {static_cast<std::int32_t>(0xc0000017u), 0, 0};
        }
        // This fixture's RAM is one section, with guest physical offsets equal
        // to section offsets below 512 MiB. No arbitrary host address is used.
        auto memory = static_cast<std::uint8_t*>(guestMemory) + static_cast<std::size_t>(range.offset);
        return createBackedAllocation(device, range.size, memory, false);
    }
    Result copySharedAllocation(std::uint32_t destination, std::uint32_t source, CopyRange range) override {
        const auto to = allocations.find(destination), from = allocations.find(source);
        if (!copyDevice || !guestMemory || !guestSection || to == allocations.end() || from == allocations.end() ||
            destination == source || to->second.owned || from->second.owned ||
            to->second.device != from->second.device || to->second.pagingFailed || from->second.pagingFailed ||
            !range.size || range.size > to->second.size || range.size > from->second.size ||
            range.sourceOffset > from->second.size - range.size || range.destinationOffset > to->second.size - range.size)
            return {Invalid, 0, 0};
        const auto toOffset = static_cast<std::uint8_t*>(to->second.memory) - static_cast<std::uint8_t*>(guestMemory);
        const auto fromOffset = static_cast<std::uint8_t*>(from->second.memory) - static_cast<std::uint8_t*>(guestMemory);
        if (toOffset % 65536 || fromOffset % 65536 || to->second.size % 65536 || from->second.size % 65536)
            return {Invalid, 0, 0};
        HRESULT hr;
        if (!copyHeap) {
            hr = copyDevice->OpenExistingHeapFromFileMapping(guestSection, IID_PPV_ARGS(&copyHeap));
            if (FAILED(hr)) return copyFailure(hr);
            if (copyHeap->GetDesc().SizeInBytes != guestBytes) return copyFailure(E_INVALIDARG);
        }
        if (!copyQueue) {
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_COPY;
            hr = copyDevice->CreateCommandQueue(&q, IID_PPV_ARGS(&copyQueue));
            if (FAILED(hr)) return copyFailure(hr);
        }
        if (!copyFence) {
            hr = copyDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence));
            if (FAILED(hr)) return copyFailure(hr);
        }
        if (!copyEvent) {
            copyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!copyEvent) return copyFailure(HRESULT_FROM_WIN32(GetLastError()));
        }
        const auto createBuffer = [&](std::uint32_t size, std::uint64_t offset, D3D12_RESOURCE_STATES state,
                                      ComPtr<ID3D12Resource>& resource) {
            D3D12_RESOURCE_DESC b{}; b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; b.Width = size; b.Height = 1;
            b.DepthOrArraySize = b.MipLevels = 1; b.SampleDesc.Count = 1; b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            b.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
            return copyDevice->CreatePlacedResource(copyHeap.Get(), offset, &b, state, nullptr, IID_PPV_ARGS(&resource));
        };
        ComPtr<ID3D12Resource> toBuffer, fromBuffer;
        hr = createBuffer(to->second.size, toOffset, D3D12_RESOURCE_STATE_COPY_DEST, toBuffer);
        if (FAILED(hr)) return copyFailure(hr);
        hr = createBuffer(from->second.size, fromOffset, D3D12_RESOURCE_STATE_COPY_SOURCE, fromBuffer);
        if (FAILED(hr)) return copyFailure(hr);
        ComPtr<ID3D12CommandAllocator> allocator;
        hr = copyDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator));
        if (FAILED(hr)) return copyFailure(hr);
        ComPtr<ID3D12GraphicsCommandList> list;
        hr = copyDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        if (FAILED(hr)) return copyFailure(hr);
        MemoryBarrier(); // guest CPU uploads precede this serialized request
        list->CopyBufferRegion(toBuffer.Get(), range.destinationOffset, fromBuffer.Get(), range.sourceOffset, range.size);
        hr = list->Close(); if (FAILED(hr)) return copyFailure(hr);
        ID3D12CommandList* lists[] = {list.Get()}; copyQueue->ExecuteCommandLists(1, lists);
        const auto target = ++copyFenceValue;
        hr = copyQueue->Signal(copyFence.Get(), target);
        if (FAILED(hr) || native_gpu::checkedFenceCompletion(target,
                [&] { return copyFence->GetCompletedValue(); },
                [&] { return SUCCEEDED(copyFence->SetEventOnCompletion(target, copyEvent)); },
                [&] { return WaitForSingleObject(copyEvent, 5000) == WAIT_OBJECT_0; }) !=
                native_gpu::FenceCompletion::Complete) {
            // GPU may still own the section and resources. Keep them alive for
            // OS process teardown; never unwind and unmap in-flight backing.
            std::cerr << "FAIL: GPU copy did not complete; worker terminated with backing retained\n" << std::flush;
            ExitProcess(3);
        }
        MemoryBarrier(); ++completedCopies; gpuCopiedBytes += range.size;
        return {0, 0, range.size};
    }
    Result writeAllocation(std::uint32_t handle, Range range, const std::uint8_t* data) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || entry->second.pagingFailed || !rangeValid(entry->second, range)) return {Invalid, 0, 0};
        // Requests are serialized. GPU copy returns only after its fence;
        // future asynchronous submissions need explicit CPU/GPU ownership.
        std::memcpy(static_cast<std::uint8_t*>(entry->second.memory) + range.offset, data, range.size);
        MemoryBarrier(); return {0, 0, range.size};
    }
    Result readAllocation(std::uint32_t handle, Range range, std::vector<std::uint8_t>& data) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || entry->second.pagingFailed || !rangeValid(entry->second, range)) return {Invalid, 0, 0};
        data.resize(range.size); MemoryBarrier();
        std::memcpy(data.data(), static_cast<const std::uint8_t*>(entry->second.memory) + range.offset, range.size);
        return {0, 0, range.size};
    }
    Result makeResident(std::uint32_t handle, std::uint32_t queue) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end()) return {Invalid, 0, 0};
        if (entry->second.pagingFailed) return {PagingTimeout, 0, 0};
        if (entry->second.resident) return {0, 0, 0};
        D3DDDI_MAKERESIDENT a{}; a.hPagingQueue = queue; a.NumAllocations = 1; a.AllocationList = &handle;
        auto status = D3DKMTMakeResident(&a);
        if (status >= 0 && !waitPaging(queue, a.PagingFenceValue)) {
            entry->second.pagingFailed = true; status = PagingTimeout;
        }
        if (status >= 0) entry->second.resident = true;
        return {status, 0, a.PagingFenceValue};
    }
    Result mapAllocation(std::uint32_t handle, std::uint32_t queue) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end() || !entry->second.resident) return {Invalid, 0, 0};
        if (entry->second.pagingFailed) return {PagingTimeout, 0, 0};
        if (entry->second.gpuAddress) return {0, 0, entry->second.gpuAddress};
        D3DDDI_MAPGPUVIRTUALADDRESS a{}; a.hPagingQueue = queue; a.hAllocation = handle;
        a.SizeInPages = entry->second.size / 4096; a.Protection.Write = 1;
        auto status = D3DKMTMapGpuVirtualAddress(&a);
        if (status >= 0) {
            // Even a timed-out pending map is owned by the allocation. Prevent
            // another map attempt until it is destroyed rather than leaking VA.
            entry->second.gpuAddress = a.VirtualAddress;
            if (!waitPaging(queue, a.PagingFenceValue)) { entry->second.pagingFailed = true; status = PagingTimeout; }
        }
        return {status, 0, status >= 0 ? a.VirtualAddress : 0};
    }
    Result queryResidency(std::uint32_t handle) override {
        const auto entry = allocations.find(handle);
        if (entry == allocations.end()) return {Invalid, 0, 0};
        D3DKMT_ALLOCATIONRESIDENCYSTATUS residency{};
        D3DKMT_QUERYALLOCATIONRESIDENCY a{}; a.hDevice = entry->second.device;
        a.phAllocationList = &handle; a.AllocationCount = 1; a.pResidencyStatus = &residency;
        const auto status = D3DKMTQueryAllocationResidency(&a); return {status, 0, static_cast<std::uint64_t>(residency)};
    }
    Result destroy(Kind kind, std::uint32_t handle) override {
        bool referenceClosedDuringGpuWork=false;
        if (asyncSubmitEnabled) {
            if (!collectRetiredSubmissions()) return {Invalid,0,0};
            // A failed/abrupt guest must not free a dependency of queued work.
            // Queue destruction below separately requires its own retirement.
            const bool pending=!pendingSubmissions.empty() || !pendingHwSignals.empty();
            if (pending && nativeDestroyWaitsForSubmissions(kind))
                return {Invalid,0,0};
            referenceClosedDuringGpuWork=pending && (kind==Kind::SharedResource || kind==Kind::SharedSync);
        }
        NTSTATUS status{};
        if (kind == Kind::VendorAllocation) {
            const auto found = vendorAllocations.find(handle);
            if (found == vendorAllocations.end()) return {Invalid, 0, 0};
            return destroyVendorAllocations(found->second.device, {handle});
        } else if (kind == Kind::VendorResource) {
            return {0, 0, 0}; // Borrowed resource identity; its single allocation owns native destruction.
        } else if (kind == Kind::Allocation) {
            const auto entry = allocations.find(handle);
            if (entry == allocations.end()) return {Invalid, 0, 0};
            D3DKMT_DESTROYALLOCATION2 a{}; a.hDevice = entry->second.device; a.hResource = entry->second.resource;
            // ExistingSysMem remains secured until destruction completes.
            // Wait for completion before releasing the worker's CPU backing.
            a.Flags.SynchronousDestroy = 1;
            status = D3DKMTDestroyAllocation2(&a);
            if (status >= 0) {
                // The allocation's GPU VA is released by the documented
                // allocation destruction. Free CPU backing only after success.
                if (entry->second.owned && !VirtualFree(entry->second.memory, 0, MEM_RELEASE)) ++cleanupFailures;
                allocatedBytes -= entry->second.size; allocations.erase(entry);
            }
        } else if (kind == Kind::PagingQueue) {
            const auto queue = pagingFences.find(handle);
            if (queue == pagingFences.end()) return {Invalid, 0, 0};
            if(!collectRetiredSubmissions()) return {Invalid,0,0};
            for(const auto& wait:pendingHwWaits)
                if(std::find(wait.syncs.begin(),wait.syncs.begin()+wait.count,queue->second.sync)!=wait.syncs.begin()+wait.count) return {Invalid,0,0};
            if (queue->second.lease) {
                try { runtime->unmap(*queue->second.lease); }
                catch (...) {
                    // No unmap acknowledgement: retain pages until the owned
                    // VM has exited. Its control failure is reported separately.
                    runtime->stop(0);
                }
                queue->second.lease.reset();
            }
            D3DDDI_DESTROYPAGINGQUEUE a{}; a.hPagingQueue = handle; status = D3DKMTDestroyPagingQueue(&a);
            if (status >= 0) pagingFences.erase(handle);
        } else if (kind == Kind::PagingSync) {
            // Borrowed queue-owned KMT sync object; the queue releases it.
            return {0, 0, 0};
        } else if (kind == Kind::HwQueue) {
            const auto queue = hwQueues.find(handle);
            if (queue == hwQueues.end() || !runtime) return {Invalid, 0, 0};
            if (!collectRetiredSubmissions()) return {Invalid,0,0};
            for(const auto& wait:pendingHwWaits)
                if(std::find(wait.syncs.begin(),wait.syncs.begin()+wait.count,queue->second.sync)!=wait.syncs.begin()+wait.count) return {Invalid,0,0};
            if (std::any_of(pendingSubmissions.begin(), pendingSubmissions.end(), [handle](const PendingSubmission& p) { return p.queue == handle; }))
                return {Invalid, 0, 0}; // Keep its fence and allocations owned.
            if (std::any_of(pendingHwSignals.begin(),pendingHwSignals.end(),[handle](const PendingHwSignal& p) {
                return std::find(p.queues.begin(),p.queues.begin()+p.count,handle) != p.queues.begin()+p.count;
            })) return {Invalid,0,0};
            if (std::any_of(pendingHwWaits.begin(), pendingHwWaits.end(), [handle,&queue](const PendingHwWait& p) {
                return p.contextStream ? p.queue == queue->second.context : p.queue == handle;
            }))
                return {Invalid, 0, 0};
            if (std::any_of(pendingContextSignals.begin(), pendingContextSignals.end(), [&queue](const PendingContextSignal& p) {
                return p.context == queue->second.context;
            })) return {Invalid, 0, 0};
            const bool afterVmExit = runtime->hasStopped();
            if (queue->second.lease) {
                try { runtime->unmap(*queue->second.lease); }
                catch (...) { runtime->stop(0); }
                queue->second.lease.reset();
            }
            D3DKMT_DESTROYHWQUEUE a{}; a.hHwQueue = handle; status = D3DKMTDestroyHwQueue(&a);
            if (status == 0) {
                hwQueues.erase(queue); ++destroyedHwQueues;
                if (afterVmExit) ++hwQueuesReleasedAfterVmExit;
            }
        } else if (kind == Kind::HwQueueSync) {
            return {0, 0, 0}; // Borrowed progress sync, owned by the hardware queue.
        } else if (kind == Kind::GpuReservation) {
            const auto entry = gpuReservations.find(handle);
            if (entry == gpuReservations.end() || !runtime) return {Invalid, 0, 0};
            const auto& range = entry->second;
            if (!validGpuReservationOutput({range.address, 0, 0, range.bytes}, range.address)) return {Invalid, 0, 0};
            for (const auto& allocation : vendorAllocations)
                if (gpuRangesOverlap(range.address, range.bytes, allocation.second.address, allocation.second.pages * 4096) &&
                    !gpuRangeContains(range.address, range.bytes, allocation.second.address, allocation.second.pages * 4096)) return {Invalid, 0, 0};
            for (const auto& allocation : allocations)
                if (gpuRangesOverlap(range.address, range.bytes, allocation.second.gpuAddress, allocation.second.size) &&
                    !gpuRangeContains(range.address, range.bytes, allocation.second.gpuAddress, allocation.second.size)) return {Invalid, 0, 0};
            for (const auto& item : hwQueues) {
                const auto& queue = item.second;
                // Failure to destroy a queue at EOF retains address ownership.
                if (runtime->hasStopped() || !queue.fence) return {Invalid, 0, 0};
                MemoryBarrier(); const auto value = *queue.fence;
                if (value == UINT64_MAX || value < queue.submittedFence) return {Invalid, 0, 0};
            }
            D3DKMT_FREEGPUVIRTUALADDRESS a{}; a.hAdapter = range.adapter; a.BaseAddress = range.address; a.Size = range.bytes;
            status = D3DKMTFreeGpuVirtualAddress(&a);
            if (status > 0) throw std::runtime_error("Unexpected native GPU reservation release status");
            if (status == 0) {
                for (auto& allocation : vendorAllocations) {
                    auto& mapped = allocation.second;
                    if (gpuRangesOverlap(range.address, range.bytes, mapped.address, mapped.pages * 4096)) {
                        vendorMappedPages -= static_cast<std::uint32_t>(mapped.pages); mapped.pages = 0; mapped.address = 0; mapped.offsetPages = 0;
                    }
                }
                for (auto& allocation : allocations)
                    if (gpuRangesOverlap(range.address, range.bytes, allocation.second.gpuAddress, allocation.second.size)) allocation.second.gpuAddress = 0;
                gpuStates.release(handle); gpuReservedBytes -= range.bytes; gpuReservations.erase(entry); ++freedReservations;
                if (runtime->hasStopped()) ++reservationsReleasedAfterVmExit;
            }
        } else if (kind == Kind::SharedResource) {
            const auto shared = sharedResourceHandles.find(handle);
            if (shared == sharedResourceHandles.end()) return {Invalid, 0, 0};
            if (compositorOutput) compositorOutput->releaseSource(handle);
            if (!shared->second.handle.close()) return {static_cast<std::int32_t>(0xc0000001u), 0, 0};
            sharedResourceHandles.erase(shared); ++closedResourceHandles; status = 0;
            std::cerr << "{\"nativeResourceHandleClosed\":true}\n";
        } else if (kind == Kind::SharedSync) {
            const auto shared = sharedSyncHandles.find(handle);
            if (shared == sharedSyncHandles.end()) return {Invalid, 0, 0};
            if (!shared->second.close()) return {static_cast<std::int32_t>(0xc0000001u), 0, 0};
            sharedSyncHandles.erase(shared); ++closedSyncHandles; status = 0;
            std::cerr << "{\"nativeSyncHandleClosed\":true}\n";
        } else if (kind == Kind::Sync) {
            const auto object = syncObjects.find(handle);
            if (object == syncObjects.end() || !runtime) return {Invalid, 0, 0};
            if (std::any_of(pendingHwWaits.begin(), pendingHwWaits.end(), [handle](const PendingHwWait& p) {
                return std::find(p.syncs.begin(), p.syncs.begin() + p.count, handle) != p.syncs.begin() + p.count;
            }) || std::any_of(pendingContextSignals.begin(), pendingContextSignals.end(), [handle](const PendingContextSignal& p) {
                return p.sync == handle;
            })) return {Invalid, 0, 0};
            const bool afterVmExit = runtime->hasStopped();
            if (object->second.lease) {
                try { runtime->unmap(*object->second.lease); }
                catch (...) { runtime->stop(0); }
                object->second.lease.reset();
            }
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT a{}; a.hSyncObject = handle;
            status = D3DKMTDestroySynchronizationObject(&a);
            if (status == 0) {
                syncObjects.erase(object); ++destroyedSyncs;
                if (afterVmExit) ++syncsReleasedAfterVmExit;
            }
        } else if (kind == Kind::Context) {
            if (!contextOwners.count(handle)) return {Invalid, 0, 0};
            if (std::any_of(pendingHwWaits.begin(), pendingHwWaits.end(), [handle](const PendingHwWait& p) {
                return p.contextStream && p.queue == handle;
            })) return {Invalid, 0, 0};
            if (std::any_of(pendingContextSignals.begin(), pendingContextSignals.end(), [handle](const PendingContextSignal& p) {
                return p.context == handle;
            })) return {Invalid, 0, 0};
            for (const auto& queue : hwQueues) if (queue.second.context == handle) return {Invalid, 0, 0};
            D3DKMT_DESTROYCONTEXT a{}; a.hContext = handle; status = D3DKMTDestroyContext(&a);
            if (status >= 0) { --activeContexts; contextOwners.erase(handle); }
        } else if (kind == Kind::Device) {
            if(!deviceAdapters.count(handle)) return {Invalid,0,0};
            // This driver is shared by coordinated actors. Enforce native
            // ownership independently of any one Session's wire bookkeeping.
            for(const auto& item:contextOwners) if(item.second.device==handle) return {Invalid,0,0};
            for(const auto& item:hwQueues) if(item.second.device==handle) return {Invalid,0,0};
            for(const auto& item:pagingFences) if(item.second.device==handle) return {Invalid,0,0};
            for(const auto& item:syncObjects) if(item.second.device==handle) return {Invalid,0,0};
            for(const auto& item:allocations) if(item.second.device==handle) return {Invalid,0,0};
            for(const auto& item:vendorAllocations) if(item.second.device==handle) return {Invalid,0,0};
            D3DKMT_DESTROYDEVICE a{}; a.hDevice = handle; status = D3DKMTDestroyDevice(&a);
            if(status>0) status=Invalid;
            if (status == 0) { --activeDevices; deviceAdapters.erase(handle); deviceExecutionStates.erase(handle); }
        } else {
            if (kind != Kind::Adapter || !ownedAdapters.count(handle)) return {Invalid, 0, 0};
            for(const auto& device:deviceAdapters) if(device.second==handle) return {Invalid,0,0};
            for (const auto& range : gpuReservations) if (range.second.adapter == handle) return {Invalid, 0, 0};
            D3DKMT_CLOSEADAPTER a{}; a.hAdapter = handle; status = D3DKMTCloseAdapter(&a);
            if(status>0) status=Invalid;
            if (status == 0) { --activeAdapters; ownedAdapters.erase(handle); }
        }
        if (status < 0) ++cleanupFailures;
        if(!status && referenceClosedDuringGpuWork) {
            ++ntReferencesClosedDuringGpuWork;
            std::cerr<<"{\"nativeNtReferenceClosedDuringGpuWork\":true,\"kind\":\""<<(kind==Kind::SharedResource ? "resource" : "sync")
                <<"\",\"backingObjectsDestroyed\":false,\"recipientReferencesIndependent\":true}\n";
        }
        return {status, 0, 0};
    }
    void finishCompositorOutput() {
        if (compositorOutput) { if (!compositorOutput->stop()) ++cleanupFailures; compositorOutput.reset(); }
    }
    std::uint64_t compositorFrameCount() const { return completedCompositorFrames; }
    void releaseGuestMemory() {
        finishCompositorOutput();
        compositorPresenter.reset();
        if (!allocations.empty() || !vendorAllocations.empty()) return;
        copyQueue.Reset(); copyFence.Reset(); copyHeap.Reset(); copyDevice.Reset();
        if (copyEvent) {
            if (CloseHandle(copyEvent)) copyEvent = nullptr;
            else ++cleanupFailures;
        }
        if (guestMemory) {
            if (UnmapViewOfFile(guestMemory)) guestMemory = nullptr;
            else ++cleanupFailures;
        }
        if (guestSection) {
            if (CloseHandle(guestSection)) guestSection = nullptr;
            else ++cleanupFailures;
        }
    }
    bool clean() const {
        return !asyncRetirementFailed && pendingSubmissions.empty() && pendingHwSignals.empty() && pendingHwWaits.empty() && pendingContextSignals.empty() && !activeAdapters && !activeDevices && !activeContexts && pagingFences.empty() && deviceExecutionStates.empty() && allocations.empty() && vendorAllocations.empty() &&
               !vendorMappedPages && !vendorResidencyAttempts && !retainedResidencyReferences && !vendorCpuBytes && deviceAdapters.empty() && translatedAllocations.empty() &&
               sharedResourceIds.empty() && resourceGuestRuntime.empty() && sharedResourceHandles.empty() && textureConsumers.empty() && contextOwners.empty() && hwQueues.empty() && syncObjects.empty() && sharedSyncHandles.empty() && gpuReservations.empty() && !gpuReservedBytes && gpuStates.empty() && ownedAdapters.empty() &&
               !compositorOutput && !compositorPresenter && !guestMemory && !guestSection && !copyDevice && !copyHeap && !copyQueue && !copyFence && !copyEvent && !cleanupFailures &&
               nativeWorkers.started==nativeWorkers.exited && !nativeWorkers.failed && !nativeWorkers.forced;
    }
    void reportCleanup() const {
        std::cerr << "{\"driverCleanupVerified\":" << (clean() ? "true" : "false")
                  << ",\"startedNativeGpuWorkers\":" << nativeWorkers.started << ",\"exitedNativeGpuWorkers\":" << nativeWorkers.exited
                  << ",\"liveNativeGpuWorkers\":" << (nativeWorkers.started-nativeWorkers.exited)
                  << ",\"forcedNativeGpuWorkers\":" << nativeWorkers.forced << ",\"failedNativeGpuWorkers\":" << nativeWorkers.failed
                  << ",\"transferredNativeTextureHandles\":" << nativeWorkers.transferred
                  << ",\"liveAdapters\":" << activeAdapters << ",\"liveDevices\":" << activeDevices
                  << ",\"completedDeviceExecutionQueries\":"<<completedDeviceExecutionQueries
                  << ",\"failedDeviceExecutionQueries\":"<<failedDeviceExecutionQueries
                  << ",\"liveDeviceExecutionStates\":"<<deviceExecutionStates.size()
                  << ",\"liveContexts\":" << activeContexts
                  << ",\"liveSharedResourceHandles\":" << sharedResourceHandles.size() << ",\"exportedResourceHandles\":" << exportedResourceHandles
                  << ",\"closedResourceHandles\":" << closedResourceHandles
                  << ",\"peakSharedResourceHandles\":"<<peakSharedResourceHandles<<",\"sharedResourceReferenceLimit\":"<<sharedResourceLimit()
                  << ",\"importedResourceFdObjects\":" << importedResourceFdObjects
                  << ",\"liveSharedSyncHandles\":" << sharedSyncHandles.size() << ",\"exportedSyncHandles\":" << exportedSyncHandles
                  << ",\"peakSharedSyncHandles\":"<<peakSharedSyncHandles<<",\"sharedSyncReferenceLimit\":"<<sharedSyncLimit()
                  << ",\"closedSyncHandles\":" << closedSyncHandles << ",\"importedSyncObjects\":" << importedSyncObjects
                  << ",\"liveTextureConsumers\":" << textureConsumers.size()
                  << ",\"openedTextureConsumers\":" << openedTextureConsumers
                  << ",\"releasedTextureConsumers\":" << releasedTextureConsumers
                  << ",\"completedSharedTextureConsumes\":" << completedSharedConsumes
                  << ",\"failedSharedTextureConsumes\":" << failedSharedConsumes
                  << ",\"completedCompositorFrames\":" << completedCompositorFrames << ",\"failedCompositorFrames\":" << failedCompositorFrames << ",\"compositorBackpressure\":" << compositorBackpressure
                  << ",\"compositorCaptureAttempts\":" << compositorCaptureAttempts
                  << ",\"completedCompositorCaptures\":" << completedCompositorCaptures
                  << ",\"failedCompositorCaptures\":" << failedCompositorCaptures
                  << ",\"sharedResourceProbes\":" << sharedResourceProbes
                  << ",\"importedSharedResources\":" << importedSharedResources
                  << ",\"failedSharedResourceProbes\":" << failedSharedResourceProbes
                  << ",\"sharedBufferProbes\":" << sharedBufferProbes
                  << ",\"importedSharedBuffers\":" << importedSharedBuffers
                  << ",\"failedSharedBufferProbes\":" << failedSharedBufferProbes
                  << ",\"completedContextPriorityChanges\":" << completedContextPriorities
                  << ",\"failedContextPriorityChanges\":" << failedContextPriorities
                  << ",\"liveHwQueues\":" << hwQueues.size() << ",\"completedHwQueues\":" << completedHwQueues
                  << ",\"hwQueueLimit\":"<<hwQueueLimit()<<",\"peakHwQueues\":"<<peakHwQueues<<",\"hwQueueQuotaRejections\":"<<hwQueueQuotaRejections
                  << ",\"completedNoBroadcastSignalHwQueues\":" << completedNoBroadcastSignalHwQueues
                  << ",\"completedNoBroadcastWaitHwQueues\":" << completedNoBroadcastWaitHwQueues
                  << ",\"destroyedHwQueues\":" << destroyedHwQueues << ",\"failedHwQueues\":" << failedHwQueues
                  << ",\"hwQueuesReleasedAfterVmExit\":" << hwQueuesReleasedAfterVmExit
                  << ",\"liveSyncObjects\":" << syncObjects.size() << ",\"completedSyncObjects\":" << completedSyncs
                  << ",\"peakSyncObjects\":"<<peakSyncObjects<<",\"syncQuotaRejections\":"<<syncQuotaRejections
                  << ",\"destroyedSyncObjects\":" << destroyedSyncs << ",\"failedSyncObjects\":" << failedSyncs
                  << ",\"completedMonitoredFences\":" << monitoredSyncs << ",\"completedSynchronizationMutexes\":" << mutexSyncs
                  << ",\"completedNoGpuAccessFences\":" << noGpuAccessSyncs << ",\"syncObjectLimit\":" << syncObjectLimit()
                  << ",\"completedNoSignalMaxValueOnTdrFences\":" << noSignalMaxValueOnTdrSyncs
                  << ",\"nativeContextSignalAttempts\":" << contextSignalAttempts
                  << ",\"completedNativeContextSignals\":" << completedContextSignals
                  << ",\"failedNativeContextSignals\":" << failedContextSignals
                  << ",\"nativeContextSignalTimeouts\":" << contextSignalTimeouts
                  << ",\"acceptedAsyncContextSignals\":" << acceptedContextSignals << ",\"retiredAsyncContextSignals\":" << retiredAsyncContextSignals
                  << ",\"pendingAsyncContextSignals\":" << pendingContextSignals.size() << ",\"peakPendingAsyncContextSignals\":" << peakPendingContextSignals
                  << ",\"syncObjectsReleasedAfterVmExit\":" << syncsReleasedAfterVmExit
                  << ",\"liveGpuReservations\":" << gpuReservations.size() << ",\"liveGpuReservedBytes\":" << gpuReservedBytes
                  << ",\"peakGpuReservations\":"<<peakReservations<<",\"gpuReservationCountLimit\":"<<reservationLimit()
                  << ",\"gpuReservationByteLimit\":"<<reservationByteLimit()<<",\"gpuReservationQuotaRejections\":"<<reservationQuotaRejections
                  << ",\"peakGpuReservedBytes\":" << peakGpuReservedBytes << ",\"completedGpuReservations\":" << completedReservations
                  << ",\"freedGpuReservations\":" << freedReservations << ",\"failedGpuReservations\":" << failedReservations
                  << ",\"gpuReservationsReleasedAfterVmExit\":" << reservationsReleasedAfterVmExit
                  << ",\"gpuStateMappingOptIn\":" << (gpuStateEnabled ? "true" : "false")
                  << ",\"completedGpuStateMaps\":" << completedGpuStateMaps << ",\"failedGpuStateMaps\":" << failedGpuStateMaps
                  << ",\"completedGpuStateWaits\":" << completedGpuStateWaits
                  << ",\"gpuStateReplacedAllocationMappings\":" << gpuStateReplacedAllocationMappings
                  << ",\"liveGpuStateRanges\":" << gpuStates.size() << ",\"liveGpuStateBytes\":" << gpuStates.bytes()
                  << ",\"commandSubmissionOptIn\":" << (submitEnabled ? "true" : "false")
                  << ",\"asynchronousSubmissionOptIn\":" << (asyncSubmitEnabled ? "true" : "false")
                  << ",\"acceptedAsyncSubmissions\":" << acceptedAsyncSubmissions
                  << ",\"pendingNativeSubmissions\":" << pendingSubmissions.size()
                  << ",\"peakPendingNativeSubmissions\":" << peakPendingSubmissions
                  << ",\"nativeHwQueueSignalAttempts\":" << hwQueueSignalAttempts << ",\"acceptedHwQueueSignals\":" << acceptedHwQueueSignals
                  << ",\"completedHwQueueSignals\":" << completedHwQueueSignals << ",\"failedHwQueueSignals\":" << failedHwQueueSignals
                  << ",\"pendingHwQueueSignals\":" << pendingHwSignals.size() << ",\"peakPendingHwQueueSignals\":" << peakPendingHwQueueSignals
                  << ",\"acceptedGpuQueueWaits\":" << acceptedHwQueueWaits << ",\"satisfiedGpuQueueWaits\":" << satisfiedHwQueueWaits
                  << ",\"failedGpuQueueWaits\":" << failedHwQueueWaits << ",\"pendingGpuQueueWaits\":" << pendingHwWaits.size()
                  << ",\"peakPendingGpuQueueWaits\":" << peakPendingHwQueueWaits
                  << ",\"nativeSubmissionAttempts\":" << submissionAttempts << ",\"completedNativeSubmissions\":" << completedSubmissions
                  << ",\"failedNativeSubmissions\":" << failedSubmissions << ",\"nativeSubmissionTimeouts\":" << submissionTimeouts
                  << ",\"submittedCommandBytes\":" << submittedCommandBytes
                  << ",\"livePagingQueues\":" << pagingFences.size() << ",\"liveAllocations\":" << allocations.size()
                  << ",\"allocatedBytes\":" << allocatedBytes << ",\"cleanupFailures\":" << cleanupFailures
                  << ",\"liveGuestMappings\":" << (guestMemory ? 1 : 0) << ",\"liveGuestSections\":" << (guestSection ? 1 : 0)
                  << ",\"liveGpuCopyObjects\":" << (!!copyDevice + !!copyHeap + !!copyQueue + !!copyFence + !!copyEvent)
                  << ",\"completedGpuCopies\":" << completedCopies << ",\"gpuCopiedBytes\":" << gpuCopiedBytes
                  << ",\"lastGpuCopyHresult\":" << static_cast<std::uint32_t>(lastCopyError)
                  << ",\"completedAdapterQueries\":" << completedQueries
                  << ",\"failedAdapterQueries\":" << failedQueries
                  << ",\"liveVendorAllocations\":" << vendorAllocations.size()
                  << ",\"peakVendorAllocationObjects\":" << peakVendorAllocationObjects
                  << ",\"vendorAllocationObjectLimit\":" << vendorObjectLimit()
                  << ",\"vendorCpuSlotLimit\":" << (runtime ? runtime->allocationSlotLimit() : DefaultVendorCpuSlots)
                  << ",\"vendorCpuMappedByteLimit\":" << vendorCpuByteLimit(capabilities().flags,openglDesktop4kCpuEnabled,openGlProfileVersion==14)
                  << ",\"vendorCpuSlotQuotaRejections\":" << vendorCpuSlotQuotaRejections
                  << ",\"completedVendorAllocations\":" << completedVendorAllocations
                  << ",\"completedVendorResources\":" << completedVendorResources
                  << ",\"destroyedVendorResources\":" << destroyedVendorResources
                  << ",\"liveVendorResources\":" << std::count_if(vendorAllocations.begin(), vendorAllocations.end(), [](const auto& item) { return item.second.resource != 0; })
                  << ",\"completedVendorDriverProtectionMaps\":" << completedVendorDriverProtectionMaps
                  << ",\"completedVendorUninitializedSourceAllocations\":" << uninitializedSourceAllocations
                  << ",\"destroyedVendorAllocations\":" << destroyedVendorAllocations
                  << ",\"allocationRetirementOptIn\":" << (retirementEnabled ? "true" : "false")
                  << ",\"vendorDestructionsWithHwQueues\":" << vendorDestructionsWithHwQueues
                  << ",\"failedVendorAllocations\":" << failedVendorAllocations
                  << ",\"nativeGpuBufferMetadataMemoryRefusals\":" << nativeGpuBufferMetadataMemoryRefusals
                  << ",\"nativeGpuBufferAdmissionQueries\":" << nativeGpuBufferAdmissionQueries
                  << ",\"nativeGpuBufferAdmissionRefusals\":" << nativeGpuBufferAdmissionRefusals
                  << ",\"failedNativeGpuBufferAdmissionQueries\":" << failedNativeGpuBufferAdmissionQueries
                  << ",\"completedVendorGpuVaMaps\":" << completedVendorMaps
                  << ",\"failedVendorGpuVaMaps\":" << failedVendorMaps
                  << ",\"completedVendorGpuVaWaits\":" << completedVendorMapWaits
                  << ",\"liveVendorMappedPages\":" << vendorMappedPages
                  << ",\"peakVendorMappedPages\":" << peakVendorMappedPages
                  << ",\"peakStandaloneMappedPages\":" << peakStandaloneMappedPages
                  << ",\"reservationMappingAccountingSeparated\":" << (allowsReservationBackedMappings()?"true":"false")
                  << ",\"vendorGpuMappedByteLimit\":" << (std::uint64_t{vendorGpuPageLimit(capabilities().flags,openglDesktop4kEnabled,openglDesktopLargeMapEnabled)}*4096+(allowsReservationBackedMappings()?reservationByteLimit():0))
                  << ",\"standaloneGpuMappedByteLimit\":" << std::uint64_t{vendorGpuPageLimit(capabilities().flags,openglDesktop4kEnabled,openglDesktopLargeMapEnabled)}*4096
                  << ",\"completedVendorResidencyRequests\":" << completedVendorResidency
                  << ",\"completedMemoryBudgetQueries\":" << completedMemoryBudgetQueries
                  << ",\"failedMemoryBudgetQueries\":" << failedMemoryBudgetQueries
                  << ",\"deferredResidencyOptIn\":" << (deferredResidencyEnabled?"true":"false")
                  << ",\"deferredEvictionRequests\":" << deferredEvictionRequests
                  << ",\"liveRetainedResidencyReferences\":" << retainedResidencyReferences
                  << ",\"peakRetainedResidencyReferences\":" << peakRetainedResidencyReferences
                  << ",\"reusedResidencyReferences\":" << reusedResidencyReferences
                  << ",\"reclaimedResidencyReferences\":" << reclaimedResidencyReferences
                  << ",\"destroyedRetainedResidencyReferences\":" << destroyedRetainedResidencyReferences
                  << ",\"gpuVaQuotaRejections\":" << gpuVaQuotaRejections
                  << ",\"attemptedVendorEvictions\":" << attemptedVendorEvictions
                  << ",\"completedVendorEvictions\":" << completedVendorEvictions
                  << ",\"failedVendorEvictions\":" << failedVendorEvictions
                  << ",\"failedVendorResidencyRequests\":" << failedVendorResidency
                  << ",\"completedVendorResidencyWaits\":" << completedVendorResidencyWaits
                  << ",\"vendorAllocationsMadeResident\":" << vendorAllocationsMadeResident
                  << ",\"liveVendorResidencyAttempts\":" << vendorResidencyAttempts
                  << ",\"peakVendorResidencyAttempts\":" << peakVendorResidencyAttempts
                  << ",\"completedVendorCpuLocks\":" << completedVendorLocks
                  << ",\"completedVendorCpuUnlocks\":" << completedVendorUnlocks
                  << ",\"completedVendorCpuSubrangeLocks\":" << completedVendorSubrangeLocks
                  << ",\"failedVendorCpuLocks\":" << failedVendorLocks
                  << ",\"failedVendorCpuUnlocks\":" << failedVendorUnlocks
                  << ",\"liveVendorCpuBytes\":" << vendorCpuBytes
                  << ",\"vendorCpuByteQuotaRejections\":"<<vendorCpuByteQuotaRejections
                  << ",\"gpuStateRetirementWaits\":"<<gpuStateRetirementWaits<<",\"gpuStateRetirementTimeouts\":"<<gpuStateRetirementTimeouts
                  << ",\"ntReferencesClosedDuringGpuWork\":"<<ntReferencesClosedDuringGpuWork
                  << ",\"peakVendorCpuBytes\":" << peakVendorCpuBytes
                  << ",\"vendorCpuContentsChanged\":" << vendorCpuContentsChanged
                  << ",\"cpuAllocationFingerprintsEnabled\":"<<(cpuStoreTest ? "true" : "false")
                  << ",\"cpuAllocationFingerprintCalls\":"<<cpuAllocationFingerprintCalls
                  << ",\"cpuAllocationFingerprintBytes\":"<<cpuAllocationFingerprintBytes
                  << ",\"cpuStoreTestRequested\":" << (cpuStoreTest ? "true" : "false")
                  << ",\"completedCpuStoreTests\":" << completedCpuStoreTests << ",\"failedCpuStoreTests\":" << failedCpuStoreTests
                  << ",\"cpuLocksReleasedAfterVmExit\":" << cpuLocksReleasedAfterVmExit
                  << ",\"completedAllocationTranslations\":" << completedTranslations << ",\"failedAllocationTranslations\":" << failedTranslations
                  << ",\"liveAllocationTranslations\":" << translatedAllocations.size()
                  << ",\"peakReportedGpuUsageBytes\":" << peakReportedGpuUsage
                  << ",\"reportedGpuUsageLimitBytes\":" << reportedGpuUsageLimit()
                  << ",\"hardVendorAllocationByteQuotaImplemented\":false}\n";
    }
};
struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
struct Winsock {
    Winsock() { WSADATA w{}; if (WSAStartup(MAKEWORD(2, 2), &w)) throw std::runtime_error("WSAStartup failed"); }
    ~Winsock() { WSACleanup(); }
};
class Stream {
    SOCKET socket;
public:
    explicit Stream(SOCKET s = INVALID_SOCKET) : socket(s) {}
    SOCKET transport() const { return socket; }
    bool read(void* buffer, std::size_t length, bool cleanEof = false) {
        auto p = static_cast<char*>(buffer); std::size_t done = 0;
        while (done != length) {
            int n = socket == INVALID_SOCKET ? static_cast<int>(std::fread(p + done, 1, length - done, stdin))
                                             : recv(socket, p + done, static_cast<int>(length - done), 0);
            if (!n && !done && cleanEof) return false;
            // QEMU can reset its chardev socket during VM poweroff. At a
            // frame boundary this is a disconnect, with the same teardown as
            // EOF. A reset in the middle of a frame remains a protocol failure.
            if (socket != INVALID_SOCKET && n == SOCKET_ERROR && !done && cleanEof &&
                WSAGetLastError() == WSAECONNRESET) return false;
            if (n <= 0) throw std::runtime_error("Truncated frame, disconnect, or receive timeout");
            done += static_cast<std::size_t>(n);
        }
        return true;
    }
    void write(const void* buffer, std::size_t length) {
        auto p = static_cast<const char*>(buffer); std::size_t done = 0;
        while (done != length) {
            int n = socket == INVALID_SOCKET ? static_cast<int>(std::fwrite(p + done, 1, length - done, stdout))
                                             : send(socket, p + done, static_cast<int>(length - done), 0);
            if (n <= 0) throw std::runtime_error("Write failed"); done += static_cast<std::size_t>(n);
        }
        if (socket == INVALID_SOCKET && std::fflush(stdout)) throw std::runtime_error("Flush failed");
    }
    void writeFrame(const std::vector<std::uint8_t>& reply) {
        driver_transport::writeFrame(reply.data(),reply.size(),[&](const driver_transport::WriteBuffer* views,std::size_t count) {
            if(socket==INVALID_SOCKET) return std::fwrite(views[0].bytes,1,views[0].size,stdout);
            std::array<WSABUF,2> buffers{};
            for(std::size_t index=0;index<count;++index) {
                buffers[index].buf=const_cast<char*>(views[index].bytes);
                buffers[index].len=static_cast<ULONG>(views[index].size);
            }
            DWORD sent=0;
            if(WSASend(socket,buffers.data(),static_cast<DWORD>(count),&sent,0,nullptr,nullptr)==SOCKET_ERROR)
                throw std::runtime_error("Framed socket write failed");
            return static_cast<std::size_t>(sent);
        });
        if(socket==INVALID_SOCKET && std::fflush(stdout)) throw std::runtime_error("Framed flush failed");
    }
};
static void serve(Stream& stream, const std::string& sectionName = {}, std::uint32_t sectionBytes = 0,
                  bool contexts = false, bool queries = false, driver_qemu::Runtime* runtime = nullptr, bool allocations = false, bool gpuVa = false, bool residency = false, bool cpu = false, bool cpuStoreTest = false, bool translation = false, bool hwQueues = false, bool sync = false, bool submit = false, bool retirement = false, bool reservation = false, bool gpuState = false, bool asyncSubmit = false, bool sharedResources = false, bool consumeShared = false, bool presentShared = false, bool nativeMetadata = false, bool consumeProcess = false, bool coordinated = false, unsigned sdlFailureTest = 0, bool deferredResidency = false, std::uint32_t actorSlots = 4, std::uint32_t resourceReferences = 16, bool mappedAdmission = false, bool applicationMemory = false, bool applicationObjects = false, driver_launcher::Lifetime* persistentLauncher = nullptr) {
    if(persistentLauncher && (!runtime || !coordinated)) throw std::runtime_error("Persistent session requires the owned coordinator");
    KmtDriver driver(sectionName, sectionBytes, contexts, queries, runtime, allocations, gpuVa, residency, cpu, cpuStoreTest, translation, hwQueues, sync, submit, retirement, reservation, gpuState, asyncSubmit, sharedResources, consumeShared, presentShared, nativeMetadata,consumeProcess);
    driver.configureDeferredResidency(deferredResidency);
    driver.configureMappedResidencyAdmission(mappedAdmission);
    driver.configureApplicationMemory(applicationMemory);
    driver.configureApplicationObjects(applicationObjects);
    std::exception_ptr failure;
    if(sdlFailureTest==2) {
        const auto control=runtime->sdlControl();
        const auto original=control->command;
        control->command=[original](const std::string& instruction) {
            if(instruction=="detach:1") throw std::runtime_error("Injected SDL detach failure; diagnostic only");
            return original(instruction);
        };
    }
    if(coordinated) {
        {
            coordinator::Runtime owner(driver,actorSlots,resourceReferences,applicationMemory,applicationObjects);
            std::uint64_t count=0;std::size_t peakObjects=0,peakActors=0;
            try {
                const unsigned seconds=runtime && runtime->fenceSlotLimit()==512 ? 240u : 120u;
                if(!persistentLauncher) std::cerr<<"{\"nativeCoordinatorStartupBudget\":true,\"seconds\":"<<seconds<<",\"performanceAccepted\":false}\n";
                const auto deadline=GetTickCount64()+seconds*1000ull;
                std::unique_ptr<driver_launcher::SessionStream> sessionReader;
                if(persistentLauncher) {
                    static_assert(coordinator::MaxPacket<=UINT32_MAX,"Session packet length representation");
                    sessionReader=std::make_unique<driver_launcher::SessionStream>(stream.transport(),static_cast<std::uint32_t>(sizeof(coordinator::Frame)),static_cast<std::uint32_t>(coordinator::MaxPacket),
                        [runtime,persistentLauncher] {
                            if(persistentLauncher->cancelled() || runtime->guestExited()) return false;
                            persistentLauncher->requireLive();return true;
                        });
                    std::cerr<<"{\"nativeCoordinatorLauncherSession\":true,\"idleDeadlineSeconds\":0,\"packetDeadlineMilliseconds\":10000,\"heldLauncherRequired\":true,\"guestSessionVerified\":false}\n";
                }
                for(;;) {
                    std::vector<std::uint8_t> packet;
                    if(sessionReader) { if(!sessionReader->packet(packet)) break; }
                    else {
                        if(GetTickCount64()>=deadline) throw std::runtime_error("Coordinated owned runtime diagnostic deadline exceeded");
                        std::uint32_t size=0;if(!stream.read(&size,sizeof size,true)) break;
                        if(size<sizeof(coordinator::Frame) || size>coordinator::MaxPacket) throw std::runtime_error("Invalid coordinated stream frame length");
                        packet.resize(size);stream.read(packet.data(),packet.size());
                    }
                    const auto reply=owner.dispatch(packet);
                    peakObjects=(std::max)(peakObjects,owner.objectCount());peakActors=(std::max)(peakActors,owner.actorCount());++count;
                    if(owner.failed() || reply.empty()) throw std::runtime_error("Coordinated controller protocol failure");
                    if(sdlFailureTest && driver.compositorFrameCount()>=64) {
                        std::cerr<<driver_qmp::Json{{"nativeSdlFailureProbe",true},{"afterAcceptedFrames",64},
                            {"retirementFailureRequested",sdlFailureTest==2},{"syntheticFailure",true},
                            {"gpuDeviceRemoved",false},{"beforeGuestReply",true}}.dump()<<'\n';
                        throw std::runtime_error("Injected SDL coordinator failure after frame 64; diagnostic only");
                    }
                    if(sessionReader) { if(!sessionReader->reply(reply)) break; }
                    else stream.writeFrame(reply);
                }
            } catch(const std::exception& error) {
                failure=std::current_exception();
                try {
                    std::cerr<<driver_qmp::Json{{"nativeCoordinatorFailure",true},{"message",error.what()},
                        {"requestsHandled",count},{"beforeOwnedVmStop",true},{"beforeGlobalTeardown",true}}.dump()<<'\n';
                } catch(...) { std::fprintf(stderr,"COORDINATOR_FIRST_FAILURE original=%s\n",error.what()); }
            } catch(...) {
                failure=std::current_exception();
                std::fprintf(stderr,"COORDINATOR_FIRST_FAILURE nonStandardException=true beforeOwnedVmStop=true\n");
            }
            std::cerr<<"{\"nativeCoordinatorSummary\":true,\"requestsHandled\":"<<count<<",\"peakActors\":"<<peakActors
                     <<",\"peakLiveObjects\":"<<peakObjects<<",\"retainedActors\":"<<owner.actorCount()
                     <<",\"liveObjectsBeforeDisconnect\":"<<owner.objectCount()<<",\"liveRetiredActors\":"<<owner.liveRetiredActorCount()
                     <<",\"liveRetiredWireObjects\":"<<owner.liveRetiredObjectCount()<<",\"nativeDriverCount\":1,\"memoryBudgetsMultiplied\":false}\n";
            // SDL imports and queued copies must retire while its owning
            // process is alive, including after a controller/transport error.
            // Keep actor Sessions/backing pages until the VM is reaped below.
            if(runtime->sdlControl()) driver.finishCompositorOutput();
            runtime->stop(failure ? 0 : 5000);
            const auto emptied=owner.teardownAfterVmExit(runtime->hasStopped());
            std::cerr<<"{\"nativeCoordinatorTeardown\":true,\"vmReaped\":true,\"globalPhases\":4,\"allWireObjectsReleased\":"<<(emptied ? "true" : "false")<<"}\n";
        }
        driver.releaseGuestMemory();driver.reportCleanup();
        if(!driver.clean()) throw std::runtime_error("Coordinated driver object cleanup failed");
        if(failure) std::rethrow_exception(failure);
        return;
    }
    {
      Session session(driver);
      std::size_t peakWireObjects = 0;
      std::uint64_t count = 0;
      try {
      const auto deadline = GetTickCount64() + 60000;
      for (; lifetimeBudgetAvailable(count,session.requestLimit()); ++count) {
        if (runtime && GetTickCount64() >= deadline) throw std::runtime_error("Owned runtime diagnostic deadline exceeded");
        // A byte stream needs a length prefix outside the borrowed 16-byte
        // descriptor header. Both are little endian; target host/guest are x64.
        std::uint32_t size{};
        if (!stream.read(&size, sizeof size, true)) break;
        if (size < sizeof(Header) || size > native_gpu::MaxPacket) throw std::runtime_error("Invalid stream frame length");
        std::vector<std::uint8_t> packet(size); stream.read(packet.data(), packet.size());
        const auto reply = session.dispatch(packet);
        peakWireObjects = (std::max)(peakWireObjects, session.objectCount());
        if (reply.size() >= sizeof(Header)) {
            Header response{}; std::memcpy(&response, reply.data(), sizeof response);
            if (response.status == -24)
                std::cerr << "{\"wireQuotaRejection\":true,\"op\":" << response.type
                          << ",\"liveObjects\":" << session.objectCount()
                          << ",\"objectLimit\":" << session.objectLimit() << "}\n";
        }
        stream.writeFrame(reply);
      }
      if (!lifetimeBudgetAvailable(count,session.requestLimit())) throw std::runtime_error("Session request quota exceeded");
      } catch (...) { failure = std::current_exception(); }
      std::cerr << "{\"wireObjectSummary\":true,\"requestsHandled\":" << count << ",\"peakLiveObjects\":" << peakWireObjects
                << ",\"liveObjectsBeforeDisconnect\":" << session.objectCount()
                << ",\"objectLimit\":" << session.objectLimit() << "}\n";
      // Session destruction releases queue pages. A transport failure must
      // first stop the owned VM; graceful EOF waits for its normal poweroff.
      if (runtime) runtime->stop(failure ? 0 : 5000);
    }
    driver.releaseGuestMemory(); driver.reportCleanup();
    if (!driver.clean()) throw std::runtime_error("Driver object cleanup failed");
    if (failure) std::rethrow_exception(failure);
}
int main(int argc, char** argv) {
    AtomicLineLog receipts(std::cerr);
    try {
        if (argc==4 && std::string(argv[1])=="--native-texture-worker") return driver_worker::run(argv[2],argv[3]);
        // Explicit experimental opt-in; keep the existing allocation endpoint
        // closed to vendor-private context data unless requested by its owner.
        bool contexts = false, queries = false, allocations = false, gpuVa = false, residency = false, cpu = false, cpuStoreTest = false, cpuEofTest = false, translation = false, hwQueues = false, hwQueueEofTest = false, sync = false, syncEofTest = false, submit = false, retirement = false, reservation = false, reservationEofTest = false, gpuState = false, gpuStateEofTest = false, cpuSpanEofTest = false, syncNoMaxEofTest = false;
        std::size_t cpuSlots = DefaultVendorCpuSlots;
        bool cpuSlotsConfigured = false;
        std::uint32_t actorSlots=4;
        bool actorSlotsConfigured=false;
        std::uint32_t resourceReferences=16;
        bool resourceReferencesConfigured=false;
        std::uint32_t fenceSlots = 0;
        bool hwQueueNoBroadcastEofTest = false;
        bool hwQueueNoBroadcastWaitEofTest = false;
        bool asyncSubmit = false, sharedResources = false, consumeShared = false, presentShared = false, nativeMetadata = false, consumeProcess = false, coordinated = false, windowInput = false, inputSelfTest = false, sdlRelay = false;
        unsigned sdlFailureTest=0;
        bool virtioTrace=false,deferredResidency=false,userNetwork=false,mappedAdmission=false,applicationMemory=false,applicationObjects=false,launcherSession=false,bootConsole=false;
        const char* readonlyRoot = nullptr;
        const char* launcherProfileJson = nullptr;
        std::optional<driver_launcher::Profile> launcherProfile;
        std::optional<std::uintptr_t> launcherCancellation;
        while (argc > 1) {
            if (argc>2 && std::string(argv[argc-2])=="--driver-launcher-profile") {
                if (launcherProfileJson) throw std::runtime_error("Launcher profile must be specified once");
                launcherProfileJson=argv[argc-1];argc-=2;continue;
            }
            if (argc>2 && std::string(argv[argc-2])=="--driver-launcher-cancel-handle") {
                if (launcherCancellation) throw std::runtime_error("Launcher cancellation handle must be specified once");
                launcherCancellation=driver_launcher::parseHandle(argv[argc-1]);argc-=2;continue;
            }
            if(argc>2 && std::string(argv[argc-2])=="--driver-resource-reference-slots") {
                if(resourceReferencesConfigured || std::string(argv[argc-1])!="32")
                    throw std::runtime_error("Application resource reference slots must be specified once as 32");
                resourceReferences=32;resourceReferencesConfigured=true;argc-=2;continue;
            }
            if(argc>2 && std::string(argv[argc-2])=="--driver-actor-slots") {
                if(actorSlotsConfigured || std::string(argv[argc-1])!="8")
                    throw std::runtime_error("Application actor slots must be specified once as 8");
                actorSlots=8;actorSlotsConfigured=true;argc-=2;continue;
            }
            if (argc>2 && std::string(argv[argc-2])=="--driver-rootfs-readonly") {
                if (readonlyRoot || !argv[argc-1][0]) throw std::runtime_error("Read-only root image must be specified once with a nonempty path");
                readonlyRoot=argv[argc-1]; argc-=2; continue;
            }
            if (argc > 2 && std::string(argv[argc - 2]) == "--driver-fence-slots") {
                const auto value = std::string(argv[argc - 1]);
                if (fenceSlots || (value != "64" && value != "128" && value != "256" && value != "512"))
                    throw std::runtime_error("Fence aperture slots must be specified once as 64, 128 or 256; 512 requires the owned coordinator");
                fenceSlots = static_cast<std::uint32_t>(std::stoul(value)); argc -= 2; continue;
            }
            if (argc > 2 && std::string(argv[argc - 2]) == "--driver-cpu-slots") {
                const auto value = std::string(argv[argc - 1]);
                if (cpuSlotsConfigured || (value != "16" && value != "32" && value != "64" && value != "128" && value != "256" && !(gpu_diagnostic::HighResolution && (value=="512" || value=="1024"))))
                    throw std::runtime_error("CPU aperture slots must be specified once as 16, 32, 64, 128 or 256; 512/1024 require the high-resolution lab");
                cpuSlots = static_cast<std::size_t>(std::stoul(value)); cpuSlotsConfigured = true;
                argc -= 2; continue;
            }
            const auto option = std::string(argv[argc - 1]);
            if (option == "--driver-contexts" && !contexts) contexts = true;
            else if (option == "--driver-queries" && !queries) queries = true;
            else if (option == "--driver-allocations" && !allocations) allocations = true;
            else if (option == "--driver-gpuva" && !gpuVa) gpuVa = true;
            else if (option == "--driver-residency" && !residency) residency = true;
            else if (option == "--driver-cpu" && !cpu) cpu = true;
            else if (option == "--cpu-store-test" && !cpuStoreTest) cpuStoreTest = true;
            else if (option == "--cpu-eof-test" && !cpuEofTest) cpuEofTest = true;
            else if (option == "--cpu-span-eof-test" && !cpuSpanEofTest) cpuSpanEofTest = true;
            else if (option == "--driver-translation" && !translation) translation = true;
            else if (option == "--driver-hwqueues" && !hwQueues) hwQueues = true;
            else if (option == "--hwqueue-eof-test" && !hwQueueEofTest) hwQueueEofTest = true;
            else if (option == "--hwqueue-no-broadcast-eof-test" && !hwQueueNoBroadcastEofTest) hwQueueNoBroadcastEofTest = true;
            else if (option == "--hwqueue-no-broadcast-wait-eof-test" && !hwQueueNoBroadcastWaitEofTest) hwQueueNoBroadcastWaitEofTest = true;
            else if (option == "--sync-eof-test" && !syncEofTest) syncEofTest = true;
            else if (option == "--sync-no-max-eof-test" && !syncNoMaxEofTest) syncNoMaxEofTest = true;
            else if (option == "--driver-syncs" && !sync) sync = true;
            else if (option == "--driver-submit" && !submit) submit = true;
            else if (option == "--driver-async-submit" && !asyncSubmit) asyncSubmit = true;
            else if (option == "--driver-consume-shared" && !consumeShared) consumeShared = true;
            else if (option == "--driver-consume-shared-process" && !consumeProcess) consumeProcess = true;
            else if (option == "--driver-present-shared" && !presentShared) presentShared = true;
            else if (option == "--driver-native-metadata" && !nativeMetadata) nativeMetadata = true;
            else if (option == "--driver-coordinator" && !coordinated) coordinated = true;
            else if (option == "--driver-launcher-session" && !launcherSession) launcherSession = true;
            else if (option == "--driver-boot-console") {
                if (bootConsole) throw std::runtime_error("Boot console must be specified once");
                bootConsole = true;
            }
            else if (option == "--driver-window-input" && !windowInput) windowInput = true;
            else if (option == "--driver-input-self-test" && !inputSelfTest) inputSelfTest = true;
            else if (option == "--driver-sdl-relay" && !sdlRelay) sdlRelay = true;
            else if (option == "--driver-virtio-trace" && !virtioTrace) virtioTrace = true;
            else if (option == "--driver-user-network" && !userNetwork) userNetwork = true;
            else if (option == "--driver-deferred-residency" && !deferredResidency) deferredResidency = true;
            else if (option == "--driver-mapped-residency-admission" && !mappedAdmission) mappedAdmission = true;
            else if (option == "--driver-application-memory" && !applicationMemory) applicationMemory = true;
            else if (option == "--driver-application-objects" && !applicationObjects) applicationObjects = true;
            else if (option == "--driver-sdl-failure-test" && !sdlFailureTest) sdlFailureTest = 1;
            else if (option == "--driver-sdl-retirement-failure-test" && !sdlFailureTest) sdlFailureTest = 2;
            else if (option == "--driver-shared-resources" && !sharedResources) sharedResources = true;
            else if (option == "--driver-retirement" && !retirement) retirement = true;
            else if (option == "--driver-reservation" && !reservation) reservation = true;
            else if (option == "--reservation-eof-test" && !reservationEofTest) reservationEofTest = true;
            else if (option == "--driver-gpu-state" && !gpuState) gpuState = true;
            else if (option == "--gpu-state-eof-test" && !gpuStateEofTest) gpuStateEofTest = true;
            else break;
            --argc;
        }
        if (bootConsole && (!launcherSession || !launcherProfileJson || !launcherCancellation || !coordinated || !sdlRelay))
            throw std::runtime_error("Boot console requires the persistent original launcher session, lifetime control and SDL coordinator");
        if (launcherCancellation && (argc!=7 || std::string(argv[1])!="--run-qemu"))
            throw std::runtime_error("Launcher lifetime requires the owned QEMU runtime");
        if (launcherCancellation && (!contexts || !queries))
            throw std::runtime_error("Launcher lifetime requires explicit query/context opt-ins");
        if(launcherSession && (!launcherProfileJson || !launcherCancellation || !coordinated || !sdlRelay))
            throw std::runtime_error("Launcher session requires an original profile, inherited lifetime control and SDL coordinator");
        if (launcherProfileJson) {
            if (!launcherCancellation) throw std::runtime_error("Launcher profile requires inherited lifetime control");
            launcherProfile=driver_launcher::Profile::parse(launcherProfileJson);
            if (!gpu_diagnostic::HighResolution || !coordinated || !sdlRelay || readonlyRoot || windowInput || inputSelfTest ||
                virtioTrace || userNetwork || sdlFailureTest || consumeProcess || cpuStoreTest || cpuEofTest || cpuSpanEofTest ||
                hwQueueEofTest || syncEofTest || syncNoMaxEofTest || reservationEofTest || gpuStateEofTest ||
                hwQueueNoBroadcastEofTest || hwQueueNoBroadcastWaitEofTest || cpuSlots!=DesktopVendorCpuSlots || fenceSlots!=512 ||
                !actorSlotsConfigured || actorSlots!=8 || !resourceReferencesConfigured || resourceReferences!=32 ||
                !mappedAdmission || !applicationMemory || !applicationObjects || !deferredResidency)
                throw std::runtime_error("Launcher profile requires its complete native desktop contract and official input");
            driver_capture::configureProfileDirectory(launcherProfile->auditDirectory);
        }
        if (readonlyRoot && (!gpu_diagnostic::HighResolution || argc!=7 || std::string(argv[1])!="--run-qemu"))
            throw std::runtime_error("Read-only root image requires the diagnostic build and owned QEMU runtime");
        if(windowInput && !coordinated) throw std::runtime_error("Window input requires the owned read-only root desktop coordinator");
        if(inputSelfTest && !windowInput) throw std::runtime_error("Input self-test requires explicit window input");
        if(sdlRelay && (!coordinated || (windowInput && !inputSelfTest)))
            throw std::runtime_error("SDL graphics requires the owned coordinator; optional QMP input must be the isolated self-test");
        if(sdlFailureTest && (!gpu_diagnostic::HighResolution || !sdlRelay || !coordinated || !readonlyRoot))
            throw std::runtime_error("SDL failure tests require the explicit high-resolution owned SDL coordinator and read-only root");
        if(virtioTrace && (!gpu_diagnostic::HighResolution || !sdlRelay || !coordinated || !readonlyRoot))
            throw std::runtime_error("Virtio trace requires the explicit high-resolution owned SDL coordinator and read-only root");
        if(userNetwork && (!gpu_diagnostic::HighResolution || !sdlRelay || !coordinated || !readonlyRoot))
            throw std::runtime_error("User networking requires the explicit high-resolution owned SDL coordinator and read-only root");
        if(actorSlotsConfigured && (!gpu_diagnostic::HighResolution || !sdlRelay || !coordinated || (!readonlyRoot && !launcherProfile) || cpuSlots!=DesktopVendorCpuSlots || fenceSlots!=512))
            throw std::runtime_error("Application actor capacity requires the explicit full owned SDL desktop and read-only root");
        if(resourceReferencesConfigured && !actorSlotsConfigured)
            throw std::runtime_error("Application resource references require the explicit application actor capacity");
        if(applicationObjects && !applicationMemory)
            throw std::runtime_error("Application objects require explicit application memory permission");
        if(applicationMemory && !mappedAdmission)
            throw std::runtime_error("Application memory requires explicit mapped residency admission and application capacity");
        if(mappedAdmission && (!deferredResidency || !resourceReferencesConfigured || !actorSlotsConfigured))
            throw std::runtime_error("Mapped residency admission requires explicit application actors, references and retaining residency");
        if(deferredResidency && (!gpu_diagnostic::HighResolution || !sdlRelay || !coordinated || (!readonlyRoot && !launcherProfile) || !residency || !retirement))
            throw std::runtime_error("Deferred residency requires the explicit high-resolution owned SDL coordinator, residency, retirement and read-only root");
        if (readonlyRoot && (cpuStoreTest || cpuEofTest || cpuSpanEofTest || hwQueueEofTest || syncEofTest || syncNoMaxEofTest ||
            reservationEofTest || gpuStateEofTest || hwQueueNoBroadcastEofTest || hwQueueNoBroadcastWaitEofTest))
            throw std::runtime_error("Read-only root image cannot be combined with forced EOF/store controls");
        if(cpuSlots==DesktopVendorCpuSlots && (!coordinated || !cpu || fenceSlots!=512)) throw std::runtime_error("1024 CPU slots require the owned coordinator, CPU locks and 512 fence slots");
        if(fenceSlots==512 && !coordinated) throw std::runtime_error("512 fence slots require the explicit owned coordinator");
        if (cpuSlots==SparseVendorCpuSlots && (!cpu || (fenceSlots!=256 && !(coordinated && fenceSlots==512)) || argc!=7 || std::string(argv[1])!="--run-qemu"))
            throw std::runtime_error("Sparse CPU aperture requires CPU locks, explicit 256 fence slots and an owned QEMU runtime");
        if(coordinated && (!gpu_diagnostic::HighResolution || argc!=7 || std::string(argv[1])!="--run-qemu" || (!readonlyRoot && !launcherProfile) ||
            (cpuSlots!=SparseVendorCpuSlots && cpuSlots!=DesktopVendorCpuSlots) || (fenceSlots!=256 && fenceSlots!=512) || !nativeMetadata || !presentShared || !consumeShared || !sharedResources ||
            !asyncSubmit || !sync || !gpuState || !retirement || consumeProcess || cpuStoreTest || cpuEofTest || cpuSpanEofTest ||
            hwQueueEofTest || syncEofTest || syncNoMaxEofTest || reservationEofTest || gpuStateEofTest || hwQueueNoBroadcastEofTest || hwQueueNoBroadcastWaitEofTest))
            throw std::runtime_error("Coordinator requires the high-resolution owned read-only root runtime, exact sparse apertures and complete native presentation opt-ins");
        if (gpu_diagnostic::HighResolution) {
            if (!nativeMetadata || !consumeShared || !cpu || cpuSlots < ExpandedVendorCpuSlots ||
                argc != 7 || std::string(argv[1]) != "--run-qemu")
                throw std::runtime_error("4K lab build requires native metadata, consumption and 128, 256, 512 or coordinated 1024 CPU slots in an owned QEMU runtime");
            (void)driver_capture::directory(); // Validate capture destination before GPU work.
        }
        if (allocations && (!contexts || !queries)) throw std::runtime_error("Vendor allocations require explicit query/context opt-ins");
        if (nativeMetadata && !sharedResources) throw std::runtime_error("Native texture metadata requires explicit shared resource opt-in");
        if (presentShared && !consumeShared) throw std::runtime_error("Shared texture presentation requires explicit consumption opt-in");
        if (consumeProcess && !consumeShared) throw std::runtime_error("Shared GPU worker requires explicit consumption opt-in");
        if (consumeShared && (!sharedResources || !asyncSubmit || !sync)) throw std::runtime_error("Shared texture consumption requires shared resources, asynchronous submission and sync opt-ins");
        if (sharedResources && !allocations) throw std::runtime_error("Shared resources require explicit allocation opt-in");
        if (sharedResources && (argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Shared resources require an owned QEMU runtime");
        if (gpuVa && !allocations) throw std::runtime_error("Vendor GPU-address mappings require explicit allocation opt-in");
        if (residency && !allocations) throw std::runtime_error("Vendor residency requires explicit allocation opt-in");
        if (translation && !allocations) throw std::runtime_error("Allocation translation requires explicit allocation opt-in");
        if (hwQueues && (!contexts || !translation || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Hardware queues require context and allocation translation opt-ins in an owned QEMU runtime");
        if (sync && (argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Synchronization objects require an owned QEMU runtime");
        if (retirement && !hwQueues)
            throw std::runtime_error("Allocation retirement requires owned hardware queues");
        if (reservation && (!gpuVa || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("GPU reservation requires GPU-address mappings in an owned QEMU runtime");
        if (syncNoMaxEofTest && (!sync || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest))
            throw std::runtime_error("NoSignalMaxValueOnTdr EOF control requires syncs and a separate diagnostic run");
        if (reservationEofTest && (!reservation || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest))
            throw std::runtime_error("Reservation EOF control requires reservation opt-in and a separate diagnostic run");
        if (gpuState && !reservation) throw std::runtime_error("GPU state mappings require the owned reservation opt-in");
        if (cpuSpanEofTest && (!cpu || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest || gpuStateEofTest))
            throw std::runtime_error("CPU span EOF control requires CPU locks and a separate diagnostic run");
        if (gpuStateEofTest && (!gpuState || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest))
            throw std::runtime_error("GPU state EOF control requires state mappings and a separate diagnostic run");
        if (submit && (!hwQueues || !sync || !cpu || !residency || cpuStoreTest || cpuEofTest || hwQueueEofTest || syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest || syncNoMaxEofTest))
            throw std::runtime_error("Submission requires owned queues, syncs, CPU mappings and residency in a separate diagnostic run");
        if (asyncSubmit && (!submit || !retirement || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Asynchronous submission requires submission, retirement and an owned QEMU runtime");
        if (cpu && (!gpuVa || argc != 7 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Vendor CPU locks require GPU-address mappings and an owned QEMU runtime");
        if (cpuStoreTest && !cpu) throw std::runtime_error("CPU store control requires explicit CPU-lock opt-in");
        if (cpuSlotsConfigured && !cpu) throw std::runtime_error("CPU aperture capacity requires the explicit CPU-lock opt-in");
        if (fenceSlots && (!gpu_diagnostic::HighResolution || argc < 2 || std::string(argv[1]) != "--run-qemu"))
            throw std::runtime_error("Fence aperture capacity requires the diagnostic build and owned QEMU runtime");
        if (syncEofTest && (!sync || cpuStoreTest || cpuEofTest || hwQueueEofTest))
            throw std::runtime_error("Synchronization EOF control requires synchronization opt-in and a separate diagnostic run");
        if (cpuEofTest && (!cpu || cpuStoreTest)) throw std::runtime_error("CPU EOF control requires CPU locks and a separate run from store control");
        if (hwQueueEofTest && (!hwQueues || cpuEofTest || cpuStoreTest))
            throw std::runtime_error("Hardware queue EOF control requires hardware queues and a separate diagnostic run");
        if (hwQueueNoBroadcastEofTest && (!hwQueues || !submit || !retirement || cpuStoreTest || cpuEofTest || hwQueueEofTest ||
            syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest || syncNoMaxEofTest))
            throw std::runtime_error("NoBroadcastSignal queue EOF control requires submission, retirement and a separate diagnostic run");
        if (hwQueueNoBroadcastWaitEofTest && (!hwQueues || !submit || !retirement || cpuStoreTest || cpuEofTest || hwQueueEofTest ||
            syncEofTest || reservationEofTest || gpuStateEofTest || cpuSpanEofTest || syncNoMaxEofTest || hwQueueNoBroadcastEofTest))
            throw std::runtime_error("NoBroadcastWait queue EOF control requires submission, retirement and a separate diagnostic run");
        if (argc == 2 && std::string(argv[1]) == "--stdio") {
            if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
                throw std::runtime_error("Cannot set binary stdio mode");
            Stream stream; serve(stream, {}, 0, contexts, queries, nullptr, allocations, gpuVa, residency, false, false, translation); return 0;
        }
        const bool ownedRuntime = argc == 7 && std::string(argv[1]) == "--run-qemu";
        if (!ownedRuntime && ((argc != 3 && argc != 7) || std::string(argv[1]) != "--listen")) {
            std::cerr << "Usage: driver-bridge.exe --stdio | --listen port [--guest-section name --guest-ram-bytes count] "
                "[--driver-contexts] [--driver-queries] [--driver-allocations] [--driver-gpuva] [--driver-residency] [--driver-cpu] [--driver-cpu-slots 16|32|64|128|256|512|1024] [--driver-translation] [--driver-hwqueues] [--driver-syncs] [--driver-submit] [--driver-async-submit] [--driver-retirement] [--driver-reservation] [--driver-gpu-state] [--cpu-store-test | --cpu-eof-test | --cpu-span-eof-test | --hwqueue-eof-test | --hwqueue-no-broadcast-eof-test | --hwqueue-no-broadcast-wait-eof-test | --sync-eof-test | --sync-no-max-eof-test | --reservation-eof-test | --gpu-state-eof-test]\n"
                "       driver-bridge.exe --run-qemu qemu firmware kernel initramfs fresh-log --driver-contexts --driver-queries [--driver-fence-slots 64|128|256] [--driver-coordinator] [--driver-launcher-cancel-handle inherited-handle] [--driver-launcher-profile ascii-json] [--driver-launcher-session --driver-boot-console]\n"; return 2;
        }
        if (ownedRuntime && (!contexts || !queries)) throw std::runtime_error("Owned QEMU runtime requires explicit query/context opt-ins");
        std::size_t end{}; const auto port = ownedRuntime ? 0ul : std::stoul(argv[2], &end);
        if (!ownedRuntime && (end != std::string(argv[2]).size() || port > 65535)) throw std::runtime_error("Invalid port");
        std::string sectionName; std::uint32_t sectionBytes = 0;
        if (argc == 7 && !ownedRuntime) {
            if (std::string(argv[3]) != "--guest-section" || std::string(argv[5]) != "--guest-ram-bytes")
                throw std::runtime_error("Invalid guest section arguments");
            sectionName = argv[4]; const auto bytes = std::stoul(argv[6], &end);
            if (end != std::string(argv[6]).size() || bytes > 1024u * 1024u * 1024u)
                throw std::runtime_error("Invalid guest section size");
            sectionBytes = static_cast<std::uint32_t>(bytes);
        }
        Winsock winsock; Socket listener;
        std::unique_ptr<driver_qemu::Runtime> runtime;
        // Destruction order joins the watcher before destroying Runtime; the
        // callback may stop only its held guest process, not driver/map state.
        std::unique_ptr<driver_launcher::Lifetime> launcherLifetime;
        if (launcherCancellation) {
            driver_qemu::Handle creator, cancellation;
            driver_launcher::captureActualCreator(creator);
            driver_launcher::takeInheritedCancellation(*launcherCancellation,cancellation);
            launcherLifetime=std::make_unique<driver_launcher::Lifetime>(creator.value,cancellation.value);
        }
        listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener.value == INVALID_SOCKET) throw std::runtime_error("socket failed");
        const BOOL exclusive = TRUE;
        if (setsockopt(listener.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive))
            throw std::runtime_error("Cannot reserve listener exclusively");
        sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<unsigned short>(port));
        if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof address) || listen(listener.value, 1))
            throw std::runtime_error("Loopback listen failed");
        int length = sizeof address;
        if (getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &length)) throw std::runtime_error("getsockname failed");
        std::cout << "{\"port\":" << ntohs(address.sin_port) << ",\"transport\":\"tcp-loopback\"}\n" << std::flush;
        if (ownedRuntime) runtime = std::make_unique<driver_qemu::Runtime>(ntohs(address.sin_port),
            qemu_fence::Paths{argv[2], argv[3], argv[4], argv[5], argv[6]}, cpu, cpuStoreTest, cpuEofTest, hwQueueEofTest, cpuSlots, syncEofTest, reservationEofTest, gpuStateEofTest, cpuSpanEofTest, syncNoMaxEofTest, hwQueueNoBroadcastEofTest, hwQueueNoBroadcastWaitEofTest, fenceSlots,readonlyRoot,coordinated && fenceSlots==512,windowInput,inputSelfTest,sdlRelay,virtioTrace,userNetwork,
            launcherProfile ? &*launcherProfile : nullptr,[watch=launcherLifetime.get()] { if(watch) watch->requireLive(); },
            [watch=launcherLifetime.get()](const driver_qemu::ProcessBinding& binding) {
                if(!watch) throw std::runtime_error("Launcher process binding requires lifetime control");
                std::cout<<driver_launcher::bound(binding,*watch).dump()<<'\n'<<std::flush;
            },bootConsole ? driver_qemu_args::BootDisplay::StandardVga : driver_qemu_args::BootDisplay::None);
        if (launcherLifetime) {
            launcherLifetime->start([vm=runtime.get()](driver_launcher::End why) {
                vm->stopGuest();
                std::cerr << driver_qmp::Json{{"nativeLauncherStopAcknowledged",true},{"reason",driver_launcher::reason(why)},
                    {"guestExited",vm->guestExited()},{"driverCleanupComplete",false}}.dump() << '\n';
            });
            if (runtime->guestExited()) throw std::runtime_error("Owned guest exited before launcher VM identity readiness");
            std::cout << driver_launcher::ready(runtime->processBinding(),*launcherLifetime).dump() << '\n' << std::flush;
            const auto deadline = GetTickCount64()+60000;
            for (;;) {
                if (launcherLifetime->cancelled() || launcherLifetime->callbackFailed()) throw std::runtime_error("Launcher lifetime ended before guest connection");
                if (runtime->guestExited()) throw std::runtime_error("Owned guest exited before connection");
                fd_set reads; FD_ZERO(&reads); FD_SET(listener.value,&reads); timeval slice{0,250000};
                const auto selected = select(0,&reads,nullptr,nullptr,&slice);
                if (selected == 1) break;
                if (selected == SOCKET_ERROR || GetTickCount64() >= deadline) throw std::runtime_error("Connection failed or timed out");
            }
        } else {
            fd_set reads; FD_ZERO(&reads); FD_SET(listener.value, &reads); timeval timeout{60, 0};
            if (select(0, &reads, nullptr, nullptr, &timeout) != 1) throw std::runtime_error("Connection timed out");
        }
        Socket client; client.value = accept(listener.value, nullptr, nullptr);
        if (client.value == INVALID_SOCKET) throw std::runtime_error("accept failed");
        const DWORD milliseconds = 30000;
        if (setsockopt(client.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds) ||
            setsockopt(client.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds))
            throw std::runtime_error("Cannot set socket timeouts");
        const DWORD noDelay=1;DWORD observedNoDelay=0;int optionBytes=sizeof observedNoDelay;
        if(setsockopt(client.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&noDelay),sizeof noDelay) ||
           getsockopt(client.value,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&observedNoDelay),&optionBytes) ||
           !driver_transport::tcpNoDelayEnabled(observedNoDelay,optionBytes))
            throw std::runtime_error("Cannot configure low-latency GPU RPC socket");
        std::cerr<<"{\"nativeGpuTransportConfigured\":true,\"tcpNoDelay\":true,\"tcpNoDelayOptionBytes\":"<<optionBytes
            <<",\"framedReplyScatterGather\":true,\"inputPathChanged\":false}\n";
        Stream stream(client.value); serve(stream, sectionName, sectionBytes, contexts, queries, runtime.get(), allocations, gpuVa, residency, cpu, cpuStoreTest, translation, hwQueues, sync, submit, retirement, reservation, gpuState, asyncSubmit, sharedResources, consumeShared, presentShared, nativeMetadata,consumeProcess,coordinated,sdlFailureTest,deferredResidency,actorSlots,resourceReferences,mappedAdmission,applicationMemory,applicationObjects,launcherSession ? launcherLifetime.get() : nullptr);
        if (runtime) {
            runtime->report();
            if (!runtime->cleanExit()) throw std::runtime_error("Owned QEMU runtime did not exit cleanly");
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
