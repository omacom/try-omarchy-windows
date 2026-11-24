// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace driver_bridge {
// Shadow ownership for address-only Zero/NoAccess mappings. Native GPU page
// tables perform the actual access; this never intercepts GPU reads or writes.
class GpuStateRanges {
public:
    struct Range { std::uint32_t owner; std::uint64_t address, bytes, protection; };
    enum class RetirementScope { PagingFence, OwningDevice, AllDevices };
    using Plan = std::vector<Range>;
    static constexpr std::size_t Limit = 32;
    // Address-only state does not allocate video memory. Match the bounded
    // reservation budget rather than the separate physical mapping budget.
    static constexpr std::uint64_t ByteLimit = 16ull * 1024 * 1024 * 1024;
private:
    Plan ranges;
    static bool overlap(const Range& r, std::uint64_t address, std::uint64_t bytes) {
        return r.address < address + bytes && address < r.address + r.bytes;
    }
    static void removeFrom(const Range& r, std::uint64_t address, std::uint64_t bytes, Plan& next) {
        if (!overlap(r, address, bytes)) { next.push_back(r); return; }
        if (r.address < address) next.push_back({r.owner, r.address, address - r.address, r.protection});
        const auto end = address + bytes, oldEnd = r.address + r.bytes;
        if (oldEnd > end) next.push_back({r.owner, end, oldEnd - end, r.protection});
    }
    static bool bounded(Plan& next) {
        std::sort(next.begin(), next.end(), [](const Range& a, const Range& b) {
            return a.owner < b.owner || (a.owner == b.owner && a.address < b.address);
        });
        Plan merged; merged.reserve(next.size());
        std::uint64_t bytes = 0;
        for (const auto& r : next) {
            if (r.bytes > ByteLimit - bytes) return false;
            bytes += r.bytes;
            if (!merged.empty() && merged.back().owner == r.owner && merged.back().protection == r.protection &&
                merged.back().address + merged.back().bytes == r.address) merged.back().bytes += r.bytes;
            else merged.push_back(r);
        }
        if (merged.size() > Limit) return false;
        next.swap(merged); return true;
    }
public:
    // Callers validate 48-bit GPU addresses and reservation containment first.
    // Prepare allocates before the native operation; commit cannot throw.
    bool prepare(std::uint32_t owner, std::uint64_t address, std::uint64_t bytes, std::uint64_t protection, Plan& next) const {
        next.clear(); next.reserve(ranges.size() + 2);
        for (const auto& r : ranges) {
            if (r.owner != owner) {
                if (overlap(r, address, bytes)) return false;
                next.push_back(r);
            } else removeFrom(r, address, bytes, next);
        }
        next.push_back({owner, address, bytes, protection}); return bounded(next);
    }
    bool prepareRemove(std::uint64_t address, std::uint64_t bytes, Plan& next) const {
        next.clear(); next.reserve(ranges.size() + 1);
        for (const auto& r : ranges) removeFrom(r, address, bytes, next);
        return bounded(next);
    }
    void commit(Plan& next) noexcept { ranges.swap(next); }
    void release(std::uint32_t owner) noexcept {
        ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [owner](const Range& r) { return r.owner == owner; }), ranges.end());
    }
    std::uint64_t bytes() const noexcept {
        std::uint64_t total = 0; for (const auto& r : ranges) total += r.bytes; return total;
    }
    std::size_t size() const noexcept { return ranges.size(); }
    bool empty() const noexcept { return ranges.empty(); }
    // Fresh reserved pages and an identical Zero/NoAccess state do not
    // invalidate existing allocation data. Their native paging fence still
    // orders access. Changing old protection or replacing allocation mappings
    // requires the separate hardware-retirement barrier.
    bool requiresRetirement(std::uint32_t owner,std::uint64_t address,std::uint64_t bytes,
                            std::uint64_t protection,bool allocationOverlap) const noexcept {
        return allocationOverlap || std::any_of(ranges.begin(),ranges.end(),[&](const Range& range) {
            return overlap(range,address,bytes) && (range.owner!=owner || range.protection!=protection);
        });
    }
    RetirementScope retirementScope(std::uint32_t owner,std::uint64_t address,std::uint64_t bytes,
                                    std::uint64_t protection,bool allocationOverlap) const noexcept {
        // Physical overlaps have already been checked against the paging
        // device by the caller. Recipient imports use independently mapped
        // allocations/VAs. A protection change without that ownership proof
        // retains the conservative process-wide barrier.
        if(requiresRetirement(owner,address,bytes,protection,false)) return RetirementScope::AllDevices;
        return allocationOverlap ? RetirementScope::OwningDevice : RetirementScope::PagingFence;
    }
    static bool retirementCoversQueue(RetirementScope scope,std::uint32_t device,std::uint32_t queueDevice) noexcept {
        return scope!=RetirementScope::PagingFence && (scope!=RetirementScope::OwningDevice || device==queueDevice);
    }
};
} // namespace driver_bridge
