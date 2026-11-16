// SPDX-License-Identifier: MIT
// Explicit, bounded Mesa lab contracts. Older session budgets stay unchanged.
#pragma once
#include <cstdint>
namespace driver_bridge {
struct OpenGlProfile {
    std::uint32_t version, frames, commandLimit, reservationLimit;
    std::uint64_t reservedBytes;
    // Version 1 reserves the final word as zero; later versions carry their cap.
    std::uint32_t requestLimit, objectLimit;
};
static_assert(sizeof(OpenGlProfile) == 32, "fixed OpenGL lab negotiation");
constexpr OpenGlProfile GuestOpenGlProfile{1, 2, 64, 16, 16ull * 1024 * 1024 * 1024, 10000, 0};
constexpr OpenGlProfile GuestOpenGlFenceProfile{2, 2, 64, 16, 16ull * 1024 * 1024 * 1024, 10000, 384};
// A compositor owns multiple Mesa screens (allocator and renderer). Its startup
// exceeds the two-frame probe's 96 sync objects. Version 3 is an explicit
// initialization profile, not a sustained desktop or performance guarantee.
constexpr OpenGlProfile GuestOpenGlCompositorProfile{3, 2, 256, 16, 16ull * 1024 * 1024 * 1024, 50000, 512};
// The real compositor's first draw creates additional small GPUVA reservations.
// Preserve version 3; rendering capacity is a distinct, exact negotiation.
constexpr OpenGlProfile GuestOpenGlCompositorRenderProfile{4, 2, 256, 32, 16ull * 1024 * 1024 * 1024, 50000, 768};
// Version 5 replaces lifetime command/request caps with bounded live work.
// Zero is an exact, negotiated rolling budget, never a legacy-profile wildcard.
// Memory-byte, pending-work and aperture limits remain independently enforced.
constexpr OpenGlProfile GuestOpenGlDesktopProfile{5, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
// A real 3840x2160 compositor crosses the v5 256 MiB GPU-VA alias-map
// budget while importing its output, then its reported-usage guard. Version 6
// bounds both GPU budgets at 512 MiB;
// CPU maps, live objects, pending work and all older profiles stay bounded.
constexpr OpenGlProfile GuestOpenGlDesktop4kProfile{6, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
constexpr std::uint32_t OpenGlDesktop4kGpuMappedBytes = 512u * 1024 * 1024;
constexpr std::uint64_t OpenGlDesktop4kReportedGpuBytes = 512ull * 1024 * 1024;
// Version 7 additionally requires an explicit kernel capacity receipt. A bare
// version number cannot opt into the larger CPU budget. Versions 1-6 are fixed.
constexpr OpenGlProfile GuestOpenGlDesktop4kCpuProfile{7, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
constexpr OpenGlProfile GuestOpenGlDesktopSparseProfile{8, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
constexpr OpenGlProfile GuestOpenGlDesktopLargeMapProfile{9, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
// Version 10 also carries the process-reported GPU guard, including host
// presentation allocations. Version 9's 512 MiB guard remains fixed.
constexpr OpenGlProfile GuestOpenGlDesktopPresentationProfile{10, 0, 0, 64, 16ull * 1024 * 1024 * 1024, 0, 1024};
// Version 11 adds a 512-page physical fence aperture and 128 per-actor GPUVA
// reservations. It requires the owned coordinator, whose v4 object agreement
// admits 1024 aggregate allocations. Byte budgets and all older profiles stay fixed.
constexpr OpenGlProfile GuestOpenGlDesktopFenceProfile{11, 0, 0, 128, 16ull * 1024 * 1024 * 1024, 0, 1024};
// Version 12 adds sparse virtual CPU slots, not mapped bytes. An actual kernel
// receipt and owned coordinator are required; version 11 stays at 512 slots.
constexpr OpenGlProfile GuestOpenGlDesktopSlotProfile{12, 0, 0, 128, 16ull * 1024 * 1024 * 1024, 0, 1024};
// Version 13 requires the v5 group object agreement: 64 native resource and
// sync references across four actors, with the per-actor 16-reference caps fixed.
constexpr OpenGlProfile GuestOpenGlDesktopReferenceProfile{13, 0, 0, 128, 16ull * 1024 * 1024 * 1024, 0, 1024};
// Version 14 explicitly enlarges aggregate CPU mappings for multiple clients.
// It requires the new kernel receipt and v9 object agreement; v1-13 stay fixed.
constexpr OpenGlProfile GuestOpenGlMultiappProfile{14, 0, 0, 128, 16ull * 1024 * 1024 * 1024, 0, 1024};
constexpr std::uint64_t OpenGlDesktopPresentationReportedBytes=1024ull*1024*1024;
constexpr std::uint32_t OpenGlDesktopLargeMapBytes=1024u*1024*1024;
constexpr std::uint32_t OpenGlDesktop4kCpuBytes = 192u * 1024 * 1024;
constexpr std::uint32_t OpenGlMultiappCpuBytes = 384u * 1024 * 1024;
struct OpenGlDesktop4kCapacity {
    std::uint32_t version, kernelVersion;
    std::uint64_t cpuByteLimit;
    std::uint32_t cpuSlots, fenceSlots;
    std::uint64_t reserved;
};
static_assert(sizeof(OpenGlDesktop4kCapacity)==32,"Fixed explicit kernel-capacity receipt");
constexpr OpenGlDesktop4kCapacity GuestOpenGlDesktop4kCapacity{7,1,OpenGlDesktop4kCpuBytes,256,256,0};
constexpr OpenGlDesktop4kCapacity GuestOpenGlDesktopSparseCapacity{8,1,OpenGlDesktop4kCpuBytes,512,256,0};
// Version 9 explicitly transmits the independent GPU-VA byte guard. Published
// 32-byte v7/v8 requests and their CPU/GPU budgets remain unchanged.
struct OpenGlDesktopLargeMapCapacity {
    OpenGlDesktop4kCapacity aperture;
    std::uint64_t gpuMappedByteLimit;
};
static_assert(sizeof(OpenGlDesktopLargeMapCapacity)==40,"Fixed explicit GPU-VA capacity receipt");
constexpr OpenGlDesktopLargeMapCapacity GuestOpenGlDesktopLargeMapCapacity{{9,1,OpenGlDesktop4kCpuBytes,512,256,0},OpenGlDesktopLargeMapBytes};
struct OpenGlDesktopPresentationCapacity {
    OpenGlDesktopLargeMapCapacity mapping;
    std::uint64_t reportedGpuByteLimit;
};
static_assert(sizeof(OpenGlDesktopPresentationCapacity)==48,"Fixed explicit presentation capacity receipt");
constexpr OpenGlDesktopPresentationCapacity GuestOpenGlDesktopPresentationCapacity{{{10,1,OpenGlDesktop4kCpuBytes,512,256,0},OpenGlDesktopLargeMapBytes},OpenGlDesktopPresentationReportedBytes};
constexpr OpenGlDesktopPresentationCapacity GuestOpenGlDesktopFenceCapacity{{{11,1,OpenGlDesktop4kCpuBytes,512,512,0},OpenGlDesktopLargeMapBytes},OpenGlDesktopPresentationReportedBytes};
constexpr OpenGlDesktopPresentationCapacity GuestOpenGlDesktopSlotCapacity{{{12,1,OpenGlDesktop4kCpuBytes,1024,512,0},OpenGlDesktopLargeMapBytes},OpenGlDesktopPresentationReportedBytes};
constexpr OpenGlDesktopPresentationCapacity GuestOpenGlDesktopReferenceCapacity{{{13,1,OpenGlDesktop4kCpuBytes,1024,512,0},OpenGlDesktopLargeMapBytes},OpenGlDesktopPresentationReportedBytes};
constexpr OpenGlDesktopPresentationCapacity GuestOpenGlMultiappCapacity{{{14,1,OpenGlMultiappCpuBytes,1024,512,0},OpenGlDesktopLargeMapBytes},OpenGlDesktopPresentationReportedBytes};
inline bool validOpenGlDesktopPresentationCapacity(OpenGlDesktopPresentationCapacity c) {
    auto map=c.mapping;
    if(map.aperture.version!=10 || c.reportedGpuByteLimit!=OpenGlDesktopPresentationReportedBytes) return false;
    map.aperture.version=9;
    const auto a=map.aperture;
    return a.kernelVersion==1 && a.cpuByteLimit==OpenGlDesktop4kCpuBytes && a.cpuSlots==512 &&
        a.fenceSlots==256 && !a.reserved && map.gpuMappedByteLimit==OpenGlDesktopLargeMapBytes;
}
inline bool reportedGpuBudgetFits(std::uint64_t usage,std::uint64_t additional,std::uint64_t limit) {
    return usage<=limit && additional<=limit-usage;
}
inline bool validOpenGlDesktopFenceCapacity(OpenGlDesktopPresentationCapacity c) {
    if(c.mapping.aperture.version!=11 || c.mapping.aperture.fenceSlots!=512) return false;
    c.mapping.aperture.version=10;c.mapping.aperture.fenceSlots=256;
    return validOpenGlDesktopPresentationCapacity(c);
}
inline bool validCoordinatedPresentationCapacity(OpenGlDesktopPresentationCapacity c) {
    if(c.mapping.aperture.version==14) {
        if(c.mapping.aperture.cpuByteLimit!=OpenGlMultiappCpuBytes) return false;
        c.mapping.aperture.version=13;c.mapping.aperture.cpuByteLimit=OpenGlDesktop4kCpuBytes;
    }
    if(c.mapping.aperture.version==13) c.mapping.aperture.version=12;
    if(c.mapping.aperture.version==12) {
        if(c.mapping.aperture.cpuSlots!=1024) return false;
        c.mapping.aperture.version=11;c.mapping.aperture.cpuSlots=512;
        return validOpenGlDesktopFenceCapacity(c);
    }
    return validOpenGlDesktopPresentationCapacity(c) || validOpenGlDesktopFenceCapacity(c);
}
inline bool validOpenGlDesktopSlotCapacity(OpenGlDesktopPresentationCapacity c) {
    return c.mapping.aperture.version==12 && validCoordinatedPresentationCapacity(c);
}
inline bool validOpenGlDesktopReferenceCapacity(OpenGlDesktopPresentationCapacity c) {
    return c.mapping.aperture.version==13 && validCoordinatedPresentationCapacity(c);
}
inline bool validOpenGlMultiappCapacity(OpenGlDesktopPresentationCapacity c) {
    return c.mapping.aperture.version==14 && validCoordinatedPresentationCapacity(c);
}
inline bool validOpenGlDesktopFenceProfile(OpenGlProfile p) {
    if(p.version!=11) return false;
    p.version=10;
    return p.frames==0 && p.commandLimit==0 && p.reservationLimit==128 &&
        p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validCoordinatedPresentationProfile(OpenGlProfile p) {
    if(p.version==12 || p.version==13 || p.version==14) p.version=11;
    return p.version==11 ? validOpenGlDesktopFenceProfile(p) : p.version==10 &&
        !p.frames && !p.commandLimit && p.reservationLimit==64 &&
        p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktopSlotProfile(OpenGlProfile p) {
    return p.version==12 && validCoordinatedPresentationProfile(p);
}
inline bool validOpenGlDesktopReferenceProfile(OpenGlProfile p) {
    return p.version==13 && validCoordinatedPresentationProfile(p);
}
inline bool validOpenGlMultiappProfile(OpenGlProfile p) {
    return p.version==14 && validCoordinatedPresentationProfile(p);
}
inline bool validOpenGlDesktopPresentationProfile(OpenGlProfile p) {
    return p.version==10 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
        p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktopLargeMapCapacity(OpenGlDesktopLargeMapCapacity c) {
    const auto a=c.aperture;
    return a.version==9 && a.kernelVersion==1 && a.cpuByteLimit==OpenGlDesktop4kCpuBytes &&
           a.cpuSlots==512 && a.fenceSlots==256 && !a.reserved && c.gpuMappedByteLimit==OpenGlDesktopLargeMapBytes;
}
inline bool validOpenGlDesktopSparseCapacity(OpenGlDesktop4kCapacity c) {
    return c.version==8 && c.kernelVersion==1 && c.cpuByteLimit==OpenGlDesktop4kCpuBytes &&
           c.cpuSlots==512 && c.fenceSlots==256 && !c.reserved;
}
inline bool validOpenGlDesktop4kCapacity(OpenGlDesktop4kCapacity c) {
    return c.version==7 && c.kernelVersion==1 && c.cpuByteLimit==OpenGlDesktop4kCpuBytes &&
           c.cpuSlots==256 && c.fenceSlots==256 && !c.reserved;
}
constexpr std::uint32_t OpenGlDesktopVendorAllocations = 512;
inline bool lifetimeBudgetAvailable(std::uint64_t used,std::uint32_t limit) {
    return used<UINT64_MAX && (!limit || used<limit);
}
constexpr std::uint32_t OpenGlCompositorSyncObjects = 192;
constexpr std::uint32_t OpenGlCompositorVendorAllocations = 256;
constexpr std::uint32_t OpenGlCompositorHwSignals = GuestOpenGlCompositorProfile.commandLimit;
constexpr std::uint32_t OpenGlFenceHwQueues = 16;
constexpr std::uint32_t OpenGlFenceVendorAllocations = 192;
// Lifetime signals must cover the profile's command budget. Pending native
// signals retain the independent MaxHwQueueSignals (16) concurrency bound.
constexpr std::uint32_t OpenGlFenceHwSignals = GuestOpenGlFenceProfile.commandLimit;
inline bool validOpenGlProfile(OpenGlProfile p) {
    return p.version == 1 && p.frames == 2 && p.commandLimit == 64 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 10000 && !p.objectLimit;
}
inline bool validOpenGlFenceProfile(OpenGlProfile p) {
    return p.version == 2 && p.frames == 2 && p.commandLimit == 64 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 10000 && p.objectLimit == 384;
}
inline bool validOpenGlCompositorProfile(OpenGlProfile p) {
    return p.version == 3 && p.frames == 2 && p.commandLimit == 256 && p.reservationLimit == 16 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 50000 && p.objectLimit == 512;
}
inline bool validOpenGlCompositorRenderProfile(OpenGlProfile p) {
    return p.version == 4 && p.frames == 2 && p.commandLimit == 256 && p.reservationLimit == 32 &&
           p.reservedBytes == 16ull * 1024 * 1024 * 1024 && p.requestLimit == 50000 && p.objectLimit == 768;
}
inline bool validOpenGlDesktopProfile(OpenGlProfile p) {
    return p.version==5 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktopLargeMapProfile(OpenGlProfile p) {
    return p.version==9 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktopSparseProfile(OpenGlProfile p) {
    return p.version==8 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktop4kCpuProfile(OpenGlProfile p) {
    return p.version==7 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
inline bool validOpenGlDesktop4kProfile(OpenGlProfile p) {
    return p.version==6 && !p.frames && !p.commandLimit && p.reservationLimit==64 &&
           p.reservedBytes==16ull*1024*1024*1024 && !p.requestLimit && p.objectLimit==1024;
}
}
