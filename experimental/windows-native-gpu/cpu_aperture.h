// SPDX-License-Identifier: MIT
#pragma once
#include "driver_wire.h"
#include <array>
#include <optional>
#include <stdexcept>

namespace driver_cpu {
// Lock2 owns the allocation backing store. The requested byte count comes
// from a successful native GPU mapping of this allocation at offset zero;
// VirtualQuery may coalesce several backing stores into one committed region.
// Prove containment of that bounded allocation view without exporting the
// rest of the region. Memory type/protection are checked by the Windows owner.
inline bool validNativeAllocationView(std::uint64_t source, std::uint32_t bytes,
                                      std::uint64_t regionBase, std::uint64_t regionBytes) {
    constexpr std::uint64_t limit = 1ull << 47;
    return source && source % 4096 == 0 && bytes && bytes % 4096 == 0 &&
        bytes <= driver_bridge::MaxVendorCpuMappingBytes && source <= limit - bytes &&
        regionBase && regionBase % 4096 == 0 && regionBase <= source && regionBase < limit &&
        regionBytes >= bytes && regionBytes <= limit - regionBase &&
        source - regionBase <= regionBytes - bytes;
}
struct Lease { std::uint32_t slot, bytes; std::uint64_t generation, offset; };
// One CPU view may span adjacent existing 1 MiB QEMU slots. Each native
// mapping/unmapping receives its own generation and acknowledgement.
class Aperture {
    std::array<std::optional<Lease>, driver_bridge::DesktopVendorCpuSlots> leases;
    std::size_t slots;
    std::uint64_t generation = 0;
    unsigned liveGroups = 0, liveChunks = 0, createdGroups = 0, removedGroups = 0;
    unsigned createdChunks = 0, removedChunks = 0;
    std::uint32_t liveBytes = 0;
    static std::size_t span(std::uint32_t bytes) {
        return (bytes + driver_bridge::VendorCpuSlotBytes - 1) / driver_bridge::VendorCpuSlotBytes;
    }
    std::size_t freeSpan(std::uint32_t bytes) const {
        if (!bytes || bytes % 4096 || bytes > driver_bridge::MaxVendorCpuMappingBytes) return slots;
        const auto count = span(bytes);
        for (std::size_t first = 0; first + count <= slots; ++first) {
            std::size_t n = 0; for (; n < count && !leases[first + n]; ++n) {}
            if (n == count) return first;
        }
        return slots;
    }
    bool owns(Lease lease) const {
        if (!bytesValid(lease.bytes) || lease.slot >= slots || lease.offset != lease.slot * driver_bridge::VendorCpuSlotBytes ||
            !lease.generation || lease.generation > UINT64_MAX - (span(lease.bytes) - 1) || span(lease.bytes) > slots - lease.slot) return false;
        for (std::size_t n = 0; n < span(lease.bytes); ++n) {
            const auto& entry = leases[lease.slot + n];
            if (!entry || entry->slot != lease.slot || entry->bytes != lease.bytes || entry->generation != lease.generation || entry->offset != lease.offset) return false;
        }
        return true;
    }
    static bool bytesValid(std::uint32_t bytes) { return bytes && !(bytes % 4096) && bytes <= driver_bridge::MaxVendorCpuMappingBytes; }
public:
    explicit Aperture(std::size_t count, bool desktop=false) : slots(count) {
        if (!driver_bridge::validVendorCpuSlots(count,true,desktop)) throw std::runtime_error("Invalid CPU aperture capacity");
    }
    bool canMap(std::uint32_t bytes) const { return freeSpan(bytes) != slots && generation <= UINT64_MAX - span(bytes); }
    template<class Map, class Stop> Lease map(std::uint32_t bytes, Map mapChunk, Stop stopOwnedVm) {
        const auto first = freeSpan(bytes);
        if (first == slots || generation > UINT64_MAX - span(bytes)) throw std::runtime_error("CPU aperture contiguous span unavailable");
        const auto count = span(bytes);
        const Lease lease{static_cast<std::uint32_t>(first), bytes, generation + 1, first * driver_bridge::VendorCpuSlotBytes};
        generation += count;
        // Reserve the entire span before a callback can map any native page.
        for (std::size_t n = 0; n < count; ++n) leases[first + n] = lease;
        try {
            for (std::size_t n = 0; n < count; ++n) {
                const auto offset = n * driver_bridge::VendorCpuSlotBytes;
                const auto chunk = static_cast<std::uint32_t>((std::min)(driver_bridge::VendorCpuSlotBytes, bytes - offset));
                mapChunk(static_cast<std::uint32_t>(first + n), offset, chunk, lease.generation + n); ++createdChunks;
            }
        } catch (...) {
            stopOwnedVm(); // Must return only after the VM is reaped.
            clearAfterVmExit(); throw;
        }
        ++liveGroups; liveChunks += static_cast<unsigned>(count); liveBytes += bytes; ++createdGroups;
        return lease;
    }
    template<class Unmap, class Stop> void release(Lease lease, Unmap unmapChunk, Stop stopOwnedVm) {
        if (!owns(lease)) throw std::runtime_error("CPU aperture lease ownership mismatch");
        const auto count = span(lease.bytes);
        try {
            for (std::size_t n = count; n > 0; --n) {
                unmapChunk(lease.slot + static_cast<std::uint32_t>(n - 1), lease.generation + n - 1); ++removedChunks;
            }
        } catch (...) { stopOwnedVm(); clearAfterVmExit(); throw; }
        for (std::size_t n = 0; n < count; ++n) leases[lease.slot + n].reset();
        --liveGroups; liveChunks -= static_cast<unsigned>(count); liveBytes -= lease.bytes; ++removedGroups;
    }
    void clearAfterVmExit() noexcept {
        for (auto& lease : leases) lease.reset();
        liveGroups = liveChunks = liveBytes = 0;
    }
    unsigned groups() const { return liveGroups; }
    unsigned chunks() const { return liveChunks; }
    std::size_t largestFreeRunSlots() const {
        std::size_t current=0,best=0;
        for(std::size_t n=0;n<slots;++n) {
            current=leases[n] ? 0 : current+1;
            best=(std::max)(best,current);
        }
        return best;
    }
    std::uint32_t bytes() const { return liveBytes; }
    unsigned maps() const { return createdGroups; }
    unsigned unmaps() const { return removedGroups; }
    unsigned chunkMaps() const { return createdChunks; }
    unsigned chunkUnmaps() const { return removedChunks; }
};
} // namespace driver_cpu
