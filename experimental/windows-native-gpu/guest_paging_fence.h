// SPDX-License-Identifier: MIT
// Process-local diagnostic mapping of the private QEMU read-only PCI hub.
#pragma once
#include "guest_bridge_log.h"
#include "driver_wire.h"
#include "guest_kernel_aperture.h"
#include "guest_direct_read_probe.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdio>
#include <map>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>

namespace guest_paging {
class Fences {
    struct File {
        int fd = -1;
        File() = default;
        File(const File&) = delete;
        ~File() { if (fd >= 0) syscall(SYS_close, fd); }
        void open(const std::string& path, int flags) {
            if (fd >= 0) syscall(SYS_close, fd);
            fd = static_cast<int>(syscall(SYS_openat, AT_FDCWD, path.c_str(), flags, 0));
        }
    };
    File config, resource;
    mutable guest_kernel::Aperture kernel;
    std::uint32_t slotCount = 0;
    struct Mapping { void* page; volatile std::uint64_t* fence; };
    std::map<std::uint32_t, Mapping> mappings;
    std::uint64_t reads = 0;
    unsigned created = 0;
    static constexpr std::uint32_t Identity = 0x11fd1234, Magic = 0x31484e46;
    std::uint32_t word(unsigned offset) const {
        if (kernel.enabled()) {
            const auto info = kernel.info();
            switch (offset) {
            case 0: return Identity;
            case 0x40: return Magic;
            case 0x44: return info.fence_slots;
            case 0x48: return static_cast<std::uint32_t>(info.fence_mmio_reads);
            default: throw std::runtime_error("Unknown kernel fence layout field");
            }
        }
        std::uint32_t value{};
        if (pread(config.fd, &value, sizeof value, offset) != sizeof value) throw std::runtime_error("Fence PCI config read failed");
        return value;
    }
    void discover() {
        if (kernel.enabled()) {
            slotCount = kernel.info().fence_slots;
            guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE fenceHubLayout=true slots=%u stride=4096 readonly=true kernel=true\n", slotCount);
            return;
        }
        const char* directory = "/sys/bus/pci/devices";
        auto devices = opendir(directory);
        if (!devices) throw std::runtime_error("Fence PCI inventory unavailable");
        std::string found;
        unsigned count = 0;
        while (auto entry = readdir(devices)) {
            if (entry->d_name[0] == '.') continue;
            if (++count > 256) { closedir(devices); throw std::runtime_error("Fence PCI inventory bound exceeded"); }
            const auto path = std::string(directory) + '/' + entry->d_name;
            File candidate; candidate.open(path + "/config", O_RDONLY | O_CLOEXEC);
            std::uint32_t identity{};
            if (candidate.fd >= 0 && pread(candidate.fd, &identity, sizeof identity, 0) == sizeof identity && identity == Identity) {
                if (!found.empty()) { closedir(devices); throw std::runtime_error("Multiple private fence hubs"); }
                found = path;
            }
        }
        closedir(devices);
        if (found.empty()) throw std::runtime_error("Private fence hub absent");
        config.open(found + "/config", O_RDONLY | O_CLOEXEC);
        slotCount = word(0x44);
        if (word(0) != Identity || word(0x40) != Magic || !driver_bridge::validFenceSlots(slotCount)) throw std::runtime_error("Fence hub layout mismatch");
        File enable; enable.open(found + "/enable", O_WRONLY | O_CLOEXEC);
        if (enable.fd < 0 || write(enable.fd, "1", 1) != 1) throw std::runtime_error("Cannot enable private fence hub");
        resource.open(found + "/resource0", O_RDONLY | O_CLOEXEC | O_SYNC);
        if (resource.fd < 0) throw std::runtime_error("Fence BAR unavailable");
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE fenceHubLayout=true slots=%u stride=4096 readonly=true\n", slotCount);
    }
public:
    Fences() = default;
    std::size_t active() const { return mappings.size(); }
    Fences(const Fences&) = delete;
    ~Fences() { if (resource.fd >= 0) for (const auto& mapping : mappings) munmap(mapping.second.page, 4096); }
    void* map(std::uint32_t queue, std::uint64_t offset, const char* category = "paging") {
        if (!slotCount) discover();
        if (!queue || offset % 8 || offset >= (slotCount==512 && kernel.enabled() ? driver_bridge::DesktopFenceApertureBytes : driver_bridge::FenceApertureBytes) || mappings.count(queue))
            throw std::runtime_error("Invalid guest fence identity or offset");
        if (offset >= slotCount * 4096ull || mappings.size() >= slotCount) throw std::runtime_error("Fence mapping exceeds configured aperture");
        const auto before = word(0x48);
        auto page = kernel.enabled() ? kernel.map(WDDM_APERTURE_FENCE, offset & ~4095ull, 4096) :
            mmap(nullptr, 4096, PROT_READ, MAP_SHARED, resource.fd, static_cast<off_t>(offset & ~4095ull));
        if (page == MAP_FAILED) throw std::runtime_error("Guest fence mapping failed");
        auto fence = reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(page) + (offset & 4095));
        // This is a diagnostic, not a polling API. Prove these reads bypass
        // the QEMU MMIO emulation before exposing the CPU fence to the UMD.
        const auto observed = guest_direct_read::fenceProbe([&]() { return *fence; });
        try {
            if (word(0x48) != before) throw std::runtime_error("Guest fence reads were emulated");
            mappings.emplace(queue, Mapping{page, fence});
        } catch (...) {
            if (kernel.enabled()) kernel.unmap(page, 4096); else munmap(page, 4096);
            throw;
        }
        reads += guest_direct_read::ProbeLoads; ++created;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE %sFenceMapped=true direct=true loads=%u offset=%llu value=%llu\n",
                     category, guest_direct_read::ProbeLoads, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(observed));
        return const_cast<std::uint64_t*>(fence);
    }
    void verifyRetired(std::uint32_t queue, std::uint64_t target, const char* operation) {
        const auto found = mappings.find(queue);
        if (found == mappings.end()) throw std::runtime_error("Mapped guest paging fence absent");
        const auto before = word(0x48);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto observed = guest_direct_read::fenceProbe([&]() { return *found->second.fence; });
        if (!guest_direct_read::retired(observed,target) || word(0x48) != before) throw std::runtime_error("Guest fence did not directly observe paging retirement");
        reads += guest_direct_read::ProbeLoads;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE pagingFenceRetired=true direct=true loads=%u target=%llu observed=%llu operation=%s\n",
                     guest_direct_read::ProbeLoads, static_cast<unsigned long long>(target), static_cast<unsigned long long>(observed), operation);
    }
    bool waitRetired(std::uint32_t object, std::uint64_t target, const char* owner) {
        const auto found = mappings.find(object);
        if (found == mappings.end() || target == UINT64_MAX) throw std::runtime_error("Invalid mapped CPU wait fence");
        const auto before = word(0x48);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint64_t observed = 0;
        do {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            observed = *found->second.fence; ++reads;
            if (observed == UINT64_MAX) throw std::runtime_error("Mapped CPU wait observed device loss");
            if (observed >= target) break;
            usleep(100);
        } while (std::chrono::steady_clock::now() < deadline);
        if (word(0x48) != before) throw std::runtime_error("CPU wait used emulated fence reads");
        if (observed < target) return false;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE cpuFenceWaitRetired=true direct=true target=%llu observed=%llu owner=%s\n",
                     static_cast<unsigned long long>(target), static_cast<unsigned long long>(observed), owner);
        return true;
    }
    void unmap(std::uint32_t queue) {
        const auto found = mappings.find(queue);
        if (found == mappings.end()) throw std::runtime_error("Guest paging queue absent");
        if (kernel.enabled()) kernel.unmap(found->second.page, 4096);
        else if (munmap(found->second.page, 4096)) throw std::runtime_error("Guest fence unmap failed");
        mappings.erase(found);
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE guestFenceUnmapped=true\n");
    }
    unsigned total() const { return created; }
    int syncFile(const void* word,std::uint64_t value,int nativeFd=-1) {
        if (!kernel.enabled()) throw std::system_error(EOPNOTSUPP,std::generic_category(),"Sync-file requires kernel leases");
        for (const auto& entry:mappings) if (entry.second.fence==word) return kernel.syncFile(word,value,nativeFd);
        throw std::system_error(EBADF,std::generic_category(),"Sync-file requires the exact owned fence pointer");
    }
    wddm_sync_file_stats syncFileInfo() { return kernel.syncFileInfo(); }
    const void* mappedWord(std::uint32_t object) const {
        const auto found=mappings.find(object);
        if(found==mappings.end()) throw std::system_error(EBADF,std::generic_category(),"Owned native fence has no CPU lease");
        return const_cast<const std::uint64_t*>(found->second.fence);
    }
    bool contains(std::uint32_t queue) const { return mappings.count(queue) != 0; }
    wddm_fence_eventfd_point eventPoint(std::uint32_t object, std::uint64_t target) const {
        return kernel.eventPoint(mappedWord(object), target);
    }
    bool eventStatsAvailable() const { return kernel.eventStatsAvailable(); }
    wddm_fence_eventfd_stats eventStats() const { return kernel.eventStats(); }
    std::uint64_t directLoads() const { return reads; }
};
} // namespace guest_paging
