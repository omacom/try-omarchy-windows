// SPDX-License-Identifier: MIT
// Independent, deliberately small WDDM experiment, not the /dev/dxg or RM ABI.
#pragma once
#include "actor_retirement.h"
#include "stream_profile.h"
#include "opengl_profile.h"
#include "application_memory_profile.h"
#include "wire.h"
#include "diagnostic_profile.h"
#include "gpu_state_ranges.h"
#include "gpu_mapping_accounting.h"
#include "compositor_capture_wire.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>

namespace driver_bridge {
using native_gpu::Header;
constexpr std::uint32_t Version = 1;
// The live NVIDIA D3D12 runtime needs a fifth paging queue with 127
// objects already owned. Keep the aggregate cap separate from the tighter
// per-kind and byte budgets, including two handles per native queue.
constexpr std::size_t MaxObjects = 256;
constexpr std::uint32_t MaxAllocation = 1024 * 1024;
constexpr std::uint32_t MaxAllocatedBytes = 16 * 1024 * 1024;
constexpr std::uint32_t MaxChunk = 4064;
constexpr std::uint32_t ContextCapability = 32;
constexpr std::uint32_t MaxContextPrivateBytes = 4000;
constexpr std::uint32_t QueryCapability = 64;
constexpr std::uint32_t GuestPagingCapability = 128;
constexpr std::uint32_t VendorAllocationCapability = 256;
constexpr std::uint32_t VendorResourceCapability = 524288;
constexpr std::uint32_t SharedVendorResourceCapability = 268435456;
constexpr std::uint32_t SharedTextureConsumeCapability = 536870912;
constexpr std::uint32_t NativeSharedTextureCapability = 1073741824;
struct SharedTextureConsumeDesc { std::uint32_t sync, reserved; std::uint64_t fence; };
static_assert(sizeof(SharedTextureConsumeDesc) == 16, "fixed shared texture handoff layout");
inline bool validSharedTextureConsume(SharedTextureConsumeDesc d) { return d.sync && !d.reserved && d.fence && d.fence != UINT64_MAX; }
constexpr std::uint32_t MaxVendorPrivateBytes = 4000;
constexpr std::size_t MaxVendorAllocations = 96;
constexpr std::size_t ExpandedVendorAllocations = 128;
constexpr std::uint32_t ExpandedVendorObjectsCapability = 67108864;
inline std::size_t vendorAllocationObjectLimit(std::uint32_t capabilities) {
    return capabilities & ExpandedVendorObjectsCapability ? ExpandedVendorAllocations : MaxVendorAllocations;
}
constexpr std::size_t DefaultVendorCpuSlots = 16;
// Slots reserve guest virtual MMIO space; they do not raise the physical CPU
// byte budget. Many small UMD views otherwise consume the 128-slot layout
// before a full 4K readback can obtain a contiguous range.
constexpr std::size_t ExpandedVendorCpuSlots = 128;
constexpr std::size_t MaxVendorCpuSlots = 256; // Published v1-7 receipt ceiling.
constexpr std::size_t SparseVendorCpuSlots = 512;
constexpr std::size_t DesktopVendorCpuSlots = 1024;
inline bool validVendorCpuSlots(std::size_t slots, bool sparse=false, bool desktop=false) {
    return (!desktop || sparse) && slots >= DefaultVendorCpuSlots && slots <= (desktop ? DesktopVendorCpuSlots : sparse ? SparseVendorCpuSlots : MaxVendorCpuSlots) && !(slots & (slots - 1));
}
constexpr std::uint32_t VendorGpuVaCapability = 512;
constexpr std::uint32_t VendorResidencyCapability = 1024;
constexpr std::uint32_t VendorMemoryManagementCapability = 0x80000000u;
constexpr std::uint32_t VendorCpuCapability = 2048;
// Only explicitly selected 128/256-slot native profiles advertise this budget.
// Preserve standard allocation and 16/32/64-slot vendor CPU budgets.
constexpr std::uint32_t ExpandedVendorCpuCapability = 4194304;
constexpr std::uint32_t ExpandedVendorCpuBytes = gpu_diagnostic::ExpandedCpuBytes;
inline std::uint32_t vendorCpuByteLimit(std::uint32_t capabilities, bool desktop4kCpu=false, bool multiappCpu=false) {
    if (desktop4kCpu && (capabilities & ExpandedVendorCpuCapability)) return multiappCpu ? OpenGlMultiappCpuBytes : OpenGlDesktop4kCpuBytes;
    return (capabilities & ExpandedVendorCpuCapability) ? ExpandedVendorCpuBytes : MaxAllocatedBytes;
}
inline bool vendorCpuBudgetFits(std::uint32_t used, std::uint32_t bytes, std::uint32_t capabilities, bool desktop4kCpu=false, bool multiappCpu=false) {
    const auto limit = vendorCpuByteLimit(capabilities,desktop4kCpu,multiappCpu);
    return used <= limit && bytes <= limit - used;
}
constexpr std::uint32_t VendorTranslationCapability = 4096;
constexpr std::uint32_t HwQueueCapability = 8192;
constexpr std::uint32_t SyncCapability = 16384;
// Synchronization mutexes consume no fence BAR slots. Live queue startup
// exceeds 64 independent objects while staying inside the 64-page BAR.
constexpr std::size_t MaxSyncObjects = 96;
constexpr std::uint32_t NoSignalMaxValueOnTdrSyncFlag = 64;
constexpr std::uint32_t NoGpuAccessSyncFlag = 128;
// NT-shareable monitored fences have no global D3DKMT handle. Exporting an
// NT handle is a separate operation; creation must preserve these flags.
constexpr std::uint32_t NtSharedNoGpuAccessSyncFlags = 1 | 2 | NoGpuAccessSyncFlag;
constexpr std::uint32_t NtSharedGpuSyncFlags = 1 | 2;
constexpr std::uint32_t UnwaitCpuWaitersOnlyOnDestroySyncFlag = 1024;
constexpr std::uint32_t SharedSyncImportFlags = NtSharedNoGpuAccessSyncFlags | UnwaitCpuWaitersOnlyOnDestroySyncFlag;
constexpr std::size_t MaxSharedSyncHandles = 16;
constexpr std::uint32_t MaxPendingHwQueueWaits = 16, MaxHwQueueWaitObjects = 32;
struct SyncSharingProfile { std::uint32_t version, handleLimit, importFlags, syncLimit, pendingWaitLimit; };
constexpr SyncSharingProfile GuestSyncSharingProfile{2, MaxSharedSyncHandles, SharedSyncImportFlags, MaxSyncObjects, MaxPendingHwQueueWaits};
inline bool validSyncSharingProfile(SyncSharingProfile p) {
    return p.version == 2 && p.handleLimit == MaxSharedSyncHandles && p.importFlags == SharedSyncImportFlags &&
           p.syncLimit == MaxSyncObjects && p.pendingWaitLimit == MaxPendingHwQueueWaits;
}
struct SharedSyncOpenDesc { std::uint32_t device, flags, affinity, reserved; };
struct SharedSyncInfoDesc { std::uint32_t device, reserved; };
constexpr std::size_t MaxSharedResourceHandles = 16;
constexpr std::uint32_t MaxSharedResourceRuntimeBytes = 1024;
struct ResourceSharingProfile { std::uint32_t version, handleLimit, allocationCount, runtimeByteLimit; };
constexpr ResourceSharingProfile GuestResourceSharingProfile{1, MaxSharedResourceHandles, 1, MaxSharedResourceRuntimeBytes};
constexpr std::uint32_t ApplicationResourceReferences=32;
constexpr ResourceSharingProfile GuestApplicationResourceSharingProfile{2, ApplicationResourceReferences, 1, MaxSharedResourceRuntimeBytes};
inline bool validResourceSharingProfile(ResourceSharingProfile p) {
    return p.version == 1 && p.handleLimit == MaxSharedResourceHandles && p.allocationCount == 1 &&
           p.runtimeByteLimit == MaxSharedResourceRuntimeBytes;
}
inline bool validApplicationResourceSharingProfile(ResourceSharingProfile p) {
    return p.version==2 && p.handleLimit==ApplicationResourceReferences && p.allocationCount==1 &&
           p.runtimeByteLimit==MaxSharedResourceRuntimeBytes;
}
inline bool validCoordinatedResourceSharingProfile(ResourceSharingProfile p) {
    return validResourceSharingProfile(p) || validApplicationResourceSharingProfile(p);
}
static_assert(sizeof(ResourceSharingProfile) == 16, "fixed native resource sharing negotiation");
struct SharedResourceQueryDesc { std::uint32_t device, runtimeCapacity; };
struct SharedResourceInfo { std::uint32_t runtimeBytes, driverBytes, resourceDriverBytes, allocations; };
inline bool validSharedResourceInfo(SharedResourceInfo info) {
    return info.runtimeBytes && info.runtimeBytes <= MaxSharedResourceRuntimeBytes && info.allocations == 1 &&
        info.driverBytes && info.driverBytes <= MaxVendorPrivateBytes && info.resourceDriverBytes <= MaxVendorPrivateBytes;
}
static_assert(sizeof(SharedResourceQueryDesc) == 8 && sizeof(SharedResourceInfo) == 16, "fixed resource metadata");
struct SharedResourceOpenDesc { std::uint32_t device, runtimeBytes, driverBytes, resourceDriverBytes; };
struct SharedResourceOpenReply { std::uint32_t resource, allocationDriverBytes, allocationDriverOffset, reserved; };
inline bool validSharedResourceOpen(SharedResourceOpenDesc d) {
    return d.device && validSharedResourceInfo({d.runtimeBytes, d.driverBytes, d.resourceDriverBytes, 1}) &&
        d.runtimeBytes + d.driverBytes + d.resourceDriverBytes <= native_gpu::MaxPacket - sizeof(Header) - 16 - sizeof(SharedResourceOpenReply);
}
static_assert(sizeof(SharedResourceOpenDesc) == 16 && sizeof(SharedResourceOpenReply) == 16, "fixed resource open metadata");
static_assert(sizeof(SyncSharingProfile) == 20 && sizeof(SharedSyncOpenDesc) == 16 && sizeof(SharedSyncInfoDesc) == 8, "fixed shared fence negotiation");
struct HwQueueWaitDesc { std::uint32_t count, reserved; };
struct HwQueueWaitItem { std::uint32_t sync, reserved; std::uint64_t fence; };
static_assert(sizeof(HwQueueWaitDesc) == 8 && sizeof(HwQueueWaitItem) == 16, "fixed GPU queue wait layout");
inline bool validSharedSyncOpen(SharedSyncOpenDesc d) {
    return d.device && (d.flags == 0 || d.flags == NoGpuAccessSyncFlag || d.flags == NtSharedGpuSyncFlags ||
           d.flags == NtSharedNoGpuAccessSyncFlags || d.flags == SharedSyncImportFlags) && d.affinity <= 1 && !d.reserved;
}
inline bool packetSignalSyncFlags(std::uint32_t flags) {
    return flags == NoGpuAccessSyncFlag || flags == NtSharedNoGpuAccessSyncFlags ||
           flags == (NtSharedNoGpuAccessSyncFlags | UnwaitCpuWaitersOnlyOnDestroySyncFlag);
}
constexpr std::uint32_t ContextSignalCapability = 1048576;
constexpr std::uint32_t MaxContextSignals = 16;
struct ContextSignalDesc { std::uint32_t sync, flags; std::uint64_t fence; };
static_assert(sizeof(ContextSignalDesc) == 16, "fixed context signal layout");
inline bool validContextSignal(ContextSignalDesc d) { return d.sync && d.flags == 4 && d.fence && d.fence != UINT64_MAX; }
struct SyncDesc { std::uint32_t type, flags, affinity, reserved; std::uint64_t initial; };
struct SyncReply { std::uint64_t offset, gpuAddress; };
static_assert(sizeof(SyncDesc) == 24 && sizeof(SyncReply) == 16, "fixed synchronization layouts");
inline bool validSync(SyncDesc d) {
    return !d.reserved && ((d.type == 1 && !d.flags && !d.affinity && d.initial <= 1) ||
           (d.type == 5 && (d.flags == 0 || d.flags == NtSharedGpuSyncFlags || d.flags == NoSignalMaxValueOnTdrSyncFlag || packetSignalSyncFlags(d.flags)) && d.affinity <= 1));
}
constexpr std::size_t MaxHwQueues = 8;
constexpr std::uint32_t HwQueueSignalCapability = 134217728;
constexpr std::uint32_t MaxHwQueueSignals = 16;
struct HwQueueSignalDesc { std::uint32_t flags, count; std::uint64_t fence; };
static_assert(sizeof(HwQueueSignalDesc) == 16, "fixed hardware queue signal layout");
inline bool validHwQueueSignal(HwQueueSignalDesc d) {
    return (d.flags == 0 || d.flags == 4) && d.count && d.count <= MaxHwQueues && d.fence && d.fence != UINT64_MAX;
}
constexpr std::uint32_t AsyncHwSubmitCapability = 33554432;
constexpr std::uint32_t NoBroadcastSignalHwQueueCapability = 2097152;
constexpr std::uint32_t NoBroadcastSignalHwQueueFlag = 2;
constexpr std::uint32_t NoBroadcastWaitHwQueueCapability = 16777216;
constexpr std::uint32_t NoBroadcastWaitHwQueueFlag = 4;
constexpr std::uint32_t InternalNoBroadcastHwQueueFlags = NoBroadcastSignalHwQueueFlag | NoBroadcastWaitHwQueueFlag;
struct HwQueueDesc { std::uint32_t flags, privateBytes, reserved, reserved2; };
struct HwQueueReply { std::uint32_t sync, reserved; std::uint64_t offset, gpuAddress; };
static_assert(sizeof(HwQueueDesc) == 16 && sizeof(HwQueueReply) == 24, "fixed hardware queue layouts");
inline bool validHwQueue(HwQueueDesc d) {
    return (d.flags == 0 || d.flags == NoBroadcastSignalHwQueueFlag || d.flags == InternalNoBroadcastHwQueueFlags) &&
           d.privateBytes && d.privateBytes <= 4000 && !d.reserved && !d.reserved2;
}
struct VendorTranslationDesc { std::uint32_t device, adapter, reserved, reserved2; };
static_assert(sizeof(VendorTranslationDesc) == 16, "fixed allocation translation layout");
constexpr std::uint64_t VendorCpuSlotBytes = MaxAllocation;
constexpr std::uint32_t MaxVendorCpuMappingBytes = gpu_diagnostic::MaxMappingBytes;
constexpr std::uint64_t VendorCpuApertureBytes = MaxVendorCpuSlots * VendorCpuSlotBytes;
constexpr std::uint64_t CpuStoreFirstMarker = 0x4350554649525354ull, CpuStoreLastMarker = 0x4350554c41535421ull;
struct VendorCpuDesc { std::uint32_t device, flags; };
struct VendorCpuReply { std::uint32_t bytes, reserved; std::uint64_t generation; };
static_assert(sizeof(VendorCpuDesc) == 8 && sizeof(VendorCpuReply) == 16, "fixed CPU lock layouts");
inline bool validVendorCpuReply(std::uint64_t offset, VendorCpuReply r, bool sparse=false, bool desktop=false) {
    if(desktop && !sparse) return false;
    const auto limit=(desktop ? DesktopVendorCpuSlots : sparse ? SparseVendorCpuSlots : MaxVendorCpuSlots)*VendorCpuSlotBytes;
    return r.bytes && r.bytes <= MaxVendorCpuMappingBytes && !(r.bytes % 4096) && !r.reserved && r.generation &&
           !(offset % VendorCpuSlotBytes) && offset < limit && r.bytes <= limit - offset;
}
constexpr std::uint32_t MaxVendorResidencyAttempts = 64;
struct ResidentDesc { std::uint32_t count, flags, priorities, reserved; };
static_assert(sizeof(ResidentDesc) == 16, "fixed residency descriptor");
inline bool validResident(ResidentDesc d) {
    // CantTrimFurther is used by the live runtime. MustSucceed can force
    // device loss and is deliberately absent from this diagnostic.
    return d.count && d.count <= MaxVendorAllocations && d.flags <= 1 && d.priorities <= 1 && !d.reserved;
}
struct ResidentReply { std::uint32_t count, reserved; std::uint64_t bytesToTrim; };
static_assert(sizeof(ResidentReply) == 16, "fixed residency output");
struct MemoryBudgetDesc { std::uint32_t group, physicalAdapter; };
struct MemoryBudgetReply { std::uint64_t budget, usage, reservation, available; };
struct EvictDesc { std::uint32_t count, flags; };
inline bool validMemoryBudget(MemoryBudgetDesc d) { return d.group<=1 && !d.physicalAdapter; }
inline bool validEvict(EvictDesc d) { return d.count && d.count<=MaxVendorAllocations && d.flags<=3; }
static_assert(sizeof(MemoryBudgetDesc)==8 && sizeof(MemoryBudgetReply)==32 && sizeof(EvictDesc)==8,"fixed memory management ABI");
constexpr std::uint64_t MaxGpuAddress = 1ull << 48;
inline bool validSyncGpuAddress(SyncDesc desc, std::uint64_t address) {
    // NoGPUAccess fences have CPU storage but no GPU virtual address.
    return (desc.flags & NoGpuAccessSyncFlag) ? address == 0 : address && !(address % 8) && address < MaxGpuAddress;
}
constexpr std::uint32_t HwSubmitCapability = 32768;
constexpr std::uint32_t VendorRetirementCapability = 65536;
constexpr std::uint32_t GpuReservationCapability = 131072;
constexpr std::uint32_t GpuStateCapability = 262144;
constexpr std::size_t MaxGpuReservations = 8;
constexpr std::uint64_t MaxGpuReservationBytes = 4ull * 1024 * 1024 * 1024;
constexpr std::uint64_t MaxGpuReservedBytes = 4 * MaxGpuReservationBytes;
struct GpuReservationDesc { std::uint64_t base, minimum, maximum, bytes; };
struct FreeGpuReservationDesc { std::uint32_t adapter, reserved; };
static_assert(sizeof(GpuReservationDesc) == 32 && sizeof(FreeGpuReservationDesc) == 8, "fixed GPU reservation layouts");
inline bool validGpuReservation(GpuReservationDesc d) {
    constexpr auto alignment = 65536ull;
    if (!d.bytes || d.bytes % alignment || d.bytes > MaxGpuReservationBytes ||
        d.base % alignment || d.minimum % alignment || d.maximum % alignment ||
        d.base >= MaxGpuAddress || d.minimum >= MaxGpuAddress || d.maximum > MaxGpuAddress) return false;
    if (d.base) return d.base <= MaxGpuAddress - d.bytes;
    const auto end = d.maximum ? d.maximum : MaxGpuAddress;
    return d.minimum <= end && d.bytes <= end - d.minimum;
}
inline bool validGpuReservationOutput(GpuReservationDesc d, std::uint64_t address) {
    if (!validGpuReservation(d) || !address || address % 65536 || address > MaxGpuAddress - d.bytes) return false;
    return d.base ? address == d.base : address >= d.minimum && (!d.maximum || address + d.bytes <= d.maximum);
}
inline bool gpuRangesOverlap(std::uint64_t a, std::uint64_t bytes, std::uint64_t b, std::uint64_t otherBytes) {
    return a && bytes && b && otherBytes && a < b + otherBytes && b < a + bytes;
}
inline bool gpuRangeContains(std::uint64_t a, std::uint64_t bytes, std::uint64_t b, std::uint64_t otherBytes) {
    return b >= a && otherBytes <= bytes && b - a <= bytes - otherBytes;
}
constexpr std::uint32_t MaxHwSubmissions = 16;
struct HwSubmitDesc {
    std::uint64_t address, fence;
    std::uint32_t bytes, privateBytes, primaries, reserved;
};
static_assert(sizeof(HwSubmitDesc) == 32, "fixed command submission layout");
inline bool validHwSubmit(HwSubmitDesc d) {
    return d.address && !(d.address % 4096) && d.address < MaxGpuAddress &&
           d.bytes && !(d.bytes % 4096) && d.bytes <= MaxAllocation && d.address <= MaxGpuAddress - d.bytes &&
           d.fence && d.fence != UINT64_MAX && d.privateBytes <= 4000 && !d.primaries && !d.reserved;
}
// GPU-only mappings do not consume the separate 1 MiB CPU aperture slots.
constexpr std::uint32_t MaxVendorMapPages = gpu_diagnostic::MaxMappingBytes / 4096;
// A successful device occupies almost all of the old 16 MiB GPU map budget;
// queue startup then needs another 4 MiB allocation. The 128-slot graphics
// profile negotiates a separate 64 MiB GPU-map budget. Native reported usage
// keeps its independent 64 MiB guard.
constexpr std::uint32_t MaxVendorGpuMappedBytes = 32 * 1024 * 1024;
constexpr std::uint32_t MaxVendorMappedPages = MaxVendorGpuMappedBytes / 4096;
constexpr std::uint32_t ExpandedVendorGpuCapability = 8388608;
constexpr std::uint32_t ExpandedVendorGpuMappedBytes = gpu_diagnostic::ExpandedGpuBytes;
inline std::uint32_t vendorGpuPageLimit(std::uint32_t capabilities, bool desktop4k = false, bool largeMap = false) {
    if (largeMap && desktop4k && (capabilities & ExpandedVendorGpuCapability)) return OpenGlDesktopLargeMapBytes/4096;
    if (desktop4k && (capabilities & ExpandedVendorGpuCapability)) return OpenGlDesktop4kGpuMappedBytes / 4096;
    return ((capabilities & ExpandedVendorGpuCapability) ? ExpandedVendorGpuMappedBytes : MaxVendorGpuMappedBytes) / 4096;
}
inline bool vendorGpuBudgetFits(std::uint32_t usedPages, std::uint64_t pages, std::uint32_t capabilities, bool desktop4k = false, bool largeMap = false) {
    const auto limit = vendorGpuPageLimit(capabilities,desktop4k,largeMap);
    return usedPages <= limit && pages <= limit - usedPages;
}
struct GpuVaDesc {
    std::uint32_t queue, reserved;
    std::uint64_t base, minimum, maximum, offsetPages, sizePages, protection, driverProtection;
};
// Opaque NVIDIA mapping value observed for a nonshared D3D12 DEFAULT resource.
constexpr std::uint64_t VendorDefaultDriverProtection = 0x10000001;
static_assert(sizeof(GpuVaDesc) == 64, "fixed GPU-address descriptor");
inline bool validGpuVa(GpuVaDesc d) {
    if (!d.queue || d.reserved || !d.sizePages || d.sizePages > MaxVendorMapPages ||
        d.offsetPages > MaxVendorMapPages - d.sizePages || d.protection > 3 ||
        (d.driverProtection && (d.driverProtection != VendorDefaultDriverProtection || d.protection != 1)) ||
        d.base % 4096 || d.minimum % 4096 || d.maximum % 4096 ||
        d.base >= MaxGpuAddress || d.minimum >= MaxGpuAddress || d.maximum > MaxGpuAddress) return false;
    const auto bytes = d.sizePages * 4096;
    if (d.base) return d.base <= MaxGpuAddress - bytes;
    const auto end = d.maximum ? d.maximum : MaxGpuAddress;
    return d.minimum <= end && bytes <= end - d.minimum;
}
inline bool validGpuVaOutput(GpuVaDesc d, std::uint64_t address) {
    if (!validGpuVa(d) || !address || address % 4096 || address > MaxGpuAddress - d.sizePages * 4096) return false;
    if (d.base) return address == d.base;
    return address >= d.minimum && (!d.maximum || address + d.sizePages * 4096 <= d.maximum);
}
// Exact opaque NVIDIA value observed after an owned NT resource open. The
// ordinary validator above stays strict. Provenance is internal bookkeeping,
// never a flag supplied in GpuVaDesc. The forwarded descriptor is not rewritten.
constexpr std::uint64_t VendorImportedDriverProtection = 0x10000009;
inline bool validImportedGpuVa(GpuVaDesc d, bool openedSharedResource) {
    if (validGpuVa(d)) return true;
    if (!openedSharedResource || d.driverProtection != VendorImportedDriverProtection || d.protection != 1) return false;
    d.driverProtection = 0;
    return validGpuVa(d);
}
inline bool validImportedGpuVaOutput(GpuVaDesc d, std::uint64_t address, bool openedSharedResource) {
    if (!validImportedGpuVa(d, openedSharedResource)) return false;
    d.driverProtection = 0;
    return validGpuVaOutput(d, address);
}
inline bool validGpuState(GpuVaDesc d) {
    const auto state = d.protection & 12;
    return d.queue && !d.reserved && d.base && !d.offsetPages && !d.driverProtection &&
           !(d.protection & ~15ull) && (state == 4 || state == 8) &&
           d.sizePages && d.sizePages <= MaxGpuReservationBytes / 4096 &&
           !(d.base % 4096) && !(d.minimum % 4096) && !(d.maximum % 4096) &&
           d.base < MaxGpuAddress && d.minimum < MaxGpuAddress && d.maximum <= MaxGpuAddress &&
           d.base <= MaxGpuAddress - d.sizePages * 4096;
}
struct GpuVaReply { std::uint64_t fence; };
static_assert(sizeof(GpuVaReply) == 8, "fixed GPU-address fence reply");
struct VendorAllocationDesc {
    std::uint32_t flags, priority, source, privateBytes, reserved, reserved2;
};
static_assert(sizeof(VendorAllocationDesc) == 24, "fixed vendor allocation layout");
constexpr std::uint32_t UninitializedDisplaySource = UINT32_MAX;
inline bool validVendorAllocation(VendorAllocationDesc d) {
    // Standalone video memory only. No primary, stereo, resource sharing,
    // system-memory pointer or host CPU address crosses this interface.
    // An unset source is allowed for a non-primary allocation. It does not
    // select a display source and is normalized to NOTAPPLICABLE on Windows.
    return (d.source == 0 || d.source == UninitializedDisplaySource) &&
           !d.reserved && !d.reserved2 && d.privateBytes && d.privateBytes <= MaxVendorPrivateBytes &&
           ((d.flags == 0 && d.priority == 0) ||
            (d.flags == 4 && d.priority >= 0x28000000u && d.priority <= 0xc8000000u));
}
struct DestroyVendorDesc { std::uint32_t count, reserved; };
static_assert(sizeof(DestroyVendorDesc) == 8, "fixed vendor destruction layout");
struct VendorResourceReply { std::uint32_t resource, reserved; };
static_assert(sizeof(VendorResourceReply) == 8, "fixed resource ownership reply");
struct SharedVendorAllocationDesc {
    VendorAllocationDesc allocation;
    std::uint32_t runtimeBytes, reserved;
};
static_assert(sizeof(SharedVendorAllocationDesc) == 32, "fixed shared resource layout");
inline bool validSharedVendorAllocation(SharedVendorAllocationDesc d) {
    return validVendorAllocation(d.allocation) && d.allocation.flags == 4 &&
           d.runtimeBytes && d.runtimeBytes <= 1024 && !d.reserved &&
           d.allocation.privateBytes + d.runtimeBytes <= native_gpu::MaxPacket - sizeof(Header) - sizeof(d);
}
struct NativeTextureDesc { std::uint32_t width, height, format, flags; };
static_assert(sizeof(NativeTextureDesc) == 16, "fixed public texture profile");
inline bool validNativeTexture(NativeTextureDesc d) {
    // Initial diagnostic profile: TEX2D, RGBA8, one mip/array/sample,
    // ALLOW_RENDER_TARGET, DEFAULT/SHARED heap, UNKNOWN layout, no clear value.
    return d.width == gpu_diagnostic::Width && d.height == gpu_diagnostic::Height && d.format == 28 && d.flags == 1;
}
struct NativeSharedVendorAllocationDesc {
    SharedVendorAllocationDesc shared;
    NativeTextureDesc texture;
};
static_assert(sizeof(NativeSharedVendorAllocationDesc) == 48, "fixed typed shared resource layout");
inline bool validNativeSharedVendorAllocation(NativeSharedVendorAllocationDesc d) {
    return validSharedVendorAllocation(d.shared) && validNativeTexture(d.texture) &&
           d.shared.allocation.privateBytes + d.shared.runtimeBytes <= native_gpu::MaxPacket - sizeof(Header) - sizeof(d);
}
// Buffer metadata uses a separate, explicitly queried operation. Existing
// texture declarations and capability tuples retain their original contracts.
struct NativeGpuBufferDesc { std::uint64_t bytes; std::uint32_t flags, reserved; };
struct NativeGpuBufferCaps {
    std::uint32_t version, flags;
    std::uint64_t maxBytes;
    std::uint32_t resourceFlags, reserved;
};
constexpr NativeGpuBufferCaps NativeGpuBufferProfile{1, 1, 64ull * 1024 * 1024, 4, 0};
static_assert(sizeof(NativeGpuBufferDesc) == 16 && sizeof(NativeGpuBufferCaps) == 24, "Fixed native buffer ABI");
inline bool validNativeGpuBufferCaps(NativeGpuBufferCaps c) {
    return c.version == 1 && c.flags == 1 && c.maxBytes == NativeGpuBufferProfile.maxBytes && c.resourceFlags == 4 && !c.reserved;
}
inline bool validNativeGpuBuffer(NativeGpuBufferDesc d) {
    return d.bytes && d.bytes <= NativeGpuBufferProfile.maxBytes && (d.flags == 0 || d.flags == 4) && !d.reserved;
}
struct NativeSharedGpuBufferAllocationDesc { SharedVendorAllocationDesc shared; NativeGpuBufferDesc buffer; };
static_assert(sizeof(NativeSharedGpuBufferAllocationDesc) == 48, "Fixed typed shared buffer layout");
inline bool validNativeSharedGpuBufferAllocation(NativeSharedGpuBufferAllocationDesc d) {
    return validSharedVendorAllocation(d.shared) && validNativeGpuBuffer(d.buffer) &&
        d.shared.allocation.privateBytes + d.shared.runtimeBytes <= native_gpu::MaxPacket - sizeof(Header) - sizeof(d);
}
// Variable-size compositor buffers use a separate operation. The fixed RGBA
// texture/stream/Windows-consumer profiles above retain their old contract.
struct NativeInteropCaps {
    std::uint32_t version, formats, maxWidth, maxHeight;
    std::uint64_t maxLogicalBytes;
    std::uint32_t flags, reserved;
};
constexpr NativeInteropCaps NativeInteropProfile{1,7,4096,4096,64ull*1024*1024,1,0};
static_assert(sizeof(NativeInteropCaps)==32,"native opaque resource capabilities ABI");
inline bool validNativeInteropCaps(NativeInteropCaps c) {
    return c.version==1 && c.formats==7 && c.maxWidth==4096 && c.maxHeight==4096 &&
           c.maxLogicalBytes==64ull*1024*1024 && c.flags==1 && !c.reserved;
}
inline bool validNativeInteropTexture(NativeTextureDesc d) {
    return d.width && d.width<=NativeInteropProfile.maxWidth && d.height && d.height<=NativeInteropProfile.maxHeight &&
           (d.format==28 || d.format==87 || d.format==88) && d.flags==1 &&
           std::uint64_t(d.width)*d.height*4<=NativeInteropProfile.maxLogicalBytes;
}
inline bool validNativeInteropAllocation(NativeSharedVendorAllocationDesc d) {
    return validSharedVendorAllocation(d.shared) && validNativeInteropTexture(d.texture) &&
           d.shared.allocation.privateBytes+d.shared.runtimeBytes<=native_gpu::MaxPacket-sizeof(Header)-sizeof(d);
}
constexpr std::uint32_t DefaultFenceSlots = 64, ExpandedFenceSlots = 128, MaxFenceSlots = 256;
constexpr std::uint32_t DesktopFenceSlots=512;
inline bool validFenceSlots(std::uint32_t slots,bool desktop=false) { return slots == DefaultFenceSlots || slots == ExpandedFenceSlots || slots == MaxFenceSlots || (desktop && slots==DesktopFenceSlots); }
constexpr std::uint64_t FenceApertureBytes = MaxFenceSlots * 4096;
constexpr std::uint64_t DesktopFenceApertureBytes=DesktopFenceSlots*4096;
constexpr std::uint32_t MaxQueryBytes = 65536;
constexpr std::size_t MaxActiveQueries = 2;
struct QueryDesc { std::uint32_t type, bytes, reserved, reserved2; };
static_assert(sizeof(QueryDesc) == 16, "fixed query layout");
inline bool validQuery(QueryDesc d) {
    if (d.reserved || d.reserved2 || !d.bytes || d.bytes > MaxQueryBytes) return false;
    // Inline WDDM x64/UTF-16 layouts observed during Linux D3D12 startup.
    // Pointer-bearing PnP queries, registry queries and SetWorkingSet are
    // deliberately absent. Opaque UMD data requires an explicit host opt-in.
    switch (d.type) {
        case 0: return true;
        case 1: return d.bytes == 524;
        case 3: return d.bytes == 24;
        case 13: case 15: case 24: case 27: case 30: case 55: case 56: return d.bytes == 4;
        case 17: case 34: return d.bytes == 12;
        case 18: return d.bytes == 8;
        case 31: return d.bytes == 28;
        case 60: return d.bytes == 80;
        case 61: return d.bytes == 56;
        case 62: return d.bytes == 64;
        case 66: return d.bytes == 8192;
        default: return false;
    }
}
enum class Op : std::uint32_t {
    Hello = 0x2000, OpenAdapter, QueryDriverVersion, CloseAdapter,
    CreateDevice, DestroyDevice, CreatePagingQueue, ReadPagingFence, DestroyPagingQueue,
    CreateAllocation = 0x2010, WriteAllocation, ReadAllocation, MakeResident,
    MapAllocation, QueryResidency, DestroyAllocation, CreateSharedAllocation, CopySharedAllocation,
    CreateContext = 0x2020, DestroyContext, SetContextInProcessPriority,
    BeginAdapterQuery = 0x2030, WriteAdapterQuery, RunAdapterQuery, ReadAdapterQuery, EndAdapterQuery,
    CreateGuestPagingQueue = 0x2040,
    CreateVendorAllocation = 0x2050, DestroyVendorAllocations, MapVendorAllocation, MakeVendorResident,
    LockVendorAllocation, UnlockVendorAllocation, TranslateVendorAllocation,
    CreateVendorResourceAllocation, DestroyVendorResource, CreateSharedVendorResourceAllocation, ConsumeSharedTexture,
    CreateNativeSharedVendorResourceAllocation,
    BeginStreaming, ConsumeStreamingTexture,
    BeginOpenGl,
    CreateHwQueue = 0x2060, DestroyHwQueue, SubmitHwQueue, QueueHwCommand,
    CreateSync = 0x2070, DestroySync, SignalContextSync, QueueHwQueueSignal,
    BeginSyncSharing, ExportSync, ImportSync, CloseSharedSync, QuerySharedSyncInfo, QueueHwQueueWait, QueueContextSync, QueueContextWait,
    ReserveGpuAddress = 0x2080, FreeGpuReservation, MapGpuState,
    BeginResourceSharing = 0x2090, ExportResource, CloseSharedResource, QuerySharedResourceInfo, ImportResource,
    QueryMemoryBudget = 0x20a0, EvictVendorAllocations,
    CreateNativeInteropResource, QueryNativeInteropCaps, CaptureCompositorResource, QueueCompositorFrame,
    QueryDeviceExecutionState = 0x20b0,
    QueryNativeGpuBufferCaps = 0x20c0, CreateNativeSharedGpuBufferAllocation,
    QueryNativeGpuBufferAdmission = 0x20d0
};
enum class Kind { Adapter, Device, PagingQueue, Allocation, Context, PagingSync, VendorAllocation, HwQueue, HwQueueSync, Sync, GpuReservation, VendorResource, SharedSync, SharedResource };
// Closing an exported NT reference does not destroy a KMT allocation/sync or
// its independently opened recipient. Queue paths check their own retirement.
// Every backing object still requires the global pending-submission guard.
inline bool nativeDestroyWaitsForSubmissions(Kind kind) {
    return kind!=Kind::HwQueue && kind!=Kind::HwQueueSync && kind!=Kind::SharedResource && kind!=Kind::SharedSync;
}
struct ContextDesc {
    std::uint32_t node, engine, flags, clientHint, privateBytes, reserved;
};
static_assert(sizeof(ContextDesc) == 24, "fixed context layout");
inline bool validContext(ContextDesc d) {
    if (d.reserved || d.privateBytes > MaxContextPrivateBytes) return false;
    // Synchronization-only contexts do not require vendor initialization data.
    if (d.flags == 8) return d.node == 0 && d.engine == 0 && d.clientHint == 0 && !d.privateBytes;
    // D3D12 virtual contexts, including Mesa's documented GLON12 client hint.
    // The client hint identifies the frontend; it grants no extra operations.
    // Never forward DisableGpuTimeout, TestContext or unknown flag bits.
    return d.node < 64 && d.engine == 1 && (d.flags == 0 || d.flags == 16) &&
           (d.clientHint == 12 || d.clientHint == 16) && d.privateBytes;
}
inline bool validContextPriority(std::int32_t priority) { return priority == 0 || priority == 1; }
inline bool contextCanSignal(std::uint32_t flags) { return flags==8 || flags==16; }
inline bool contextCanWait(std::uint32_t flags,bool coordinated) { return flags==16 || (coordinated && flags==8); }
struct Range { std::uint32_t offset; std::uint32_t size; };
static_assert(sizeof(Range) == 8, "fixed transfer layout");
struct GuestRange { std::uint64_t offset; std::uint32_t size; std::uint32_t reserved; };
static_assert(sizeof(GuestRange) == 16, "fixed guest range layout");
struct CopyRange { std::uint32_t source; std::uint32_t sourceOffset; std::uint32_t destinationOffset; std::uint32_t size; };
static_assert(sizeof(CopyRange) == 16, "fixed GPU copy layout");
struct Result { std::int32_t ntstatus; std::uint32_t nativeHandle; std::uint64_t value; };
struct MemoryBudgetResult { Result result; MemoryBudgetReply info; };
struct EvictResult { Result result; bool attempted; };
inline bool validHwSubmitReply(Result result, std::uint64_t target, std::uint32_t capabilities) {
    return target && target != UINT64_MAX && !result.nativeHandle && result.ntstatus <= 0 &&
           (result.ntstatus < 0 ? result.value == 0 : result.value != UINT64_MAX &&
            ((capabilities & AsyncHwSubmitCapability) || result.value >= target));
}
// Host-local diagnostics, never serialized as a guest capability or wire reply.
// Queue acceptance is distinct from a completed GPU fence.
struct SubmissionActivity {
    std::uint64_t acceptedCommands=0,asynchronousCommands=0,acceptedCommandBytes=0;
    bool exact=true;
    void record(bool asynchronous,std::uint32_t bytes) noexcept {
        if(!exact) return;
        if(acceptedCommands==UINT64_MAX || bytes>UINT64_MAX-acceptedCommandBytes) { exact=false;return; }
        ++acceptedCommands;
        if(asynchronous) ++asynchronousCommands;
        acceptedCommandBytes+=bytes;
    }
};
struct GpuVaResult { Result map; std::uint64_t fence; };
struct ResidentResult { Result residency; ResidentReply output; };
struct VendorCpuResult { Result lock; VendorCpuReply output; };
struct VendorResourceResult { Result allocation; std::uint32_t resource; };
struct SharedResourceOpenResult { Result allocation; SharedResourceOpenReply info; };
struct SharedReferenceAlias { std::int32_t ntstatus; std::uint32_t ticket; };
// The synchronization object is borrowed from the paging queue. Destruction
// of the queue owns its lifetime; no native handle is exposed to the guest.
struct GuestPagingResult { Result queue; std::uint32_t sync; std::uint64_t offset; };
struct HwQueueResult { Result queue; std::uint32_t sync; std::uint64_t offset, gpuAddress; };
struct SyncResult { Result object; SyncReply output; };
struct PagingReply { std::uint32_t sync, reserved; std::uint64_t offset; };
static_assert(sizeof(PagingReply) == 16, "fixed guest paging reply");
// Native KMT object fields use local IDs, never raw handles/pointers. Opt-in
// context private data is opaque and requires a compatible PV-aware UMD.
// NTSTATUS is preserved in
// this fixed payload; Header.status is a negative errno for protocol errors.
struct Reply { std::int32_t ntstatus; std::uint32_t reserved; std::uint64_t value; };
static_assert(sizeof(Reply) == 16, "fixed reply layout");
struct CoordinatedObjectCapacity {
    std::uint32_t version=3,hwQueues=32,syncObjects=384,leaseLimit=768;
    std::uint64_t reservedGpuBytes=64ull*1024*1024*1024;
    // v3 reserves the final word as zero. v4 explicitly carries the aggregate
    // native allocation-object quota; CPU/GPU byte budgets remain independent.
    std::uint32_t reservations=256,vendorAllocations=0;
};
// v5 fixes aggregate native NT-reference limits independently of byte budgets.
// Per-actor resource/sync profiles still advertise/enforce sixteen references.
constexpr std::size_t MaxCoordinatedSharedReferences=64;
static_assert(sizeof(CoordinatedObjectCapacity)==32,"Fixed coordinated object/address-space capacity");
inline bool validCoordinatedObjectCapacity(CoordinatedObjectCapacity value) {
    return ((value.version==3 && !value.vendorAllocations) || ((value.version>=4 && value.version<=9) && value.vendorAllocations==1024) ||
            (value.version==10 && value.vendorAllocations==2048)) &&
        value.hwQueues==(value.version>=8 ? 64u : 32u) && value.syncObjects==(value.version>=8 ? 768u : 384u) &&
        value.leaseLimit==(value.version>=7 ? 1536u : 768u) &&
        value.reservedGpuBytes==64ull*1024*1024*1024 && value.reservations==256;
}
constexpr CoordinatedObjectCapacity GuestCoordinatedDesktopObjects{4,32,384,768,64ull*1024*1024*1024,256,1024};
constexpr CoordinatedObjectCapacity GuestCoordinatedReferenceObjects{5,32,384,768,64ull*1024*1024*1024,256,1024};
// v6 explicitly separates owned reservation mappings from the standalone
// mapping quota. It requires a retaining native residency backend.
constexpr CoordinatedObjectCapacity GuestCoordinatedManagedObjects{6,32,384,768,64ull*1024*1024*1024,256,1024};
// v7 explicitly negotiates metadata for all slots of the existing desktop
// apertures. Physical memory, object quotas and older agreements stay fixed.
constexpr CoordinatedObjectCapacity GuestCoordinatedLeaseObjects{7,32,384,1536,64ull*1024*1024*1024,256,1024};
// v8 supplies object records for all four supported actors at their existing
// per-actor bounds. Fence aperture slots and physical byte guards are separate.
constexpr CoordinatedObjectCapacity GuestCoordinatedActorObjects{8,4*OpenGlFenceHwQueues,4*OpenGlCompositorSyncObjects,1536,64ull*1024*1024*1024,256,1024};
constexpr CoordinatedObjectCapacity GuestCoordinatedMultiappObjects{9,64,768,1536,64ull*1024*1024*1024,256,1024};
// v10 expands only aggregate allocation metadata. Per-actor and physical byte
// limits stay fixed; both the owner and controller must explicitly select it.
constexpr CoordinatedObjectCapacity GuestCoordinatedApplicationObjects{10,64,768,1536,64ull*1024*1024*1024,256,2048};
inline bool validCoordinatedAgreement(OpenGlDesktopPresentationCapacity capacity,CoordinatedObjectCapacity objects,bool applicationMemory=false) {
    return (applicationMemory ? validApplicationMemoryCapacity(capacity) : validCoordinatedPresentationCapacity(capacity)) && validCoordinatedObjectCapacity(objects) &&
        (capacity.mapping.aperture.version==14 ? (objects.version==9 || (applicationMemory && objects.version==10)) : capacity.mapping.aperture.version==13 ? (objects.version>=5 && objects.version<=8) :
            objects.version==(capacity.mapping.aperture.version>=11 ? 4u : 3u));
}
enum class PresentationStopProgress { Pending, Complete, Failed };
class Driver {
public:
    virtual bool supportsLiveRetirement() const { return false; }
    virtual bool prepareLiveRetirement() { return false; }
    virtual bool stopCoordinatedPresentation() { return false; }
    virtual PresentationStopProgress pollCoordinatedPresentationStop() { return PresentationStopProgress::Failed; }
    virtual bool beginCoordinatedObjects(CoordinatedObjectCapacity) { return false; }
    virtual bool beginApplicationMemory(ApplicationMemoryProfile) { return false; }
    virtual ~Driver() = default;
    virtual native_gpu::Capabilities capabilities() const = 0;
    virtual StreamProfile beginStreaming() { return {}; }
    virtual OpenGlProfile beginOpenGl(std::uint32_t = 1) { return {}; }
    virtual Result queueCompositorFrame(std::uint32_t, std::uint32_t, CompositorCaptureDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu),0,0};
    }
    virtual NativeInteropCaps nativeInteropCaps() const { return {}; }
    virtual NativeGpuBufferCaps nativeGpuBufferCaps() const { return {}; }
    virtual Result queryNativeGpuBufferAdmission(NativeGpuBufferDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual VendorResourceResult createNativeSharedGpuBufferAllocation(std::uint32_t, NativeSharedGpuBufferAllocationDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual Result captureCompositorResource(std::uint32_t, std::uint32_t, CompositorCaptureDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result consumeStreamingTexture(std::uint32_t, std::uint32_t, SharedTextureConsumeDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result openAdapter() = 0;
    // Trusted native identity check; wire IDs alone cannot establish equivalence.
    virtual bool samePhysicalAdapter(std::uint32_t, std::uint32_t) const { return false; }
    virtual Result queryVersion(std::uint32_t adapter) = 0;
    virtual Result queryAdapter(std::uint32_t adapter, QueryDesc desc, std::vector<std::uint8_t>& data) = 0;
    virtual Result createDevice(std::uint32_t adapter) = 0;
    virtual Result queryDeviceExecutionState(std::uint32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu),0,0};
    }
    virtual Result createContext(std::uint32_t device, ContextDesc desc, std::vector<std::uint8_t>& data) = 0;
    virtual Result setContextInProcessPriority(std::uint32_t, std::int32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result createPagingQueue(std::uint32_t device) = 0;
    virtual Result createVendorAllocation(std::uint32_t, VendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual VendorResourceResult createVendorResourceAllocation(std::uint32_t, VendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual VendorResourceResult createSharedVendorResourceAllocation(std::uint32_t, SharedVendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual VendorResourceResult createNativeSharedVendorResourceAllocation(std::uint32_t, NativeSharedVendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual VendorResourceResult createNativeInteropResource(std::uint32_t, NativeSharedVendorAllocationDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu),0,0},0};
    }
    virtual Result consumeSharedTexture(std::uint32_t, std::uint32_t, SharedTextureConsumeDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result destroyVendorAllocations(std::uint32_t, const std::vector<std::uint32_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual GpuVaResult mapVendorAllocation(std::uint32_t, std::uint32_t, GpuVaDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual ResidentResult makeVendorResident(std::uint32_t, ResidentDesc, const std::vector<std::uint32_t>&,
                                            const std::vector<std::uint32_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0, 0}};
    }
    virtual GuestPagingResult createGuestPagingQueue(std::uint32_t) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0, 0};
    }
    virtual VendorCpuResult lockVendorAllocation(std::uint32_t, std::uint32_t) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0, 0}};
    }
    virtual Result unlockVendorAllocation(std::uint32_t, std::uint32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result translateVendorAllocation(std::uint32_t, std::uint32_t, std::uint32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual HwQueueResult createHwQueue(std::uint32_t, HwQueueDesc, std::vector<std::uint8_t>&) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0, 0, 0};
    }
    virtual SyncResult createSync(std::uint32_t, SyncDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0}};
    }
    virtual SyncSharingProfile beginSyncSharing() { return {}; }
    virtual Result exportSync(std::uint32_t) { return {static_cast<std::int32_t>(0xc00000bbu), 0, 0}; }
    virtual SyncResult importSync(std::uint32_t, SharedSyncOpenDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, {0, 0}};
    }
    virtual ResourceSharingProfile beginResourceSharing() { return {}; }
    virtual MemoryBudgetResult queryMemoryBudget(std::uint32_t, MemoryBudgetDesc) { return {{-95,0,0},{}}; }
    // Only a backend that retains a real native residency reference may accept
    // logical eviction while a VM CPU lease remains active. Legacy backends
    // keep the last-reference rejection below.
    virtual bool retainsMappedResidencyOnEvict() const { return false; }
    virtual bool allowsReservationBackedMappings() const { return false; }
    virtual EvictResult evictVendorAllocations(std::uint32_t, EvictDesc, const std::vector<std::uint32_t>&) { return {{-95,0,0},false}; }
    virtual Result querySharedResourceInfo(std::uint32_t, std::uint32_t, SharedResourceInfo&, std::vector<std::uint8_t>&) { return {-95, 0, 0}; }
    virtual SharedResourceOpenResult importResource(std::uint32_t, SharedResourceOpenDesc, std::vector<std::uint8_t>&) { return {{-95, 0, 0}, {}}; }
    virtual Result exportResource(std::uint32_t) { return {static_cast<std::int32_t>(0xc00000bbu), 0, 0}; }
    virtual Result duplicateSharedReference(Kind,std::uint32_t) { return {static_cast<std::int32_t>(0xc00000bbu),0,0}; }
    virtual Result querySharedSyncInfo(std::uint32_t, std::uint32_t) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result queueHwQueueWait(std::uint32_t, const std::vector<HwQueueWaitItem>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result queueContextWait(std::uint32_t, const std::vector<HwQueueWaitItem>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result signalContextSync(std::uint32_t, std::uint32_t, ContextSignalDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result queueContextSync(std::uint32_t, std::uint32_t, ContextSignalDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result submitHwQueue(std::uint32_t, HwSubmitDesc, const std::vector<std::uint8_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result queueHwCommand(std::uint32_t, HwSubmitDesc, const std::vector<std::uint8_t>&) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result queueHwQueueSignal(std::uint32_t, const std::vector<std::uint32_t>&, HwQueueSignalDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual Result reserveGpuAddress(std::uint32_t, GpuReservationDesc) {
        return {static_cast<std::int32_t>(0xc00000bbu), 0, 0};
    }
    virtual GpuVaResult mapGpuState(std::uint32_t, std::uint32_t, GpuVaDesc) {
        return {{static_cast<std::int32_t>(0xc00000bbu), 0, 0}, 0};
    }
    virtual Result readPagingFence(std::uint32_t queue) = 0;
    virtual Result createAllocation(std::uint32_t device, std::uint32_t size) = 0;
    virtual Result createSharedAllocation(std::uint32_t device, GuestRange range) = 0;
    virtual Result copySharedAllocation(std::uint32_t destination, std::uint32_t source, CopyRange range) = 0;
    virtual Result writeAllocation(std::uint32_t allocation, Range range, const std::uint8_t* data) = 0;
    virtual Result readAllocation(std::uint32_t allocation, Range range, std::vector<std::uint8_t>& data) = 0;
    virtual Result makeResident(std::uint32_t allocation, std::uint32_t queue) = 0;
    virtual Result mapAllocation(std::uint32_t allocation, std::uint32_t queue) = 0;
    virtual Result queryResidency(std::uint32_t allocation) = 0;
    virtual Result destroy(Kind kind, std::uint32_t handle) = 0;
};
// Immutable, owner-created agreement shared by all actor Sessions. It seeds
// native state once; client negotiation only validates/reads the cached data.
class CoordinatedProfiles {
    friend class Session;
    Driver* owner;
    native_gpu::Capabilities caps{};
    OpenGlProfile gl{};
    SyncSharingProfile sync{};
    ResourceSharingProfile resource{};
    bool managedMappings=false,applicationMemoryEnabled=false;
    OpenGlDesktopPresentationCapacity presentation{};
public:
    CoordinatedProfiles(Driver& driver,OpenGlDesktopPresentationCapacity capacity,CoordinatedObjectCapacity objectCapacity={},
                        std::uint32_t resourceReferences=MaxSharedResourceHandles,bool applicationMemory=false) : owner(&driver) {
        constexpr auto required=AsyncHwSubmitCapability|SyncCapability|GpuReservationCapability|VendorMemoryManagementCapability|
            ExpandedVendorGpuCapability|ExpandedVendorCpuCapability|VendorAllocationCapability|SharedVendorResourceCapability|NativeSharedTextureCapability;
        caps=driver.capabilities();
        if((resourceReferences!=MaxSharedResourceHandles && resourceReferences!=ApplicationResourceReferences) ||
            (resourceReferences==ApplicationResourceReferences && (capacity.mapping.aperture.version!=14 || (objectCapacity.version!=9 && objectCapacity.version!=10))) ||
            (applicationMemory && resourceReferences!=ApplicationResourceReferences) ||
            !validCoordinatedAgreement(capacity,objectCapacity,applicationMemory) || caps.version!=Version || caps.vendor!=0x10de ||
            (caps.flags&required)!=required) throw std::runtime_error("Invalid coordinated native capacity/capabilities");
        if(applicationMemory && !driver.beginApplicationMemory(GuestApplicationMemoryProfile))
            throw std::runtime_error("Native application memory capacity unavailable");
        applicationMemoryEnabled=applicationMemory;presentation=capacity;
        gl=driver.beginOpenGl(capacity.mapping.aperture.version);
        if(gl.version!=capacity.mapping.aperture.version || !validCoordinatedPresentationProfile(gl)) throw std::runtime_error("Coordinated native OpenGL agreement failed");
        if(!driver.beginCoordinatedObjects(objectCapacity)) throw std::runtime_error("Coordinated native object agreement failed");
        managedMappings=objectCapacity.version>=6;
        if(managedMappings && (!driver.allowsReservationBackedMappings() || !driver.retainsMappedResidencyOnEvict()))
            throw std::runtime_error("Managed GPU mapping backend absent");
        sync=driver.beginSyncSharing();
        if(!validSyncSharingProfile(sync)) throw std::runtime_error("Coordinated native fence agreement failed");
        resource=driver.beginResourceSharing();
        if(!validResourceSharingProfile(resource)) throw std::runtime_error("Coordinated native resource agreement failed");
        // The v9 native agreement already enforces 64 references for the whole
        // group. This changes only how that fixed pool may be distributed to
        // a recipient, after the trusted controller and owner both opt in.
        if(resourceReferences==ApplicationResourceReferences) resource=GuestApplicationResourceSharingProfile;
    }
    CoordinatedProfiles(const CoordinatedProfiles&)=delete;
    CoordinatedProfiles& operator=(const CoordinatedProfiles&)=delete;
    CoordinatedProfiles(CoordinatedProfiles&&)=delete;
    CoordinatedProfiles& operator=(CoordinatedProfiles&&)=delete;
    native_gpu::Capabilities capabilities() const { return caps; }
    OpenGlProfile openGl() const { return gl; }
    SyncSharingProfile syncSharing() const { return sync; }
    ResourceSharingProfile resourceSharing() const { return resource; }
    bool applicationMemory() const { return applicationMemoryEnabled; }
};
class Session {
    struct Object {
        Kind kind; std::uint32_t parent; std::uint32_t nativeHandle; std::uint32_t size;
        std::uint64_t guestOffset; bool shared;
        std::uint32_t gpuPages = 0;
        bool reservationBackedMapping = false;
        std::uint64_t gpuAddress = 0;
        std::uint32_t residencyAttempts = 0;
        std::uint64_t gpuOffsetPages = 0;
        std::uint32_t cpuBytes = 0;
        std::uint64_t cpuOffset = 0;
        std::uint32_t driverToken = 0;
        std::uint32_t contextFlags = 0;
        std::uint32_t syncType = 0;
        std::uint64_t reservedGpuBytes = 0;
        bool resident = false;
        std::uint64_t submittedFence = 0;
        std::uint32_t vendorResource = 0;
        bool openedSharedResource = false;
        std::uint32_t syncFlags = 0;
        // Proven lower bound. Failed/partial native operations can leave
        // extra residency references which must not authorize unsafe access.
        std::uint32_t residentReferences = 0;
    };
    Driver& driver;
    std::uint64_t fenceApertureBytes=FenceApertureBytes;
    const CoordinatedProfiles* coordinated=nullptr;
    bool mayPresent=true;
    std::map<std::uint32_t, Object> objects;
    struct Query {
        QueryDesc desc; std::vector<std::uint8_t> data;
        std::uint32_t written = 0; bool executed = false; Result result{};
    };
    std::map<std::uint32_t, Query> queries;
    std::uint32_t nextId = 1;
    unsigned liveRetirementPhase=0;
    std::uint32_t allocatedBytes = 0;
    std::uint32_t vendorMappedPages = 0;
    std::uint32_t vendorCpuBytes = 0;
    std::uint64_t submissionAttempts = 0;
    SubmissionActivity submissionActivity;
    std::uint64_t contextSignalAttempts = 0;
    std::uint64_t hwQueueSignalAttempts = 0;
    std::uint64_t gpuReservedBytes = 0;
    GpuStateRanges gpuStates;
    bool negotiated = false;
    bool streamingNegotiated = false;
    bool openglNegotiated = false;
    bool openglFenceNegotiated = false;
    bool openglCompositorNegotiated = false;
    bool openglRenderNegotiated = false;
    bool openglDesktopNegotiated = false;
    bool openglDesktop4kNegotiated = false;
    bool openglDesktop4kCpuNegotiated = false;
    bool openglDesktopSparseNegotiated = false;
    bool openglDesktopSlotsNegotiated = false;
    bool openglDesktopLargeMapNegotiated = false;
    unsigned compositorCaptureAttempts = 0;
    bool compositorQueueUsed = false;
    bool compositorQueueEnded = false;
    std::uint64_t compositorQueuedFrames = 0;
    unsigned compositorQueueAudits = 0;
    std::size_t wireObjectLimit = MaxObjects;
    bool syncSharingNegotiated = false;
    bool resourceSharingNegotiated = false;
    static std::vector<std::uint8_t> reply(Header h, std::int32_t error,
                                         std::uint32_t id = 0, const Result* result = nullptr) {
        h.handle = id; h.status = error; h.padding = 0;
        std::vector<std::uint8_t> out(sizeof h + (result ? sizeof(Reply) : 0));
        std::memcpy(out.data(), &h, sizeof h);
        if (result) {
            const Reply body{result->ntstatus, 0, result->value};
            std::memcpy(out.data() + sizeof h, &body, sizeof body);
        }
        return out;
    }
    std::vector<std::uint8_t> insert(Header h, Kind kind, std::uint32_t parent, Result result,
                                   std::uint32_t size = 0, std::uint64_t guestOffset = 0, bool shared = false, std::uint32_t contextFlags = 0) {
        if (result.ntstatus < 0) return reply(h, 0, 0, &result);
        if (!result.nativeHandle) return reply(h, -5);
        const auto id = nextId++;
        Object object{kind, parent, result.nativeHandle, size, guestOffset, shared}; object.contextFlags = contextFlags;
        objects.emplace(id, object);
        allocatedBytes += size;
        return reply(h, 0, id, &result);
    }
public:
    explicit Session(Driver& d) : driver(d) {}
    SubmissionActivity acceptedSubmissionActivity() const noexcept { return submissionActivity; }
    Session(Driver& d,const CoordinatedProfiles& group,bool presentationOwner=false) : driver(d),coordinated(&group),mayPresent(presentationOwner) {
        if(group.owner!=&d) throw std::runtime_error("Coordinated profile belongs to another native Driver");
    }
    // Trusted coordinator API, deliberately absent from guest dispatch. The
    // caller must resolve the actual capability FD before choosing the donor.
    // A recipient owns a fresh NT reference, never the donor's closing handle.
    SharedReferenceAlias aliasSharedReference(const Session& donor,Kind kind,std::uint32_t ticket,std::uint32_t adapter) {
        constexpr auto invalid=static_cast<std::int32_t>(0xc000000du);
        constexpr auto exhausted=static_cast<std::int32_t>(0xc0000017u);
        if(this==&donor || &driver!=&donor.driver || !negotiated || !donor.negotiated ||
            (kind!=Kind::SharedResource && kind!=Kind::SharedSync) ||
            (kind==Kind::SharedResource ? !resourceSharingNegotiated || !donor.resourceSharingNegotiated :
                                        !syncSharingNegotiated || !donor.syncSharingNegotiated)) return {invalid,0};
        const auto source=donor.objects.find(ticket);
        const auto target=objects.find(adapter);
        if(source==donor.objects.end() || source->second.kind!=kind || target==objects.end() || target->second.kind!=Kind::Adapter)
            return {invalid,0};
        const auto parent=donor.objects.find(source->second.parent);
        if(parent==donor.objects.end() || parent->second.kind!=Kind::Adapter ||
            !driver.samePhysicalAdapter(parent->second.nativeHandle,target->second.nativeHandle)) return {invalid,0};
        const auto count=std::count_if(objects.begin(),objects.end(),[kind](const auto& value){return value.second.kind==kind;});
        const auto referenceLimit=kind==Kind::SharedResource ? resourceReferenceLimit() : MaxSharedSyncHandles;
        if(objects.size()>=objectLimit() || nextId==UINT32_MAX || static_cast<std::size_t>(count)>=referenceLimit) {
            std::cerr<<"{\"nativeSharedAliasAdmissionRejected\":true,\"stage\":\"wire\",\"kind\":\""<<(kind==Kind::SharedResource ? "resource" : "sync")
                <<"\",\"liveWireObjects\":"<<objects.size()<<",\"wireObjectLimit\":"<<objectLimit()<<",\"liveReferences\":"<<count
                <<",\"referenceLimit\":"<<referenceLimit<<",\"idExhausted\":"<<(nextId==UINT32_MAX ? "true" : "false")<<"}\n";
            return {exhausted,0};
        }
        const auto native=driver.duplicateSharedReference(kind,source->second.nativeHandle);
        const bool known=native.nativeHandle==source->second.nativeHandle || std::any_of(objects.begin(),objects.end(),
            [&](const auto& value){return value.second.kind==kind && value.second.nativeHandle==native.nativeHandle;});
        if(native.ntstatus>0 || native.value || (native.ntstatus<0 ? native.nativeHandle!=0 : !native.nativeHandle) ||
            (!native.ntstatus && known)) {
            if(native.nativeHandle && !known) driver.destroy(kind,native.nativeHandle);
            return {static_cast<std::int32_t>(0xc0000001u),0};
        }
        if(native.ntstatus) return {native.ntstatus,0};
        const auto id=nextId++;
        try {
            Object alias{kind,adapter,native.nativeHandle,0,0,true};alias.syncFlags=source->second.syncFlags;
            if(!objects.emplace(id,alias).second) throw std::runtime_error("Shared alias identity collision");
        } catch(...) { driver.destroy(kind,native.nativeHandle);throw; }
        return {0,id};
    }
    bool resourceAdaptersCompatible(std::uint32_t first, std::uint32_t second) const {
        if (first==second) return true;
        if (!openglRenderNegotiated) return false;
        const auto a=objects.find(first), b=objects.find(second);
        return a!=objects.end() && b!=objects.end() && a->second.kind==Kind::Adapter && b->second.kind==Kind::Adapter &&
               driver.samePhysicalAdapter(a->second.nativeHandle,b->second.nativeHandle);
    }
    std::size_t objectCount() const { return objects.size(); }
    std::size_t objectLimit() const { return wireObjectLimit; }
    std::size_t resourceReferenceLimit() const { return coordinated ? coordinated->resource.handleLimit : MaxSharedResourceHandles; }
    std::size_t hwQueueLimit() const { return openglFenceNegotiated ? OpenGlFenceHwQueues : MaxHwQueues; }
    std::size_t reservationLimit() const { return coordinated && openglDesktopNegotiated ? coordinated->gl.reservationLimit : openglDesktopNegotiated ? GuestOpenGlDesktopProfile.reservationLimit : openglRenderNegotiated ? GuestOpenGlCompositorRenderProfile.reservationLimit : openglNegotiated ? GuestOpenGlProfile.reservationLimit : MaxGpuReservations; }
    std::size_t syncObjectLimit() const { return openglCompositorNegotiated ? OpenGlCompositorSyncObjects : MaxSyncObjects; }
    std::uint32_t openGlCommandLimit() const { return openglDesktopNegotiated ? 0 : openglCompositorNegotiated ? GuestOpenGlCompositorProfile.commandLimit : GuestOpenGlProfile.commandLimit; }
    std::uint32_t hwQueueSignalLimit() const { return openglDesktopNegotiated ? 0 : openglCompositorNegotiated ? OpenGlCompositorHwSignals : openglFenceNegotiated ? OpenGlFenceHwSignals : MaxHwQueueSignals; }
    std::size_t vendorObjectLimit() const { return openglDesktopNegotiated ? OpenGlDesktopVendorAllocations : openglCompositorNegotiated ? OpenGlCompositorVendorAllocations : openglFenceNegotiated ? OpenGlFenceVendorAllocations : vendorAllocationObjectLimit(driver.capabilities().flags); }
    unsigned requestLimit() const { return openglDesktopNegotiated ? 0 : streamingNegotiated ? GuestStreamProfile.requestLimit : openglCompositorNegotiated ? GuestOpenGlCompositorProfile.requestLimit : 10000; }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Only the privileged coordinator may call this after pidfd-confirmed
    // process exit, CPU-view revocation and shared capability drainage. Keep
    // failed children owned; do not advance to parent/resource phases early.
    LiveRetirementProgress retireExitedActor() {
        if(!coordinated || mayPresent || (coordinated->gl.version!=13 && coordinated->gl.version!=14))
            throw std::runtime_error("Live retirement requires a coordinated application");
        return retireExitedObjects();
    }
    // Only the terminal group control may retire the presentation owner, after
    // every actor exited and the native presentation queues were drained.
    LiveRetirementProgress retireStoppedPrimary() {
        if(!coordinated || !mayPresent || (coordinated->gl.version!=13 && coordinated->gl.version!=14))
            throw std::runtime_error("Terminal retirement requires the coordinated primary");
        return retireExitedObjects();
    }
    bool hasSharedReferences() const {
        return std::any_of(objects.begin(),objects.end(),[](const auto& item) {
            return item.second.kind==Kind::SharedResource || item.second.kind==Kind::SharedSync;
        });
    }
private:
    LiveRetirementProgress retireExitedObjects() {
        LiveRetirementProgress progress;
        progress.remaining=static_cast<std::uint32_t>(objects.size());progress.phase=liveRetirementPhase;
        for(const auto& item:objects)
            if(item.second.kind==Kind::SharedResource || item.second.kind==Kind::SharedSync) ++progress.sharedReferences;
        if(objects.empty()) { progress.phase=liveRetirementPhase=4;return progress; }
        if(progress.sharedReferences) { progress.flags=RetirementSharedReferences;return progress; }
        if(!driver.prepareLiveRetirement()) { progress.flags=RetirementGpuBusy;return progress; }
        queries.clear();
        while(liveRetirementPhase<4 && progress.attempted<LiveRetirementProfile{}.batch) {
            bool remain=false;
            auto cursor=objects.end();
            while(cursor!=objects.begin() && progress.attempted<LiveRetirementProfile{}.batch) {
                auto current=std::prev(cursor);const auto kind=current->second.kind;
                const bool queue=kind==Kind::HwQueue || kind==Kind::HwQueueSync;
                const bool selected=liveRetirementPhase==0 ? queue : liveRetirementPhase==1 ? !queue && kind!=Kind::GpuReservation && kind!=Kind::Adapter :
                                    liveRetirementPhase==2 ? kind==Kind::GpuReservation : kind==Kind::Adapter;
                if(!selected) { cursor=current;continue; }
                remain=true;
                if(std::any_of(objects.begin(),objects.end(),[&](const auto& child){return child.second.parent==current->first;})) {
                    cursor=current;continue;
                }
                ++progress.attempted;
                const auto result=driver.destroy(kind,current->second.nativeHandle);
                if(result.ntstatus>0 || result.nativeHandle || result.value) throw std::runtime_error("Malformed live native destruction result");
                if(!result.ntstatus) { cursor=objects.erase(current);++progress.destroyed; }
                else { progress.flags|=RetirementNativeBusy;progress.ntstatus=result.ntstatus;cursor=current; }
            }
            // Recheck after the pass: a removed borrowed child may unblock its
            // queue/parent in this same batch, while a failed child stays charged.
            if(remain) {
                const bool remaining=std::any_of(objects.begin(),objects.end(),[&](const auto& item) {
                    const auto kind=item.second.kind;const bool queue=kind==Kind::HwQueue || kind==Kind::HwQueueSync;
                    return liveRetirementPhase==0 ? queue : liveRetirementPhase==1 ? !queue && kind!=Kind::GpuReservation && kind!=Kind::Adapter :
                           liveRetirementPhase==2 ? kind==Kind::GpuReservation : kind==Kind::Adapter;
                });
                if(remaining) break;
            }
            ++liveRetirementPhase;
        }
        progress.remaining=static_cast<std::uint32_t>(objects.size());progress.phase=liveRetirementPhase;
        if(objects.empty()) { progress.phase=liveRetirementPhase=4;progress.flags=0;progress.ntstatus=0; }
        return progress;
    }
    static unsigned teardownPhase(Kind kind) {
        return kind==Kind::HwQueue || kind==Kind::HwQueueSync ? 0u :
               kind==Kind::GpuReservation ? 2u : kind==Kind::Adapter ? 3u : 1u;
    }
    void teardownObjectsPhase(unsigned phase) {
        if(!phase) queries.clear();
        // A failed queue can still retain allocations unrelated to its direct
        // parent chain. Keep this session's later phases blocked until every
        // earlier phase has actually completed, not merely been attempted.
        if(std::any_of(objects.begin(),objects.end(),[phase](const auto& item) {
            return teardownPhase(item.second.kind)<phase;
        })) return;
        // Count parent dependencies once instead of scanning the entire live
        // object table for every destruction. Allocation failure retains all
        // ownership; no native release is attempted with an incomplete index.
        std::map<std::uint32_t,std::size_t> children;
        try {
            for(const auto& item:objects) if(item.second.parent) ++children[item.second.parent];
        } catch(const std::bad_alloc&) { return; }
        auto cursor=objects.end();
        while(cursor!=objects.begin()) {
            auto current=std::prev(cursor);
            const auto childCount=children.find(current->first);
            if(teardownPhase(current->second.kind)!=phase ||
               (childCount!=children.end() && childCount->second)) { cursor=current;continue; }
            const auto result=driver.destroy(current->second.kind,current->second.nativeHandle);
            // Informational/malformed replies do not prove native release.
            // Retain the child so neither its parent nor a later phase runs.
            if(!result.ntstatus && !result.nativeHandle && !result.value) {
                const auto parent=children.find(current->second.parent);
                if(parent!=children.end() && parent->second) --parent->second;
                cursor=objects.erase(current);
            }
            else cursor=current;
        }
    }
public:
    // Trusted owner-only cleanup after proven VM exit. The coordinator runs
    // each phase across every Session before advancing: another actor's live
    // hardware queue must not prevent reservation release permanently.
    void ownerTeardownPhase(unsigned phase) {
        if(!coordinated || phase>3) throw std::runtime_error("Invalid coordinated teardown phase");
        teardownObjectsPhase(phase);
    }
    // Disconnect follows the same dependency checks. Failed native ownership
    // remains tracked by the Driver until its process-teardown backstop.
    ~Session() {
        for(unsigned phase=0;phase<4;++phase) teardownObjectsPhase(phase);
    }
    std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& packet) {
        if (packet.size() < sizeof(Header)) return {};
        Header h{}; std::memcpy(&h, packet.data(), sizeof h);
        if (packet.size() > native_gpu::MaxPacket || h.status || h.padding) return reply(h, -22);
        const auto op = static_cast<Op>(h.type);
        if(coordinated && !mayPresent && (op==Op::CaptureCompositorResource || op==Op::QueueCompositorFrame)) return reply(h,-13);
        if (op == Op::Hello) {
            if (h.handle || packet.size() != sizeof h + 4) return reply(h, -22);
            if (!objects.empty()) return reply(h, -16);
            std::uint32_t version{}; std::memcpy(&version, packet.data() + sizeof h, 4);
            negotiated = false; streamingNegotiated = false; openglNegotiated = false; syncSharingNegotiated = false;
            resourceSharingNegotiated = false;
            wireObjectLimit = MaxObjects;
            openglFenceNegotiated = false;
            openglCompositorNegotiated = false;
            openglRenderNegotiated = false;
            openglDesktopNegotiated = false;
            openglDesktop4kNegotiated = false;
            openglDesktop4kCpuNegotiated = false;
            openglDesktopSparseNegotiated = false;
            openglDesktopSlotsNegotiated = false;
            openglDesktopLargeMapNegotiated = false;
            fenceApertureBytes=FenceApertureBytes;
            if (version != Version) return reply(h, -93);
            const auto caps = driver.capabilities();
            if (caps.version != Version) return reply(h, -93);
            if(coordinated && std::memcmp(&caps,&coordinated->caps,sizeof caps)) return reply(h,-95);
            negotiated = true;
            auto out = reply(h, 0); out.resize(sizeof h + sizeof caps);
            std::memcpy(out.data() + sizeof h, &caps, sizeof caps);
            return out;
        }
        if (op == Op::BeginStreaming) {
            if(coordinated) return reply(h,-95);
            if (!negotiated) return reply(h, -71);
            if (h.handle || packet.size() != sizeof h) return reply(h, -22);
            if (streamingNegotiated || openglNegotiated ||
                std::any_of(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::VendorResource; }))
                return reply(h, -16);
            const auto required = NativeSharedTextureCapability | SharedTextureConsumeCapability | AsyncHwSubmitCapability | UINT32_C(0x80000000);
            if ((driver.capabilities().flags & required) != required) return reply(h, -95);
            const auto profile = driver.beginStreaming();
            if (!validStreamProfile(profile)) return reply(h, -95);
            streamingNegotiated = true;
            auto out = reply(h, 0); out.resize(sizeof h + sizeof profile);
            std::memcpy(out.data() + sizeof h, &profile, sizeof profile); return out;
        }
        if (op == Op::QueryNativeGpuBufferCaps) {
            if (!negotiated) return reply(h, -71);
            if (h.handle || packet.size() != sizeof h) return reply(h, -22);
            const auto caps = driver.nativeGpuBufferCaps();
            if (!validNativeGpuBufferCaps(caps)) return reply(h, -95);
            auto out = reply(h, 0); out.resize(sizeof h + sizeof caps);
            std::memcpy(out.data() + sizeof h, &caps, sizeof caps); return out;
        }
        if (op == Op::QueryNativeGpuBufferAdmission) {
            if (!negotiated) return reply(h, -71);
            if (h.handle || packet.size() != sizeof h + sizeof(NativeGpuBufferDesc)) return reply(h, -22);
            if (!validNativeGpuBufferCaps(driver.nativeGpuBufferCaps())) return reply(h, -95);
            NativeGpuBufferDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validNativeGpuBuffer(desc)) return reply(h, -22);
            const auto result = driver.queryNativeGpuBufferAdmission(desc);
            if (result.nativeHandle || result.value || result.ntstatus > 0) return reply(h, -5);
            return reply(h, 0, 0, &result);
        }
        if (op == Op::QueryNativeInteropCaps) {
            if(!negotiated) return reply(h,-71);
            if(h.handle || packet.size()!=sizeof h) return reply(h,-22);
            const auto caps=driver.nativeInteropCaps();
            if(!validNativeInteropCaps(caps)) return reply(h,-95);
            auto out=reply(h,0); out.resize(sizeof h+sizeof caps);
            std::memcpy(out.data()+sizeof h,&caps,sizeof caps); return out;
        }
        if (op == Op::BeginOpenGl) {
            if (!negotiated) return reply(h, -71);
            if (h.handle || (packet.size() != sizeof h && packet.size() != sizeof h + sizeof(std::uint32_t) && packet.size() != sizeof h + sizeof(OpenGlDesktop4kCapacity) && packet.size() != sizeof h + sizeof(OpenGlDesktopLargeMapCapacity) && packet.size() != sizeof h + sizeof(OpenGlDesktopPresentationCapacity))) return reply(h, -22);
            std::uint32_t version = 1;
            if (packet.size() != sizeof h) {
                std::memcpy(&version, packet.data() + sizeof h, sizeof version);
                if((version==11 || version==12 || version==13 || version==14) && !coordinated) return reply(h,-93);
                if (version != 2 && version != 3 && version != 4 && version != 5 && version != 6 && version != 7 && version != 8 && version != 9 && version != 10 && version!=11 && version!=12 && version!=13 && version!=14) return reply(h, -93);
            }
            if (version==10 || version==11 || version==12 || version==13 || version==14) {
                if(coordinated && version!=coordinated->gl.version) return reply(h,-95);
                if(packet.size()!=sizeof h+sizeof(OpenGlDesktopPresentationCapacity)) return reply(h,-22);
                OpenGlDesktopPresentationCapacity capacity{};std::memcpy(&capacity,packet.data()+sizeof h,sizeof capacity);
                if(version>=11 ? !coordinated || std::memcmp(&capacity,&coordinated->presentation,sizeof capacity) : !validOpenGlDesktopPresentationCapacity(capacity)) return reply(h,-22);
            } else if (version==9) {
                if (packet.size()!=sizeof h+sizeof(OpenGlDesktopLargeMapCapacity)) return reply(h,-22);
                OpenGlDesktopLargeMapCapacity capacity{};std::memcpy(&capacity,packet.data()+sizeof h,sizeof capacity);
                if (!validOpenGlDesktopLargeMapCapacity(capacity)) return reply(h,-22);
            } else if (version>=7) {
                if (packet.size()!=sizeof h+sizeof(OpenGlDesktop4kCapacity)) return reply(h,-22);
                OpenGlDesktop4kCapacity capacity{}; std::memcpy(&capacity,packet.data()+sizeof h,sizeof capacity);
                if (version==7 ? !validOpenGlDesktop4kCapacity(capacity) : !validOpenGlDesktopSparseCapacity(capacity)) return reply(h,-22);
            } else if (packet.size()!=sizeof h && packet.size()!=sizeof h+sizeof(std::uint32_t)) return reply(h,-22);
            if (openglNegotiated || streamingNegotiated || !objects.empty()) return reply(h, -16);
            const auto required = AsyncHwSubmitCapability | SyncCapability | GpuReservationCapability | UINT32_C(0x80000000) |
                (version>=6 ? ExpandedVendorGpuCapability : 0u) | (version>=7 ? ExpandedVendorCpuCapability : 0u);
            if ((driver.capabilities().flags & required) != required) return reply(h, -95);
            if(coordinated && version!=coordinated->gl.version) return reply(h,-95);
            const auto profile = coordinated ? coordinated->gl : driver.beginOpenGl(version);
            if (version == 1 ? !validOpenGlProfile(profile) : version == 2 ? !validOpenGlFenceProfile(profile) : version == 3 ? !validOpenGlCompositorProfile(profile) : version == 4 ? !validOpenGlCompositorRenderProfile(profile) : version == 5 ? !validOpenGlDesktopProfile(profile) : version==6 ? !validOpenGlDesktop4kProfile(profile) : version==7 ? !validOpenGlDesktop4kCpuProfile(profile) : version==8 ? !validOpenGlDesktopSparseProfile(profile) : version==9 ? !validOpenGlDesktopLargeMapProfile(profile) : version==14 ? !validOpenGlMultiappProfile(profile) : version==13 ? !validOpenGlDesktopReferenceProfile(profile) : version==12 ? !validOpenGlDesktopSlotProfile(profile) : version==11 ? !validOpenGlDesktopFenceProfile(profile) : !validOpenGlDesktopPresentationProfile(profile)) return reply(h, -95);
            openglNegotiated = true;
            fenceApertureBytes=version>=11 ? DesktopFenceApertureBytes : FenceApertureBytes;
            openglFenceNegotiated = version >= 2;
            openglCompositorNegotiated = version >= 3;
            openglRenderNegotiated = version >= 4;
            openglDesktopNegotiated = version >= 5;
            openglDesktop4kNegotiated = version >= 6;
            openglDesktop4kCpuNegotiated = version >= 7;
            openglDesktopSparseNegotiated = version >= 8;
            openglDesktopSlotsNegotiated = version >= 12;
            openglDesktopLargeMapNegotiated = version >= 9;
            if (version >= 2) wireObjectLimit = profile.objectLimit;
            auto out = reply(h, 0); out.resize(sizeof h + sizeof profile);
            std::memcpy(out.data() + sizeof h, &profile, sizeof profile); return out;
        }
        if (op >= Op::BeginAdapterQuery && op <= Op::EndAdapterQuery) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & QueryCapability)) return reply(h, -95);
            const auto adapter = objects.find(h.handle);
            if (adapter == objects.end() || adapter->second.kind != Kind::Adapter) return reply(h, -9);
            if (op == Op::BeginAdapterQuery) {
                if (packet.size() != sizeof h + sizeof(QueryDesc)) return reply(h, -22);
                QueryDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
                if (!validQuery(desc)) return reply(h, -22);
                if (queries.count(h.handle)) return reply(h, -16);
                if (queries.size() >= MaxActiveQueries) return reply(h, -24);
                queries.emplace(h.handle, Query{desc, std::vector<std::uint8_t>(desc.bytes)});
                const Result result{0, 0, desc.bytes}; return reply(h, 0, h.handle, &result);
            }
            const auto entry = queries.find(h.handle);
            if (entry == queries.end()) return reply(h, -2);
            auto& query = entry->second;
            if (op == Op::WriteAdapterQuery || op == Op::ReadAdapterQuery) {
                if (packet.size() < sizeof h + sizeof(Range)) return reply(h, -22);
                Range range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
                const bool write = op == Op::WriteAdapterQuery;
                if (!range.size || range.size > MaxChunk || range.size > query.desc.bytes ||
                    range.offset > query.desc.bytes - range.size ||
                    packet.size() != sizeof h + sizeof range + (write ? range.size : 0)) return reply(h, -22);
                if (write) {
                    // A complete, ordered initializer is mandatory. Reject
                    // holes, overlap, retries and writes after execution.
                    if (query.executed || range.offset != query.written) return reply(h, -71);
                    std::memcpy(query.data.data() + range.offset, packet.data() + sizeof h + sizeof range, range.size);
                    query.written += range.size;
                    const Result result{0, 0, query.written}; return reply(h, 0, h.handle, &result);
                }
                if (!query.executed) return reply(h, -71);
                auto out = reply(h, 0, h.handle, &query.result);
                out.insert(out.end(), query.data.begin() + range.offset, query.data.begin() + range.offset + range.size);
                return out;
            }
            if (packet.size() != sizeof h) return reply(h, -22);
            if (op == Op::EndAdapterQuery) {
                queries.erase(entry);
                const Result result{}; return reply(h, 0, h.handle, &result);
            }
            if (query.executed || query.written != query.desc.bytes) return reply(h, -71);
            query.result = driver.queryAdapter(adapter->second.nativeHandle, query.desc, query.data);
            query.executed = true;
            if (query.data.size() != query.desc.bytes) { queries.erase(entry); return reply(h, -5); }
            return reply(h, 0, h.handle, &query.result);
        }
        if (op == Op::SetContextInProcessPriority) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & ContextCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(std::int32_t)) return reply(h, -22);
            std::int32_t priority{}; std::memcpy(&priority, packet.data() + sizeof h, sizeof priority);
            if (!validContextPriority(priority)) return reply(h, -22);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != Kind::Context) return reply(h, -9);
            if (entry->second.contextFlags != 16) return reply(h, -95);
            const auto result = driver.setContextInProcessPriority(entry->second.nativeHandle, priority);
            if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateContext || op == Op::DestroyContext) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & ContextCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            const auto required = op == Op::CreateContext ? Kind::Device : Kind::Context;
            if (entry == objects.end() || entry->second.kind != required) return reply(h, -9);
            if (op == Op::DestroyContext) {
                if (packet.size() != sizeof h) return reply(h, -22);
                for (const auto& child : objects) if (child.second.parent == h.handle) return reply(h, -16);
                const auto result = driver.destroy(Kind::Context, entry->second.nativeHandle);
                if (result.ntstatus >= 0) objects.erase(entry);
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() < sizeof h + sizeof(ContextDesc)) return reply(h, -22);
            ContextDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validContext(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes) return reply(h, -22);
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX) return reply(h, -24);
            std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto result = driver.createContext(entry->second.nativeHandle, desc, data);
            if (data.size() != desc.privateBytes) {
                if (result.ntstatus >= 0 && result.nativeHandle) driver.destroy(Kind::Context, result.nativeHandle);
                return reply(h, -5);
            }
            auto out = insert(h, Kind::Context, h.handle, result, 0, 0, false, desc.flags);
            if (result.ntstatus >= 0 && result.nativeHandle) out.insert(out.end(), data.begin(), data.end());
            return out;
        }
        if (op == Op::CreateGuestPagingQueue) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & GuestPagingCapability)) return reply(h, -95);
            if (packet.size() != sizeof h) return reply(h, -22);
            const auto device = objects.find(h.handle);
            if (device == objects.end() || device->second.kind != Kind::Device) return reply(h, -9);
            if (objects.size() > objectLimit() - 2 || nextId >= UINT32_MAX - 1) return reply(h, -24);
            const auto native = driver.createGuestPagingQueue(device->second.nativeHandle);
            if (native.queue.ntstatus < 0) return reply(h, 0, 0, &native.queue);
            if (!native.queue.nativeHandle || !native.sync || native.queue.value ||
                native.offset % 8 || native.offset >= fenceApertureBytes) {
                if (native.queue.nativeHandle) driver.destroy(Kind::PagingQueue, native.queue.nativeHandle);
                return reply(h, -5);
            }
            const auto queue = nextId++, sync = nextId++;
            try {
                auto out = reply(h, 0, queue, &native.queue);
                const PagingReply body{sync, 0, native.offset};
                const auto start = out.size(); out.resize(start + sizeof body);
                std::memcpy(out.data() + start, &body, sizeof body);
                objects.emplace(queue, Object{Kind::PagingQueue, h.handle, native.queue.nativeHandle, 0, 0, false});
                objects.emplace(sync, Object{Kind::PagingSync, queue, native.sync, 0, 0, false});
                return out;
            } catch (...) {
                objects.erase(sync); objects.erase(queue);
                driver.destroy(Kind::PagingQueue, native.queue.nativeHandle);
                throw;
            }
        }
        if (op == Op::CreateHwQueue || op == Op::DestroyHwQueue) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & HwQueueCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != (op == Op::CreateHwQueue ? Kind::Context : Kind::HwQueue)) return reply(h, -9);
            if (op == Op::DestroyHwQueue) {
                if (packet.size() != sizeof h) return reply(h, -22);
                const auto result = driver.destroy(Kind::HwQueue, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) {
                    for (auto child = objects.begin(); child != objects.end(); )
                        if (child->second.parent == h.handle && child->second.kind == Kind::HwQueueSync) child = objects.erase(child);
                        else ++child;
                    objects.erase(entry);
                }
                return reply(h, 0, h.handle, &result);
            }
            if (entry->second.contextFlags != 16) return reply(h, -95);
            if (packet.size() < sizeof h + sizeof(HwQueueDesc)) return reply(h, -22);
            HwQueueDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validHwQueue(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes) return reply(h, -22);
            if ((desc.flags & NoBroadcastSignalHwQueueFlag) && !(driver.capabilities().flags & NoBroadcastSignalHwQueueCapability)) return reply(h, -95);
            if ((desc.flags & NoBroadcastWaitHwQueueFlag) && !(driver.capabilities().flags & NoBroadcastWaitHwQueueCapability)) return reply(h, -95);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::HwQueue; });
            if (objects.size() > objectLimit() - 2 || nextId >= UINT32_MAX - 1 || static_cast<std::size_t>(count) >= hwQueueLimit()) return reply(h, -24);
            std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto native = driver.createHwQueue(entry->second.nativeHandle, desc, data);
            if (data.size() != desc.privateBytes || native.queue.value ||
                (native.queue.ntstatus >= 0 && (native.queue.ntstatus != 0 || !native.queue.nativeHandle || !native.sync ||
                    native.sync == native.queue.nativeHandle || native.offset % 8 || native.offset >= fenceApertureBytes ||
                    !native.gpuAddress || native.gpuAddress % 8 || native.gpuAddress >= MaxGpuAddress)) ||
                (native.queue.ntstatus < 0 && (native.queue.nativeHandle || native.sync || native.offset || native.gpuAddress))) {
                if (native.queue.nativeHandle) driver.destroy(Kind::HwQueue, native.queue.nativeHandle);
                return reply(h, -5);
            }
            const auto queue = native.queue.ntstatus == 0 ? nextId++ : 0;
            const auto sync = queue ? nextId++ : 0;
            try {
                auto out = reply(h, 0, queue, &native.queue);
                const HwQueueReply body{sync, 0, native.offset, native.gpuAddress};
                const auto start = out.size(); out.resize(start + sizeof body); std::memcpy(out.data() + start, &body, sizeof body);
                out.insert(out.end(), data.begin(), data.end()); // Private in/out remains opaque, including native failure.
                if (queue) {
                    objects.emplace(queue, Object{Kind::HwQueue, h.handle, native.queue.nativeHandle, 0, 0, false});
                    objects.emplace(sync, Object{Kind::HwQueueSync, queue, native.sync, 0, 0, false});
                }
                return out;
            } catch (...) {
                objects.erase(sync); objects.erase(queue);
                if (native.queue.nativeHandle) driver.destroy(Kind::HwQueue, native.queue.nativeHandle);
                throw;
            }
        }
        if (op == Op::SubmitHwQueue || op == Op::QueueHwCommand) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & HwSubmitCapability)) return reply(h, -95);
            const bool asynchronous = op == Op::QueueHwCommand;
            if (asynchronous && !(driver.capabilities().flags & AsyncHwSubmitCapability)) return reply(h, -95);
            const auto queue = objects.find(h.handle);
            if (queue == objects.end() || queue->second.kind != Kind::HwQueue) return reply(h, -9);
            if (packet.size() < sizeof h + sizeof(HwSubmitDesc)) return reply(h, -22);
            HwSubmitDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validHwSubmit(desc) || packet.size() != sizeof h + sizeof desc + desc.privateBytes ||
                desc.fence <= queue->second.submittedFence) return reply(h, -22);
            const auto device = objects.at(queue->second.parent).parent;
            unsigned commandOwners = 0;
            for (const auto& item : objects) {
                const auto& allocation = item.second;
                if (allocation.kind != Kind::VendorAllocation || allocation.parent != device) continue;
                if (!allocation.gpuPages || desc.address < allocation.gpuAddress) continue;
                const auto offset = desc.address - allocation.gpuAddress;
                if (offset <= allocation.gpuPages * 4096ull && desc.bytes <= allocation.gpuPages * 4096ull - offset) {
                    // Unreferenced allocations may remain nonresident. This
                    // preflight validates the uniquely owned command buffer;
                    // opaque UMD references remain subject to native VidMm.
                    if (!allocation.resident || !allocation.cpuBytes || offset > allocation.cpuBytes ||
                        desc.bytes > allocation.cpuBytes - offset) return reply(h, -16);
                    ++commandOwners;
                }
            }
            if (commandOwners != 1) return reply(h, -9);
            if (!lifetimeBudgetAvailable(submissionAttempts,streamingNegotiated ? GuestStreamProfile.totalCommands :
                                      openglNegotiated ? openGlCommandLimit() : MaxHwSubmissions)) return reply(h, -24);
            ++submissionAttempts;
            const std::vector<std::uint8_t> data(packet.begin() + sizeof h + sizeof desc, packet.end());
            const auto native = asynchronous ? driver.queueHwCommand(queue->second.nativeHandle, desc, data) :
                                              driver.submitHwQueue(queue->second.nativeHandle, desc, data);
            if (!validHwSubmitReply(native, desc.fence, asynchronous ? AsyncHwSubmitCapability : 0)) return reply(h, -5);
            if (native.ntstatus == 0) {
                queue->second.submittedFence = desc.fence;
                submissionActivity.record(asynchronous,desc.bytes);
            }
            return reply(h, 0, h.handle, &native);
        }
        if (op == Op::BeginSyncSharing) {
            if (!negotiated) return reply(h, -71);
            if (h.handle || packet.size() != sizeof h) return reply(h, -22);
            if (syncSharingNegotiated) return reply(h, -16);
            const auto profile = coordinated ? coordinated->sync : driver.beginSyncSharing();
            if (!validSyncSharingProfile(profile)) return reply(h, -95);
            auto out = reply(h, 0); const auto start = out.size(); out.resize(start + sizeof profile);
            std::memcpy(out.data() + start, &profile, sizeof profile);
            syncSharingNegotiated = true;
            return out;
        }
        if (op == Op::QueueHwQueueWait || op == Op::QueueContextWait) {
            const bool contextStream = op == Op::QueueContextWait;
            if (!negotiated || !syncSharingNegotiated) return reply(h, -71);
            const auto required = (contextStream ? ContextCapability : HwQueueCapability) | AsyncHwSubmitCapability | SyncCapability;
            if ((driver.capabilities().flags & required) != required) return reply(h, -95);
            if (packet.size() < sizeof h + sizeof(HwQueueWaitDesc)) return reply(h, -22);
            HwQueueWaitDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.count || desc.count > MaxHwQueueWaitObjects || desc.reserved ||
                packet.size() != sizeof h + sizeof desc + desc.count * sizeof(HwQueueWaitItem)) return reply(h, -22);
            const auto queue = objects.find(h.handle);
            if (queue == objects.end() || queue->second.kind != (contextStream ? Kind::Context : Kind::HwQueue) ||
                (contextStream && !contextCanWait(queue->second.contextFlags,coordinated))) return reply(h, -9);
            const auto device = contextStream ? queue->second.parent : objects.at(queue->second.parent).parent;
            std::vector<HwQueueWaitItem> items(desc.count);
            std::memcpy(items.data(), packet.data() + sizeof h + sizeof desc, items.size() * sizeof(items[0]));
            for (std::size_t n = 0; n < items.size(); ++n) {
                const auto sync = objects.find(items[n].sync);
                if (items[n].reserved || items[n].fence == UINT64_MAX) return reply(h, -22);
                if(sync==objects.end()) return reply(h,-9);
                const bool independent=sync->second.kind==Kind::Sync && sync->second.syncType==5 && sync->second.parent==device;
                const auto paging=objects.find(sync->second.parent);
                const bool borrowed=coordinated && sync->second.kind==Kind::PagingSync && paging!=objects.end() &&
                    paging->second.kind==Kind::PagingQueue && paging->second.parent==device;
                const auto progressContext=paging==objects.end() ? objects.end() : objects.find(paging->second.parent);
                const bool hardware=coordinated && sync->second.kind==Kind::HwQueueSync && paging!=objects.end() && paging->second.kind==Kind::HwQueue &&
                    progressContext!=objects.end() && progressContext->second.kind==Kind::Context && progressContext->second.parent==device;
                if(!independent && !borrowed && !hardware) return reply(h,-9);
                for (std::size_t previous = 0; previous < n; ++previous)
                    if (items[previous].sync == sync->second.nativeHandle) return reply(h, -22);
                items[n].sync = sync->second.nativeHandle;
            }
            const auto result = contextStream ? driver.queueContextWait(queue->second.nativeHandle, items) :
                                                driver.queueHwQueueWait(queue->second.nativeHandle, items);
            if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::BeginResourceSharing) {
            if (!negotiated) return reply(h, -71);
            if (h.handle || packet.size() != sizeof h) return reply(h, -22);
            if (resourceSharingNegotiated) return reply(h, -16);
            const auto required = SharedVendorResourceCapability | NativeSharedTextureCapability;
            if ((driver.capabilities().flags & required) != required) return reply(h, -95);
            const auto profile = coordinated ? coordinated->resource : driver.beginResourceSharing();
            if (!(coordinated ? validCoordinatedResourceSharingProfile(profile) : validResourceSharingProfile(profile))) return reply(h, -95);
            resourceSharingNegotiated = true;
            auto out = reply(h, 0); out.resize(sizeof h + sizeof profile);
            std::memcpy(out.data() + sizeof h, &profile, sizeof profile);
            return out;
        }
        if (op == Op::QuerySharedResourceInfo) {
            if (!negotiated || !resourceSharingNegotiated) return reply(h, -71);
            if (packet.size() != sizeof h + sizeof(SharedResourceQueryDesc)) return reply(h, -22);
            SharedResourceQueryDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.device || desc.runtimeCapacity > MaxSharedResourceRuntimeBytes) return reply(h, -22);
            const auto ticket = objects.find(h.handle), device = objects.find(desc.device);
            if (ticket == objects.end() || ticket->second.kind != Kind::SharedResource || device == objects.end() ||
                device->second.kind != Kind::Device || !resourceAdaptersCompatible(device->second.parent,ticket->second.parent)) return reply(h, -9);
            SharedResourceInfo info{}; std::vector<std::uint8_t> runtime;
            const auto result = driver.querySharedResourceInfo(ticket->second.nativeHandle, device->second.nativeHandle, info, runtime);
            if (result.ntstatus > 0 || result.nativeHandle || result.value ||
                (result.ntstatus < 0 ? (info.runtimeBytes || info.driverBytes || info.resourceDriverBytes || info.allocations || !runtime.empty()) :
                 (!validSharedResourceInfo(info) || runtime.size() != info.runtimeBytes))) return reply(h, -5);
            if (!result.ntstatus && desc.runtimeCapacity && desc.runtimeCapacity != info.runtimeBytes) return reply(h, -22);
            auto out = reply(h, 0, h.handle, &result);
            out.resize(out.size() + sizeof info + desc.runtimeCapacity);
            std::memcpy(out.data() + sizeof h + sizeof(Reply), &info, sizeof info);
            if (!result.ntstatus && desc.runtimeCapacity)
                std::memcpy(out.data() + sizeof h + sizeof(Reply) + sizeof info, runtime.data(), runtime.size());
            return out;
        }
        if (op == Op::ImportResource) {
            if (!negotiated || !resourceSharingNegotiated) return reply(h, -71);
            if (packet.size() != sizeof h + sizeof(SharedResourceOpenDesc)) return reply(h, -22);
            SharedResourceOpenDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validSharedResourceOpen(desc)) return reply(h, -22);
            const auto ticket = objects.find(h.handle), device = objects.find(desc.device);
            if (ticket == objects.end() || ticket->second.kind != Kind::SharedResource || device == objects.end() ||
                device->second.kind != Kind::Device || !resourceAdaptersCompatible(device->second.parent,ticket->second.parent)) return reply(h, -9);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::VendorAllocation; });
            if (objects.size() > objectLimit() - 2 || nextId >= UINT32_MAX - 1 || static_cast<std::size_t>(count) >= vendorObjectLimit()) return reply(h, -24);
            const auto dataBytes = desc.runtimeBytes + desc.resourceDriverBytes + desc.driverBytes;
            std::vector<std::uint8_t> data(dataBytes);
            auto nativeDesc = desc; nativeDesc.device = device->second.nativeHandle;
            const auto opened = driver.importResource(ticket->second.nativeHandle, nativeDesc, data);
            const auto result = opened.allocation; const auto& info = opened.info;
            if (data.size() != dataBytes || result.ntstatus > 0 || info.reserved ||
                (result.ntstatus < 0 ? (result.nativeHandle || result.value || info.resource || info.allocationDriverBytes || info.allocationDriverOffset) :
                 (!result.nativeHandle || !info.resource || info.resource == result.nativeHandle || result.value % 4096 || result.value >= MaxGpuAddress ||
                  !info.allocationDriverBytes || info.allocationDriverOffset > desc.driverBytes || info.allocationDriverBytes > desc.driverBytes - info.allocationDriverOffset))) {
                if (result.nativeHandle) driver.destroy(Kind::VendorAllocation, result.nativeHandle);
                return reply(h, -5);
            }
            const auto id = result.ntstatus == 0 ? nextId++ : 0;
            const auto resourceId = id ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &result);
                auto output = info; output.resource = resourceId;
                const auto bytes = reinterpret_cast<const std::uint8_t*>(&output);
                out.insert(out.end(), bytes, bytes + sizeof output); out.insert(out.end(), data.begin(), data.end());
                if (id) {
                    Object allocation{Kind::VendorAllocation, desc.device, result.nativeHandle, 0, 0, false}; allocation.vendorResource = resourceId;
                    allocation.openedSharedResource = true;
                    if (!objects.emplace(id, allocation).second || !objects.emplace(resourceId, Object{Kind::VendorResource, id, info.resource, 0, 0, true}).second)
                        throw std::runtime_error("Imported resource identity collision");
                }
                return out;
            } catch (...) {
                objects.erase(id); objects.erase(resourceId);
                if (result.nativeHandle) driver.destroy(Kind::VendorAllocation, result.nativeHandle);
                throw;
            }
        }
        if (op == Op::ExportResource || op == Op::CloseSharedResource) {
            if (!negotiated || !resourceSharingNegotiated) return reply(h, -71);
            if (packet.size() != sizeof h) return reply(h, -22);
            const auto source = objects.find(h.handle);
            if (source == objects.end() || source->second.kind !=
                (op == Op::ExportResource ? Kind::VendorResource : Kind::SharedResource)) return reply(h, -9);
            if (op == Op::CloseSharedResource) {
                const auto result = driver.destroy(Kind::SharedResource, source->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (!result.ntstatus) objects.erase(source);
                return reply(h, 0, h.handle, &result);
            }
            if (!source->second.shared) return reply(h, -22);
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX ||
                static_cast<std::size_t>(std::count_if(objects.begin(), objects.end(), [](const auto& item) {
                    return item.second.kind == Kind::SharedResource; })) >= resourceReferenceLimit()) return reply(h, -24);
            const auto result = driver.exportResource(source->second.nativeHandle);
            if (result.ntstatus > 0 || result.value || (result.ntstatus < 0 ? result.nativeHandle != 0 : result.nativeHandle == 0)) {
                if (result.nativeHandle) driver.destroy(Kind::SharedResource, result.nativeHandle);
                return reply(h, -5);
            }
            const auto device = objects.at(source->second.parent).parent;
            const auto adapter = objects.at(device).parent;
            const auto id = result.ntstatus == 0 ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &result);
                if (id && !objects.emplace(id, Object{Kind::SharedResource, adapter, result.nativeHandle, 0, 0, true}).second)
                    throw std::runtime_error("Shared resource identity collision");
                return out;
            } catch (...) { if (result.nativeHandle) driver.destroy(Kind::SharedResource, result.nativeHandle); throw; }
        }
        if (op == Op::QuerySharedSyncInfo) {
            if (!negotiated || !syncSharingNegotiated) return reply(h, -71);
            if (packet.size() != sizeof h + sizeof(SharedSyncInfoDesc)) return reply(h, -22);
            SharedSyncInfoDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.device || desc.reserved) return reply(h, -22);
            const auto ticket = objects.find(h.handle), device = objects.find(desc.device);
            if (ticket == objects.end() || ticket->second.kind != Kind::SharedSync ||
                device == objects.end() || device->second.kind != Kind::Device ||
                device->second.parent != ticket->second.parent) return reply(h, -9);
            const auto result = driver.querySharedSyncInfo(ticket->second.nativeHandle, device->second.nativeHandle);
            // This ticket is a monitored fence, never a resource. Preserve
            // the native failure used by D3D12 to select its fence import.
            if (result.ntstatus >= 0 || result.nativeHandle || result.value) return reply(h, -5);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::ExportSync || op == Op::ImportSync || op == Op::CloseSharedSync) {
            if (!negotiated || !syncSharingNegotiated) return reply(h, -71);
            const auto entry = objects.find(h.handle);
            const auto expected = op == Op::ExportSync ? Kind::Sync : Kind::SharedSync;
            if (entry == objects.end() || entry->second.kind != expected) return reply(h, -9);
            if (op == Op::CloseSharedSync) {
                if (packet.size() != sizeof h) return reply(h, -22);
                const auto result = driver.destroy(Kind::SharedSync, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (!result.ntstatus) objects.erase(entry);
                return reply(h, 0, h.handle, &result);
            }
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX) return reply(h, -24);
            if (op == Op::ExportSync) {
                if (packet.size() != sizeof h) return reply(h, -22);
                if (entry->second.syncType != 5 || !entry->second.shared) return reply(h, -22);
                const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::SharedSync; });
                if (static_cast<std::size_t>(count) >= MaxSharedSyncHandles) return reply(h, -24);
                const auto result = driver.exportSync(entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.value || (result.ntstatus < 0 ? result.nativeHandle != 0 : result.nativeHandle == 0)) {
                    if (result.nativeHandle) driver.destroy(Kind::SharedSync, result.nativeHandle);
                    return reply(h, -5);
                }
                // The NT reference outlives the original device. Its adapter
                // remains owned; destroying the original sync does not revoke it.
                const auto adapter = objects.at(entry->second.parent).parent;
                const auto id = result.ntstatus == 0 ? nextId++ : 0;
                try {
                    auto out = reply(h, 0, id, &result);
                    if (id) { Object shared{Kind::SharedSync, adapter, result.nativeHandle, 0, 0, true};
                        shared.syncFlags = entry->second.syncFlags; objects.emplace(id, shared); }
                    return out;
                } catch (...) { if (result.nativeHandle) driver.destroy(Kind::SharedSync, result.nativeHandle); throw; }
            }
            if (packet.size() != sizeof h + sizeof(SharedSyncOpenDesc)) return reply(h, -22);
            SharedSyncOpenDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validSharedSyncOpen(desc)) return reply(h, -22);
            const auto device = objects.find(desc.device);
            if (device == objects.end() || device->second.kind != Kind::Device || device->second.parent != entry->second.parent) return reply(h, -9);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::Sync; });
            if (static_cast<std::size_t>(count) >= syncObjectLimit()) return reply(h, -24);
            const auto parent = desc.device; desc.device = device->second.nativeHandle;
            const auto native = driver.importSync(entry->second.nativeHandle, desc);
            const SyncDesc importedDesc{5, desc.flags, desc.affinity, 0, 0};
            if (native.object.ntstatus > 0 || native.object.value ||
                (native.object.ntstatus < 0 ? (native.object.nativeHandle || native.output.offset || native.output.gpuAddress) :
                 (!native.object.nativeHandle || native.output.offset % 8 || native.output.offset >= fenceApertureBytes ||
                  !validSyncGpuAddress(importedDesc, native.output.gpuAddress)))) {
                if (native.object.nativeHandle) driver.destroy(Kind::Sync, native.object.nativeHandle);
                return reply(h, -5);
            }
            const auto id = native.object.ntstatus == 0 ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &native.object);
                const auto start = out.size(); out.resize(start + sizeof native.output);
                std::memcpy(out.data() + start, &native.output, sizeof native.output);
                if (id) { Object owned{Kind::Sync, parent, native.object.nativeHandle, 0, 0, true};
                    owned.syncType = 5; owned.syncFlags = desc.flags; objects.emplace(id, owned); }
                return out;
            } catch (...) { if (native.object.nativeHandle) driver.destroy(Kind::Sync, native.object.nativeHandle); throw; }
        }
        if (op == Op::CreateSync || op == Op::DestroySync) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & SyncCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != (op == Op::CreateSync ? Kind::Device : Kind::Sync)) return reply(h, -9);
            if (op == Op::DestroySync) {
                if (packet.size() != sizeof h) return reply(h, -22);
                const auto result = driver.destroy(Kind::Sync, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) objects.erase(entry);
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() != sizeof h + sizeof(SyncDesc)) return reply(h, -22);
            SyncDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validSync(desc)) return reply(h, -22);
            if (desc.flags == NtSharedGpuSyncFlags && !syncSharingNegotiated) return reply(h, -71);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::Sync; });
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX || static_cast<std::size_t>(count) >= syncObjectLimit()) return reply(h, -24);
            const auto native = driver.createSync(entry->second.nativeHandle, desc);
            if (native.object.value || (native.object.ntstatus >= 0 && (native.object.ntstatus != 0 || !native.object.nativeHandle ||
                    (desc.type == 1 && (native.output.offset || native.output.gpuAddress)) ||
                    (desc.type == 5 && (native.output.offset % 8 || native.output.offset >= fenceApertureBytes ||
                        !validSyncGpuAddress(desc, native.output.gpuAddress))))) ||
                (native.object.ntstatus < 0 && (native.object.nativeHandle || native.output.offset || native.output.gpuAddress))) {
                if (native.object.nativeHandle) driver.destroy(Kind::Sync, native.object.nativeHandle);
                return reply(h, -5);
            }
            const auto id = native.object.ntstatus == 0 ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &native.object);
                const auto start = out.size(); out.resize(start + sizeof native.output);
                std::memcpy(out.data() + start, &native.output, sizeof native.output);
                if (id) {
                    Object owned{Kind::Sync, h.handle, native.object.nativeHandle, 0, 0, false}; owned.syncType = desc.type;
                    owned.syncFlags = desc.flags; owned.submittedFence = desc.initial; owned.shared = (desc.flags & 3) == 3;
                    objects.emplace(id, owned);
                }
                return out;
            } catch (...) {
                if (native.object.nativeHandle) driver.destroy(Kind::Sync, native.object.nativeHandle);
                throw;
            }
        }
        if (op == Op::QueueHwQueueSignal) {
            if (!negotiated) return reply(h,-71);
            if (!(driver.capabilities().flags & HwQueueSignalCapability)) return reply(h,-95);
            if (packet.size() < sizeof h + sizeof(HwQueueSignalDesc)) return reply(h,-22);
            HwQueueSignalDesc desc{}; std::memcpy(&desc,packet.data()+sizeof h,sizeof desc);
            if (!validHwQueueSignal(desc) || packet.size() != sizeof h + sizeof desc + desc.count * sizeof(std::uint32_t))
                return reply(h,-22);
            const auto sync = objects.find(h.handle);
            if (sync == objects.end() || sync->second.kind != Kind::Sync || sync->second.syncType != 5 ||
                !packetSignalSyncFlags(sync->second.syncFlags)) return reply(h,-9);
            if (desc.fence <= sync->second.submittedFence) return reply(h,-22);
            std::vector<std::uint32_t> ids(desc.count), nativeQueues;
            std::memcpy(ids.data(),packet.data()+sizeof h+sizeof desc,desc.count*sizeof(std::uint32_t));
            for (std::size_t n=0;n<ids.size();++n) {
                const auto queue = objects.find(ids[n]);
                if (queue == objects.end() || queue->second.kind != Kind::HwQueue ||
                    objects.at(queue->second.parent).parent != sync->second.parent ||
                    std::find(ids.begin(),ids.begin()+n,ids[n]) != ids.begin()+n) return reply(h,-9);
                nativeQueues.push_back(queue->second.nativeHandle);
            }
            if (streamingNegotiated ? hwQueueSignalAttempts + contextSignalAttempts >= GuestStreamProfile.totalSignals :
                                      !lifetimeBudgetAvailable(hwQueueSignalAttempts,hwQueueSignalLimit())) return reply(h,-24);
            ++hwQueueSignalAttempts;
            const auto result = driver.queueHwQueueSignal(sync->second.nativeHandle,nativeQueues,desc);
            if (!validHwSubmitReply(result,desc.fence,AsyncHwSubmitCapability)) return reply(h,-5);
            if (result.ntstatus == 0) sync->second.submittedFence = desc.fence;
            return reply(h,0,h.handle,&result);
        }
        if (op == Op::SignalContextSync || op == Op::QueueContextSync) {
            const bool asynchronous = op == Op::QueueContextSync;
            if (!negotiated) return reply(h, -71);
            if (asynchronous && !syncSharingNegotiated) return reply(h, -71);
            if (asynchronous && !(driver.capabilities().flags & AsyncHwSubmitCapability)) return reply(h, -95);
            if (!(driver.capabilities().flags & ContextSignalCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(ContextSignalDesc)) return reply(h, -22);
            ContextSignalDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validContextSignal(desc)) return reply(h, -22);
            const auto context = objects.find(h.handle), sync = objects.find(desc.sync);
            if (context == objects.end() || context->second.kind != Kind::Context || !contextCanSignal(context->second.contextFlags) ||
                sync == objects.end() || sync->second.kind != Kind::Sync || sync->second.syncType != 5 ||
                !packetSignalSyncFlags(sync->second.syncFlags) || sync->second.parent != context->second.parent) return reply(h, -9);
            if (desc.fence <= sync->second.submittedFence) return reply(h, -22);
            if (streamingNegotiated ? contextSignalAttempts + hwQueueSignalAttempts >= GuestStreamProfile.totalSignals :
                                      !lifetimeBudgetAvailable(contextSignalAttempts,openglDesktopNegotiated ? 0 : MaxContextSignals)) return reply(h, -24);
            ++contextSignalAttempts;
            const auto result = asynchronous ? driver.queueContextSync(context->second.nativeHandle, sync->second.nativeHandle, desc) :
                                               driver.signalContextSync(context->second.nativeHandle, sync->second.nativeHandle, desc);
            if (result.ntstatus > 0 || result.nativeHandle ||
                (result.ntstatus == 0 && ((!asynchronous && result.value < desc.fence) || result.value == UINT64_MAX)) ||
                (result.ntstatus < 0 && result.value)) return reply(h, -5);
            if (result.ntstatus == 0) sync->second.submittedFence = desc.fence;
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::DestroyVendorResource) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorResourceCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + 4) return reply(h, -22);
            std::uint32_t deviceId{}; std::memcpy(&deviceId, packet.data() + sizeof h, 4);
            const auto resource = objects.find(h.handle);
            if (resource == objects.end() || resource->second.kind != Kind::VendorResource) return reply(h, -9);
            const auto allocation = objects.find(resource->second.parent);
            if (allocation == objects.end() || allocation->second.parent != deviceId ||
                allocation->second.kind != Kind::VendorAllocation || allocation->second.vendorResource != h.handle) return reply(h, -9);
            const Header destroyHeader{static_cast<std::uint32_t>(Op::DestroyVendorAllocations), deviceId, 0, 0};
            const DestroyVendorDesc desc{1, 0}; const auto id = allocation->first;
            std::vector<std::uint8_t> destroyPacket(sizeof destroyHeader + sizeof desc + sizeof id);
            std::memcpy(destroyPacket.data(), &destroyHeader, sizeof destroyHeader);
            std::memcpy(destroyPacket.data() + sizeof destroyHeader, &desc, sizeof desc);
            std::memcpy(destroyPacket.data() + sizeof destroyHeader + sizeof desc, &id, sizeof id);
            auto out = dispatch(destroyPacket);
            Header response{}; std::memcpy(&response, out.data(), sizeof response);
            response.type = h.type; response.handle = response.status ? 0 : h.handle;
            std::memcpy(out.data(), &response, sizeof response); return out;
        }
        if (op == Op::CreateVendorAllocation || op == Op::CreateVendorResourceAllocation ||
            op == Op::CreateSharedVendorResourceAllocation || op == Op::CreateNativeSharedVendorResourceAllocation ||
            op == Op::CreateNativeInteropResource || op == Op::CreateNativeSharedGpuBufferAllocation || op == Op::DestroyVendorAllocations) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorAllocationCapability)) return reply(h, -95);
            const bool interopTexture=op==Op::CreateNativeInteropResource;
            const bool nativeTexture = op == Op::CreateNativeSharedVendorResourceAllocation || interopTexture;
            const bool nativeBuffer = op == Op::CreateNativeSharedGpuBufferAllocation;
            const bool sharedResource = op == Op::CreateSharedVendorResourceAllocation || nativeTexture || nativeBuffer;
            const bool withResource = op == Op::CreateVendorResourceAllocation || sharedResource;
            if (withResource && !(driver.capabilities().flags & VendorResourceCapability)) return reply(h, -95);
            if (sharedResource && !(driver.capabilities().flags & SharedVendorResourceCapability)) return reply(h, -95);
            if (nativeTexture && !(driver.capabilities().flags & NativeSharedTextureCapability)) return reply(h, -95);
            if(interopTexture && !validNativeInteropCaps(driver.nativeInteropCaps())) return reply(h,-95);
            if (nativeBuffer && !validNativeGpuBufferCaps(driver.nativeGpuBufferCaps())) return reply(h, -95);
            const auto device = objects.find(h.handle);
            if (device == objects.end() || device->second.kind != Kind::Device) return reply(h, -9);
            if (op == Op::DestroyVendorAllocations) {
                // Opt-in native retirement uses VidMm's documented deferred
                // destruction contract rather than decoding private queue data.
                if (!(driver.capabilities().flags & VendorRetirementCapability))
                    for (const auto& queue : objects)
                        if (queue.second.kind == Kind::HwQueue && objects.at(queue.second.parent).parent == h.handle) return reply(h, -16);
                if (packet.size() < sizeof h + sizeof(DestroyVendorDesc)) return reply(h, -22);
                DestroyVendorDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
                if (desc.reserved || !desc.count || desc.count > MaxVendorAllocations ||
                    packet.size() != sizeof h + sizeof desc + desc.count * sizeof(std::uint32_t)) return reply(h, -22);
                std::vector<std::uint32_t> ids(desc.count), native(desc.count);
                std::memcpy(ids.data(), packet.data() + sizeof h + sizeof desc, ids.size() * sizeof(ids[0]));
                for (std::size_t n = 0; n < ids.size(); ++n) {
                    const auto entry = objects.find(ids[n]);
                    if (entry == objects.end() || entry->second.kind != Kind::VendorAllocation || entry->second.parent != h.handle)
                        return reply(h, -9);
                    if (std::find(ids.begin(), ids.begin() + n, ids[n]) != ids.begin() + n) return reply(h, -22);
                    if (entry->second.cpuBytes) return reply(h, -16);
                    if (entry->second.vendorResource && ids.size() != 1) return reply(h, -22);
                    native[n] = entry->second.nativeHandle;
                }
                const auto result = driver.destroyVendorAllocations(device->second.nativeHandle, native);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) for (const auto id : ids) {
                    if (objects.at(id).vendorResource) objects.erase(objects.at(id).vendorResource);
                    vendorMappedPages -= objects.at(id).gpuPages; objects.erase(id);
                }
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() < sizeof h + sizeof(VendorAllocationDesc)) return reply(h, -22);
            VendorAllocationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            SharedVendorAllocationDesc sharedDesc{};
            NativeSharedVendorAllocationDesc nativeDesc{};
            NativeSharedGpuBufferAllocationDesc bufferDesc{};
            const auto descriptorBytes = nativeBuffer ? sizeof bufferDesc : nativeTexture ? sizeof nativeDesc : sharedResource ? sizeof sharedDesc : sizeof desc;
            if (sharedResource) {
                if (packet.size() < sizeof h + sizeof sharedDesc) return reply(h, -22);
                std::memcpy(&sharedDesc, packet.data() + sizeof h, sizeof sharedDesc);
                if (!validSharedVendorAllocation(sharedDesc)) return reply(h, -22);
            }
            if (nativeTexture) {
                if (packet.size() < sizeof h + sizeof nativeDesc) return reply(h, -22);
                std::memcpy(&nativeDesc, packet.data() + sizeof h, sizeof nativeDesc);
                if (interopTexture ? !validNativeInteropAllocation(nativeDesc) : !validNativeSharedVendorAllocation(nativeDesc)) return reply(h, -22);
            }
            if (nativeBuffer) {
                if (packet.size() < sizeof h + sizeof bufferDesc) return reply(h, -22);
                std::memcpy(&bufferDesc, packet.data() + sizeof h, sizeof bufferDesc);
                if (!validNativeSharedGpuBufferAllocation(bufferDesc)) return reply(h, -22);
            }
            const auto dataBytes = desc.privateBytes + (sharedResource ? sharedDesc.runtimeBytes : 0);
            if (!validVendorAllocation(desc) || packet.size() != sizeof h + descriptorBytes + dataBytes) return reply(h, -22);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::VendorAllocation; });
            if (objects.size() > objectLimit() - (withResource ? 2 : 1) || nextId >= UINT32_MAX - (withResource ? 1 : 0) || static_cast<std::size_t>(count) >= vendorObjectLimit()) return reply(h, -24);
            std::vector<std::uint8_t> data(packet.begin() + sizeof h + descriptorBytes, packet.end());
            const auto created = nativeBuffer ? driver.createNativeSharedGpuBufferAllocation(device->second.nativeHandle, bufferDesc, data) :
                                interopTexture ? driver.createNativeInteropResource(device->second.nativeHandle,nativeDesc,data) :
                                nativeTexture ? driver.createNativeSharedVendorResourceAllocation(device->second.nativeHandle, nativeDesc, data) :
                                sharedResource ? driver.createSharedVendorResourceAllocation(device->second.nativeHandle, sharedDesc, data) :
                                withResource ? driver.createVendorResourceAllocation(device->second.nativeHandle, desc, data) :
                                               VendorResourceResult{driver.createVendorAllocation(device->second.nativeHandle, desc, data), 0};
            const auto result = created.allocation;
            if (data.size() != dataBytes || (result.ntstatus >= 0 && (!result.nativeHandle || result.value % 4096)) ||
                (result.ntstatus >= 0 && withResource && (!created.resource || created.resource == result.nativeHandle)) ||
                (result.ntstatus < 0 && (result.nativeHandle || result.value || created.resource))) {
                if (result.nativeHandle) driver.destroy(Kind::VendorAllocation, result.nativeHandle);
                return reply(h, -5);
            }
            const auto id = result.ntstatus >= 0 ? nextId++ : 0;
            const auto resourceId = id && withResource ? nextId++ : 0;
            try {
                auto out = reply(h, 0, id, &result);
                if (withResource) {
                    const VendorResourceReply resource{resourceId, 0};
                    const auto bytes = reinterpret_cast<const std::uint8_t*>(&resource);
                    out.insert(out.end(), bytes, bytes + sizeof resource);
                }
                out.insert(out.end(), data.begin(), data.end()); // Preserve in/out on native failure too.
                if (id) {
                    Object allocation{Kind::VendorAllocation, h.handle, result.nativeHandle, 0, 0, false}; allocation.vendorResource = resourceId;
                    objects.emplace(id, allocation);
                    if (resourceId) objects.emplace(resourceId, Object{Kind::VendorResource, id, created.resource, 0, 0, sharedResource});
                }
                return out;
            } catch (...) {
                objects.erase(id); objects.erase(resourceId);
                if (result.nativeHandle) driver.destroy(Kind::VendorAllocation, result.nativeHandle);
                throw;
            }
        }
        if (op == Op::CaptureCompositorResource) {
            if (!negotiated || !openglRenderNegotiated || !resourceSharingNegotiated || !syncSharingNegotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & SharedTextureConsumeCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(CompositorCaptureDesc)) return reply(h, -22);
            CompositorCaptureDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validCompositorCapture(desc)) return reply(h, -22);
            const auto resource = objects.find(h.handle), sync = objects.find(desc.syncTicket);
            if (resource == objects.end() || resource->second.kind != Kind::SharedResource ||
                sync == objects.end() || sync->second.kind != Kind::SharedSync || (sync->second.syncFlags & 3) != 3 ||
                !resourceAdaptersCompatible(resource->second.parent, sync->second.parent)) return reply(h, -9);
            if (compositorQueueUsed) return reply(h, -16);
            if (compositorCaptureAttempts >= 2) return reply(h, -24);
            ++compositorCaptureAttempts;
            auto native = desc; native.syncTicket = sync->second.nativeHandle;
            const auto result = driver.captureCompositorResource(resource->second.nativeHandle, sync->second.nativeHandle, native);
            if (result.nativeHandle || result.ntstatus > 0 || (result.ntstatus < 0 && result.value)) return reply(h, -5);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueueCompositorFrame) {
            if(compositorQueueEnded) return reply(h,-108);
            if (!negotiated || !openglRenderNegotiated || !resourceSharingNegotiated || !syncSharingNegotiated) return reply(h,-71);
            if (!(driver.capabilities().flags & SharedTextureConsumeCapability)) return reply(h,-95);
            if (packet.size()!=sizeof h+sizeof(CompositorCaptureDesc)) return reply(h,-22);
            CompositorCaptureDesc desc{}; std::memcpy(&desc,packet.data()+sizeof h,sizeof desc);
            if (!validCompositorFrame(desc)) return reply(h,-22);
            const auto resource=objects.find(h.handle),sync=objects.find(desc.syncTicket);
            if (resource==objects.end() || resource->second.kind!=Kind::SharedResource || sync==objects.end() ||
                sync->second.kind!=Kind::SharedSync || (sync->second.syncFlags&3)!=3 ||
                !resourceAdaptersCompatible(resource->second.parent,sync->second.parent)) return reply(h,-9);
            if (compositorCaptureAttempts) return reply(h,-16);
            if (compositorQueuedFrames==UINT64_MAX-1 || ((desc.flags&CompositorFrameAudit) && compositorQueueAudits>=2)) return reply(h,-24);
            compositorQueueUsed=true;
            if (desc.flags&CompositorFrameAudit) ++compositorQueueAudits;
            auto native=desc; native.syncTicket=sync->second.nativeHandle;
            const auto result=driver.queueCompositorFrame(resource->second.nativeHandle,sync->second.nativeHandle,native);
            if (result.nativeHandle || result.ntstatus>0 ||
                (result.ntstatus<0 ? result.value!=0 : result.value!=compositorQueuedFrames+1)) return reply(h,-5);
            if ((desc.flags&CompositorFrameAudit) && result.ntstatus==static_cast<std::int32_t>(0xc000022du)) --compositorQueueAudits;
            if (!result.ntstatus) {
                ++compositorQueuedFrames;
                if(desc.flags&CompositorFrameFinal) compositorQueueEnded=true;
            }
            return reply(h,0,h.handle,&result);
        }
        if (op == Op::ConsumeSharedTexture || op == Op::ConsumeStreamingTexture) {
            if (!negotiated) return reply(h, -71);
            const bool streaming = op == Op::ConsumeStreamingTexture;
            if (streaming != streamingNegotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & SharedTextureConsumeCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(SharedTextureConsumeDesc)) return reply(h, -22);
            SharedTextureConsumeDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validSharedTextureConsume(desc)) return reply(h, -22);
            const auto resource = objects.find(h.handle), sync = objects.find(desc.sync);
            if (resource == objects.end() || resource->second.kind != Kind::VendorResource || !resource->second.shared ||
                sync == objects.end() || sync->second.kind != Kind::Sync || sync->second.syncType != 5 ||
                sync->second.syncFlags != NoGpuAccessSyncFlag) return reply(h, -9);
            const auto allocation = objects.find(resource->second.parent);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                allocation->second.parent != sync->second.parent) return reply(h, -9);
            auto nativeDesc = desc; nativeDesc.sync = sync->second.nativeHandle;
            const auto result = streaming ? driver.consumeStreamingTexture(resource->second.nativeHandle, sync->second.nativeHandle, nativeDesc) :
                                           driver.consumeSharedTexture(resource->second.nativeHandle, sync->second.nativeHandle, nativeDesc);
            if (result.nativeHandle || result.ntstatus > 0 || (result.ntstatus < 0 && result.value)) return reply(h, -5);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::TranslateVendorAllocation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorTranslationCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(VendorTranslationDesc)) return reply(h, -22);
            VendorTranslationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.device || !desc.adapter || desc.reserved || desc.reserved2) return reply(h, -22);
            const auto allocation = objects.find(h.handle), device = objects.find(desc.device), adapter = objects.find(desc.adapter);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                device == objects.end() || device->second.kind != Kind::Device ||
                adapter == objects.end() || adapter->second.kind != Kind::Adapter ||
                allocation->second.parent != desc.device || device->second.parent != desc.adapter) return reply(h, -9);
            const auto result = driver.translateVendorAllocation(allocation->second.nativeHandle, device->second.nativeHandle, adapter->second.nativeHandle);
            if (result.nativeHandle || (result.ntstatus >= 0 && (result.ntstatus != 0 || !result.value || result.value > UINT32_MAX)) ||
                (result.ntstatus < 0 && result.value)) return reply(h, -5);
            if (result.ntstatus >= 0) {
                if (allocation->second.driverToken && allocation->second.driverToken != result.value) return reply(h, -5);
                for (const auto& item : objects)
                    if (item.first != h.handle && item.second.kind == Kind::VendorAllocation && item.second.driverToken == result.value)
                        return reply(h, -5);
                allocation->second.driverToken = static_cast<std::uint32_t>(result.value);
            }
            // This opaque KMD token is an explicit driver-private contract,
            // never a substitute for a typed ID in public bridge operations.
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::LockVendorAllocation || op == Op::UnlockVendorAllocation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorCpuCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(VendorCpuDesc)) return reply(h, -22);
            VendorCpuDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!desc.device || desc.flags) return reply(h, -22);
            const auto allocation = objects.find(h.handle), device = objects.find(desc.device);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                device == objects.end() || device->second.kind != Kind::Device || allocation->second.parent != desc.device)
                return reply(h, -9);
            auto& object = allocation->second;
            if (op == Op::UnlockVendorAllocation) {
                if (!object.cpuBytes) return reply(h, -9);
                const auto result = driver.unlockVendorAllocation(object.nativeHandle, device->second.nativeHandle);
                if (result.nativeHandle || result.value || (result.ntstatus >= 0 && result.ntstatus != 0)) return reply(h, -5);
                if (result.ntstatus >= 0) { vendorCpuBytes -= object.cpuBytes; object.cpuBytes = 0; object.cpuOffset = 0; }
                return reply(h, 0, h.handle, &result);
            }
            if (object.cpuBytes) return reply(h, -16);
            if (!object.gpuPages || object.gpuOffsetPages) return reply(h, -95);
            const auto bytes = object.gpuPages * 4096u;
            if (!vendorCpuBudgetFits(vendorCpuBytes, bytes, driver.capabilities().flags,openglDesktop4kCpuNegotiated,coordinated && coordinated->gl.version==14)) {
                std::cerr<<"{\"wireCpuByteAdmissionRejected\":true,\"usedBytes\":"<<vendorCpuBytes<<",\"requestedBytes\":"<<bytes
                    <<",\"limitBytes\":"<<vendorCpuByteLimit(driver.capabilities().flags,openglDesktop4kCpuNegotiated,coordinated && coordinated->gl.version==14)<<"}\n";
                return reply(h,-24);
            }
            const auto native = driver.lockVendorAllocation(object.nativeHandle, device->second.nativeHandle);
            if (native.lock.nativeHandle || (native.lock.ntstatus >= 0 && (native.lock.ntstatus != 0 ||
                !validVendorCpuReply(native.lock.value, native.output,openglDesktopSparseNegotiated,openglDesktopSlotsNegotiated) || native.output.bytes != bytes)) ||
                (native.lock.ntstatus < 0 && (native.lock.value || native.output.bytes || native.output.reserved || native.output.generation)))
                return reply(h, -5);
            if (native.lock.ntstatus >= 0) {
                for (const auto& item : objects) if (item.second.cpuBytes &&
                    native.lock.value < item.second.cpuOffset + item.second.cpuBytes && item.second.cpuOffset < native.lock.value + native.output.bytes)
                    return reply(h, -5);
                object.cpuBytes = native.output.bytes; object.cpuOffset = native.lock.value; vendorCpuBytes += object.cpuBytes;
            }
            auto out = reply(h, 0, h.handle, &native.lock);
            const auto start = out.size(); out.resize(start + sizeof(VendorCpuReply));
            std::memcpy(out.data() + start, &native.output, sizeof native.output); return out;
        }
        if (op == Op::MapGpuState) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & GpuStateCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(GpuVaDesc)) return reply(h, -22);
            GpuVaDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validGpuState(desc)) return reply(h, -22);
            const auto reservation = objects.find(h.handle), queue = objects.find(desc.queue);
            if (reservation == objects.end() || reservation->second.kind != Kind::GpuReservation ||
                queue == objects.end() || queue->second.kind != Kind::PagingQueue ||
                objects.at(queue->second.parent).parent != reservation->second.parent) return reply(h, -9);
            if (!gpuRangeContains(reservation->second.gpuAddress, reservation->second.reservedGpuBytes, desc.base, desc.sizePages * 4096)) return reply(h, -9);
            for (const auto& item : objects) {
                const auto& mapped = item.second;
                if (mapped.kind == Kind::Allocation && gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.size)) return reply(h, -16);
                if (mapped.kind == Kind::VendorAllocation && gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.gpuPages * 4096ull) &&
                    (mapped.parent != queue->second.parent || !gpuRangeContains(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.gpuPages * 4096ull))) return reply(h, -16);
            }
            GpuStateRanges::Plan plan;
            if (!gpuStates.prepare(h.handle, desc.base, desc.sizePages * 4096, desc.protection, plan)) return reply(h, -24);
            const auto result = driver.mapGpuState(reservation->second.nativeHandle, queue->second.nativeHandle, desc);
            if (result.map.nativeHandle || (result.map.ntstatus >= 0 &&
                ((result.map.ntstatus != 0 && result.map.ntstatus != 259) || result.map.value != desc.base)) ||
                (result.map.ntstatus < 0 && (result.map.value || result.fence))) return reply(h, -5);
            if (result.map.ntstatus >= 0) {
                for (auto& item : objects) {
                    auto& mapped = item.second;
                    if (mapped.kind == Kind::VendorAllocation && gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.gpuAddress, mapped.gpuPages * 4096ull)) {
                        vendorMappedPages -= mapped.gpuPages; mapped.gpuPages = 0; mapped.gpuAddress = 0; mapped.gpuOffsetPages = 0;
                    }
                }
                gpuStates.commit(plan);
            }
            auto out = reply(h, 0, h.handle, &result.map);
            const auto start = out.size(); out.resize(start + sizeof(GpuVaReply));
            const GpuVaReply fence{result.fence}; std::memcpy(out.data() + start, &fence, sizeof fence); return out;
        }
        if (op == Op::ReserveGpuAddress || op == Op::FreeGpuReservation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & GpuReservationCapability)) return reply(h, -95);
            const auto entry = objects.find(h.handle);
            if (entry == objects.end() || entry->second.kind != (op == Op::ReserveGpuAddress ? Kind::Adapter : Kind::GpuReservation))
                return reply(h, -9);
            if (op == Op::FreeGpuReservation) {
                if (packet.size() != sizeof h + sizeof(FreeGpuReservationDesc)) return reply(h, -22);
                FreeGpuReservationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
                if (desc.reserved) return reply(h, -22);
                if (desc.adapter != entry->second.parent) return reply(h, -9);
                for (const auto& item : objects) {
                    const auto& mapped = item.second;
                    const auto bytes = mapped.kind == Kind::Allocation ? mapped.size : mapped.gpuPages * 4096ull;
                    if ((mapped.kind == Kind::VendorAllocation || mapped.kind == Kind::Allocation) &&
                        gpuRangesOverlap(entry->second.gpuAddress, entry->second.reservedGpuBytes, mapped.gpuAddress, bytes) &&
                        !gpuRangeContains(entry->second.gpuAddress, entry->second.reservedGpuBytes, mapped.gpuAddress, bytes)) return reply(h, -16);
                }
                const auto result = driver.destroy(Kind::GpuReservation, entry->second.nativeHandle);
                if (result.ntstatus > 0 || result.nativeHandle || result.value) return reply(h, -5);
                if (result.ntstatus == 0) {
                    for (auto& item : objects) {
                        auto& mapped = item.second;
                        const auto bytes = mapped.kind == Kind::Allocation ? mapped.size : mapped.gpuPages * 4096ull;
                        if ((mapped.kind == Kind::VendorAllocation || mapped.kind == Kind::Allocation) &&
                            gpuRangesOverlap(entry->second.gpuAddress, entry->second.reservedGpuBytes, mapped.gpuAddress, bytes)) {
                            vendorMappedPages -= mapped.gpuPages; mapped.gpuPages = 0; mapped.gpuAddress = 0; mapped.gpuOffsetPages = 0;
                        }
                    }
                    gpuStates.release(h.handle); gpuReservedBytes -= entry->second.reservedGpuBytes; objects.erase(entry);
                }
                return reply(h, 0, h.handle, &result);
            }
            if (packet.size() != sizeof h + sizeof(GpuReservationDesc)) return reply(h, -22);
            GpuReservationDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validGpuReservation(desc)) return reply(h, -22);
            const auto count = std::count_if(objects.begin(), objects.end(), [](const auto& item) { return item.second.kind == Kind::GpuReservation; });
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX ||
                static_cast<std::size_t>(count) >= reservationLimit() ||
                desc.bytes > MaxGpuReservedBytes - gpuReservedBytes) return reply(h, -24);
            if (desc.base) for (const auto& item : objects)
                if (item.second.kind == Kind::GpuReservation && gpuRangesOverlap(desc.base, desc.bytes, item.second.gpuAddress, item.second.reservedGpuBytes))
                    return reply(h, -16);
            const auto result = driver.reserveGpuAddress(entry->second.nativeHandle, desc);
            if ((result.ntstatus >= 0 && (result.ntstatus != 0 || !result.nativeHandle || !validGpuReservationOutput(desc, result.value))) ||
                (result.ntstatus < 0 && (result.nativeHandle || result.value))) {
                if (result.nativeHandle) driver.destroy(Kind::GpuReservation, result.nativeHandle);
                return reply(h, -5);
            }
            if (result.ntstatus < 0) return reply(h, 0, 0, &result);
            for (const auto& item : objects)
                if (item.second.kind == Kind::GpuReservation && gpuRangesOverlap(result.value, desc.bytes, item.second.gpuAddress, item.second.reservedGpuBytes)) {
                    driver.destroy(Kind::GpuReservation, result.nativeHandle); return reply(h, -5);
                }
            const auto id = nextId++;
            try {
                auto out = reply(h, 0, id, &result);
                Object owned{Kind::GpuReservation, h.handle, result.nativeHandle, 0, 0, false};
                owned.gpuAddress = result.value; owned.reservedGpuBytes = desc.bytes;
                objects.emplace(id, owned); gpuReservedBytes += desc.bytes; return out;
            } catch (...) { driver.destroy(Kind::GpuReservation, result.nativeHandle); throw; }
        }
        if (op == Op::QueryDeviceExecutionState) {
            if (!negotiated) return reply(h,-71);
            if (packet.size()!=sizeof h) return reply(h,-22);
            const auto device=objects.find(h.handle);
            if (device==objects.end() || device->second.kind!=Kind::Device) return reply(h,-9);
            const auto result=driver.queryDeviceExecutionState(device->second.nativeHandle);
            if (result.nativeHandle || result.ntstatus>0 ||
                (result.ntstatus==0 ? result.value<1 || result.value>7 : result.value!=0)) return reply(h,-5);
            return reply(h,0,h.handle,&result);
        }
        if (op == Op::QueryMemoryBudget) {
            if (!negotiated) return reply(h,-71);
            if (!(driver.capabilities().flags & VendorMemoryManagementCapability)) return reply(h,-95);
            if (packet.size()!=sizeof h+sizeof(MemoryBudgetDesc)) return reply(h,-22);
            MemoryBudgetDesc desc{}; std::memcpy(&desc,packet.data()+sizeof h,sizeof desc);
            if (!validMemoryBudget(desc)) return reply(h,-22);
            const auto adapter=objects.find(h.handle);
            if (adapter==objects.end() || adapter->second.kind!=Kind::Adapter) return reply(h,-9);
            const auto result=driver.queryMemoryBudget(adapter->second.nativeHandle,desc);
            if (result.result.nativeHandle || result.result.value || result.result.ntstatus>0 ||
                (result.result.ntstatus<0 && (result.info.budget || result.info.usage || result.info.reservation || result.info.available)))
                return reply(h,-5);
            auto out=reply(h,0,h.handle,&result.result); const auto start=out.size(); out.resize(start+sizeof result.info);
            std::memcpy(out.data()+start,&result.info,sizeof result.info); return out;
        }
        if (op == Op::EvictVendorAllocations) {
            if (!negotiated) return reply(h,-71);
            if (!(driver.capabilities().flags & VendorMemoryManagementCapability)) return reply(h,-95);
            if (packet.size()<sizeof h+sizeof(EvictDesc)) return reply(h,-22);
            EvictDesc desc{}; std::memcpy(&desc,packet.data()+sizeof h,sizeof desc);
            if (!validEvict(desc) || packet.size()!=sizeof h+sizeof desc+desc.count*4) return reply(h,-22);
            const auto device=objects.find(h.handle);
            if (device==objects.end() || device->second.kind!=Kind::Device) return reply(h,-9);
            std::vector<std::uint32_t> ids(desc.count),native(desc.count);
            std::memcpy(ids.data(),packet.data()+sizeof h+sizeof desc,ids.size()*4);
            for (std::size_t n=0;n<ids.size();++n) {
                const auto entry=objects.find(ids[n]);
                if (entry==objects.end() || entry->second.kind!=Kind::VendorAllocation || entry->second.parent!=h.handle) return reply(h,-9);
                if (std::find(ids.begin(),ids.begin()+n,ids[n])!=ids.begin()+n) return reply(h,-22);
                if (entry->second.cpuBytes && entry->second.residentReferences<=1 &&
                    !driver.retainsMappedResidencyOnEvict()) return reply(h,-16);
                native[n]=entry->second.nativeHandle;
            }
            const auto eviction=driver.evictVendorAllocations(device->second.nativeHandle,desc,native);
            const auto& result=eviction.result;
            // At most one reference per allocation can be removed by this
            // call, including a partially failing list operation. Never
            // assume a failed operation left all references unchanged.
            if (eviction.attempted) for (const auto id:ids) {
                auto& entry=objects.at(id);
                if (entry.residentReferences) --entry.residentReferences;
                entry.resident=entry.residentReferences!=0;
            }
            if (result.nativeHandle || result.ntstatus>0 || (result.ntstatus<0 && result.value) || (!eviction.attempted && result.ntstatus>=0)) return reply(h,-5);
            return reply(h,0,h.handle,&result);
        }
        if (op == Op::MakeVendorResident) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorResidencyCapability)) return reply(h, -95);
            if (packet.size() < sizeof h + sizeof(ResidentDesc)) return reply(h, -22);
            ResidentDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validResident(desc) || packet.size() != sizeof h + sizeof desc + desc.count * 4 * (1 + desc.priorities))
                return reply(h, -22);
            const auto queue = objects.find(h.handle);
            if (queue == objects.end() || queue->second.kind != Kind::PagingQueue) return reply(h, -9);
            std::vector<std::uint32_t> ids(desc.count), native(desc.count), priorities(desc.priorities ? desc.count : 0);
            std::memcpy(ids.data(), packet.data() + sizeof h + sizeof desc, ids.size() * 4);
            if (!priorities.empty()) std::memcpy(priorities.data(), packet.data() + sizeof h + sizeof desc + ids.size() * 4, priorities.size() * 4);
            for (std::size_t n = 0; n < ids.size(); ++n) {
                const auto entry = objects.find(ids[n]);
                if (entry == objects.end() || entry->second.kind != Kind::VendorAllocation || entry->second.parent != queue->second.parent)
                    return reply(h, -9);
                if (std::find(ids.begin(), ids.begin() + n, ids[n]) != ids.begin() + n) return reply(h, -22);
                if (driver.retainsMappedResidencyOnEvict() ? entry->second.residentReferences>=MaxVendorResidencyAttempts :
                    entry->second.residencyAttempts>=MaxVendorResidencyAttempts) return reply(h, -24);
                native[n] = entry->second.nativeHandle;
            }
            // Conservatively bound all native attempts, including failure
            // that may have made only part of the list resident.
            // The retaining backend bounds real live references itself. Its
            // logical eviction/reuse transfers do not create native attempts.
            if(!driver.retainsMappedResidencyOnEvict()) for (const auto id : ids) ++objects.at(id).residencyAttempts;
            const auto result = driver.makeVendorResident(queue->second.nativeHandle, desc, native, priorities);
            if (result.residency.nativeHandle || result.output.reserved || result.output.count > desc.count ||
                (result.residency.ntstatus >= 0 && result.residency.ntstatus != 0 && result.residency.ntstatus != 0x103))
                return reply(h, -5);
            if (result.residency.ntstatus >= 0 && result.output.count == desc.count)
                for (const auto id : ids) { auto& entry=objects.at(id); entry.resident=true; ++entry.residentReferences; }
            auto out = reply(h, 0, h.handle, &result.residency);
            const auto start = out.size(); out.resize(start + sizeof(ResidentReply));
            std::memcpy(out.data() + start, &result.output, sizeof result.output);
            return out;
        }
        if (op == Op::MapVendorAllocation) {
            if (!negotiated) return reply(h, -71);
            if (!(driver.capabilities().flags & VendorGpuVaCapability)) return reply(h, -95);
            if (packet.size() != sizeof h + sizeof(GpuVaDesc)) return reply(h, -22);
            GpuVaDesc desc{}; std::memcpy(&desc, packet.data() + sizeof h, sizeof desc);
            if (!validGpuVa(desc) && desc.driverProtection != VendorImportedDriverProtection) return reply(h, -22);
            const auto allocation = objects.find(h.handle), queue = objects.find(desc.queue);
            if (allocation == objects.end() || allocation->second.kind != Kind::VendorAllocation ||
                queue == objects.end() || queue->second.kind != Kind::PagingQueue || allocation->second.parent != queue->second.parent)
                return reply(h, -9);
            auto& object = allocation->second;
            if (!validImportedGpuVa(desc, object.openedSharedResource)) return reply(h, -22);
            if (desc.driverProtection && !object.vendorResource) return reply(h, -22);
            if (object.gpuPages) return reply(h, -16);
            bool reservationBacked=false;std::uint32_t reservedPages=0;
            const bool managed=coordinated && coordinated->managedMappings && driver.allowsReservationBackedMappings();
            if(managed) {
                const auto adapter=objects.at(queue->second.parent).parent;
                reservationBacked=desc.base && std::any_of(objects.begin(),objects.end(),[&](const auto& item) {
                    return item.second.kind==Kind::GpuReservation && item.second.parent==adapter &&
                        gpuRangeContains(item.second.gpuAddress,item.second.reservedGpuBytes,desc.base,desc.sizePages*4096);
                });
                for(const auto& item:objects) if(item.second.reservationBackedMapping) reservedPages+=item.second.gpuPages;
            }
            const auto standaloneLimit=vendorGpuPageLimit(driver.capabilities().flags,openglDesktop4kNegotiated,openglDesktopLargeMapNegotiated);
            if(managed ? !driver_mapping::fits(vendorMappedPages,reservedPages,desc.sizePages,standaloneLimit,coordinated->gl.reservedBytes/4096,reservationBacked) :
                !vendorGpuBudgetFits(vendorMappedPages,desc.sizePages,driver.capabilities().flags,openglDesktop4kNegotiated,openglDesktopLargeMapNegotiated)) return reply(h,-24);
            if (desc.base) for (const auto& item : objects) {
                const auto& mapped = item.second;
                if (mapped.kind == Kind::VendorAllocation && mapped.gpuPages &&
                    desc.base < mapped.gpuAddress + mapped.gpuPages * 4096ull &&
                    mapped.gpuAddress < desc.base + desc.sizePages * 4096) return reply(h, -16);
            }
            GpuStateRanges::Plan plan;
            if (desc.base && !gpuStates.prepareRemove(desc.base, desc.sizePages * 4096, plan)) return reply(h, -24);
            const auto result = driver.mapVendorAllocation(object.nativeHandle, queue->second.nativeHandle, desc);
            if (result.map.nativeHandle || (result.map.ntstatus >= 0 &&
                (result.map.ntstatus != 0 && result.map.ntstatus != 0x103)) ||
                (result.map.ntstatus >= 0 && !validImportedGpuVaOutput(desc, result.map.value, object.openedSharedResource)) ||
                (result.map.ntstatus < 0 && (result.map.value || result.fence))) return reply(h, -5);
            if (result.map.ntstatus >= 0) {
                object.gpuPages = static_cast<std::uint32_t>(desc.sizePages); object.gpuAddress = result.map.value;
                object.reservationBackedMapping=reservationBacked;
                object.gpuOffsetPages = desc.offsetPages;
                vendorMappedPages += object.gpuPages;
                if (!desc.base && !gpuStates.prepareRemove(result.map.value, desc.sizePages * 4096, plan))
                    throw std::runtime_error("GPU state tracking quota after allocation mapping");
                gpuStates.commit(plan);
            }
            auto out = reply(h, 0, h.handle, &result.map);
            const auto start = out.size(); out.resize(start + sizeof(GpuVaReply));
            const GpuVaReply fence{result.fence}; std::memcpy(out.data() + start, &fence, sizeof fence);
            return out;
        }
        const bool memoryOp = op >= Op::CreateAllocation && op <= Op::CopySharedAllocation;
        if (!memoryOp && (op < Op::OpenAdapter || op > Op::DestroyPagingQueue)) return reply(h, -95);
        const bool scalar = op == Op::CreateAllocation || op == Op::MakeResident || op == Op::MapAllocation;
        const bool transfer = op == Op::WriteAllocation || op == Op::ReadAllocation;
        const bool shared = op == Op::CreateSharedAllocation;
        const bool copy = op == Op::CopySharedAllocation;
        if ((!scalar && !transfer && !shared && !copy && packet.size() != sizeof h) ||
            (scalar && packet.size() != sizeof h + 4) ||
            (shared && packet.size() != sizeof h + sizeof(GuestRange)) ||
            (copy && packet.size() != sizeof h + sizeof(CopyRange)) ||
            (transfer && packet.size() < sizeof h + sizeof(Range))) return reply(h, -22);
        if (!negotiated) return reply(h, -71);
        if (op == Op::OpenAdapter) {
            if (h.handle) return reply(h, -22);
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX) return reply(h, -24);
            return insert(h, Kind::Adapter, 0, driver.openAdapter());
        }
        const auto entry = objects.find(h.handle);
        if (entry == objects.end()) return reply(h, -9);
        const auto& object = entry->second;
        const bool adapterOp = op == Op::QueryDriverVersion || op == Op::CloseAdapter || op == Op::CreateDevice;
        const bool deviceOp = op == Op::DestroyDevice || op == Op::CreatePagingQueue || op == Op::CreateAllocation || shared;
        const auto required = adapterOp ? Kind::Adapter : deviceOp ? Kind::Device : memoryOp ? Kind::Allocation : Kind::PagingQueue;
        if (object.kind != required) return reply(h, -9);
        if (copy) {
            CopyRange range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
            const auto source = objects.find(range.source);
            if (source == objects.end() || source->second.kind != Kind::Allocation ||
                source->second.parent != object.parent || !source->second.shared || !object.shared) return reply(h, -9);
            const auto& from = source->second;
            if (range.source == h.handle || !range.size || range.size > object.size || range.size > from.size ||
                range.sourceOffset > from.size - range.size || range.destinationOffset > object.size - range.size ||
                object.guestOffset % 65536 || from.guestOffset % 65536 || object.size % 65536 || from.size % 65536)
                return reply(h, -22);
            const auto result = driver.copySharedAllocation(object.nativeHandle, from.nativeHandle, range);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateAllocation || shared) {
            std::uint32_t size{}; std::memcpy(&size, packet.data() + sizeof h, 4);
            GuestRange range{};
            if (shared) {
                std::memcpy(&range, packet.data() + sizeof h, sizeof range); size = range.size;
                if (range.reserved || range.offset % 4096 || range.offset > UINT64_MAX - size) return reply(h, -22);
                if (!size || size % 4096 || size > MaxAllocation) return reply(h, -22);
                for (const auto& live : objects)
                    if (live.second.shared && range.offset < live.second.guestOffset + live.second.size &&
                        live.second.guestOffset < range.offset + size) return reply(h, -16);
            }
            if (!size || size % 4096 || size > MaxAllocation) return reply(h, -22);
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX || size > MaxAllocatedBytes - allocatedBytes)
                return reply(h, -24);
            const auto result = shared ? driver.createSharedAllocation(object.nativeHandle, range)
                                       : driver.createAllocation(object.nativeHandle, size);
            return insert(h, Kind::Allocation, h.handle, result, size, range.offset, shared);
        }
        if (transfer) {
            Range range{}; std::memcpy(&range, packet.data() + sizeof h, sizeof range);
            const auto expected = sizeof h + sizeof range + (op == Op::WriteAllocation ? range.size : 0);
            if (!range.size || range.size > MaxChunk || packet.size() != expected ||
                range.size > object.size || range.offset > object.size - range.size) return reply(h, -22);
            std::vector<std::uint8_t> data;
            const auto result = op == Op::WriteAllocation
                ? driver.writeAllocation(object.nativeHandle, range, packet.data() + sizeof h + sizeof range)
                : driver.readAllocation(object.nativeHandle, range, data);
            auto out = reply(h, 0, h.handle, &result);
            if (result.ntstatus >= 0 && op == Op::ReadAllocation) {
                if (data.size() != range.size) return reply(h, -5);
                out.insert(out.end(), data.begin(), data.end());
            }
            return out;
        }
        if (op == Op::MakeResident || op == Op::MapAllocation) {
            std::uint32_t queueId{}; std::memcpy(&queueId, packet.data() + sizeof h, 4);
            const auto queue = objects.find(queueId);
            if (queue == objects.end() || queue->second.kind != Kind::PagingQueue || queue->second.parent != object.parent)
                return reply(h, -9);
            const auto result = op == Op::MakeResident ? driver.makeResident(object.nativeHandle, queue->second.nativeHandle)
                                                     : driver.mapAllocation(object.nativeHandle, queue->second.nativeHandle);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueryResidency) {
            const auto result = driver.queryResidency(object.nativeHandle); return reply(h, 0, h.handle, &result);
        }
        if (op == Op::QueryDriverVersion || op == Op::ReadPagingFence) {
            const auto result = op == Op::QueryDriverVersion ? driver.queryVersion(object.nativeHandle)
                                                            : driver.readPagingFence(object.nativeHandle);
            return reply(h, 0, h.handle, &result);
        }
        if (op == Op::CreateDevice || op == Op::CreatePagingQueue) {
            if (objects.size() >= objectLimit() || nextId == UINT32_MAX) return reply(h, -24);
            const auto result = op == Op::CreateDevice ? driver.createDevice(object.nativeHandle)
                                                       : driver.createPagingQueue(object.nativeHandle);
            return insert(h, op == Op::CreateDevice ? Kind::Device : Kind::PagingQueue, h.handle, result);
        }
        if (queries.count(h.handle)) return reply(h, -16);
        for (const auto& child : objects)
            if (child.second.parent == h.handle &&
                !(object.kind == Kind::PagingQueue && child.second.kind == Kind::PagingSync)) return reply(h, -16);
        const auto result = driver.destroy(object.kind, object.nativeHandle);
        if (result.ntstatus >= 0) {
            if (object.kind == Kind::PagingQueue) {
                for (auto child = objects.begin(); child != objects.end();) {
                    if (child->second.parent == h.handle && child->second.kind == Kind::PagingSync) child = objects.erase(child);
                    else ++child;
                }
            }
            allocatedBytes -= object.size; objects.erase(entry);
        }
        return reply(h, 0, h.handle, &result);
    }
};
inline std::vector<std::uint8_t> request(Op operation, std::uint32_t handle = 0) {
    const Header h{static_cast<std::uint32_t>(operation), handle, 0, 0};
    std::vector<std::uint8_t> out(sizeof h); std::memcpy(out.data(), &h, sizeof h); return out;
}
inline std::vector<std::uint8_t> hello(std::uint32_t version = Version) {
    auto out = request(Op::Hello); out.resize(sizeof(Header) + 4);
    std::memcpy(out.data() + sizeof(Header), &version, 4); return out;
}
template<class T> inline std::vector<std::uint8_t> request(Op operation, std::uint32_t handle, const T& body) {
    auto out = request(operation, handle); out.resize(sizeof(Header) + sizeof body);
    std::memcpy(out.data() + sizeof(Header), &body, sizeof body); return out;
}
} // namespace driver_bridge
