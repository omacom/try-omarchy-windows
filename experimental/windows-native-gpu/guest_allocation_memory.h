// SPDX-License-Identifier: MIT
// Diagnostic process-local views of owned native NVIDIA CPU allocation locks.
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
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace guest_allocation {
class Memory {
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
    std::size_t slotCount = 0;
    bool sparseAperture = false;
    bool desktopSlots = false;
    struct Mapping { void* data; std::uint32_t bytes, references; std::uint64_t offset, generation; };
    std::map<std::uint32_t, Mapping> mappings;
    unsigned created = 0, unmapped = 0;
    std::uint64_t loads = 0;
    static constexpr std::uint32_t Identity = 0x11fc1234, Magic = 0x31484c41;
    std::uint32_t word(unsigned offset) const {
        if (kernel.enabled()) {
            const auto info = kernel.info();
            switch (offset) {
            case 0: return Identity;
            case 0x40: return Magic;
            case 0x44: return info.allocation_slots;
            case 0x48: return static_cast<std::uint32_t>(info.cpu_slot_bytes);
            case 0x4c: return static_cast<std::uint32_t>(info.allocation_mmio_reads);
            case 0x50: return static_cast<std::uint32_t>(info.allocation_mmio_writes);
            default: throw std::runtime_error("Unknown kernel allocation layout field");
            }
        }
        std::uint32_t value{};
        if (pread(config.fd, &value, sizeof value, offset) != sizeof value) throw std::runtime_error("Allocation PCI config unavailable");
        return value;
    }
    void discover() {
        if (kernel.enabled()) {
            slotCount = kernel.info().allocation_slots;
            if (!driver_bridge::validVendorCpuSlots(slotCount,sparseAperture,desktopSlots)) throw std::runtime_error("Unnegotiated allocation layout");
            guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuLayout=true slots=%zu stride=%llu kernel=true\n", slotCount,
                         static_cast<unsigned long long>(driver_bridge::VendorCpuSlotBytes));
            return;
        }
        const char* directory = "/sys/bus/pci/devices";
        auto devices = opendir(directory);
        if (!devices) throw std::runtime_error("Allocation PCI inventory unavailable");
        std::string found;
        unsigned count = 0;
        while (auto entry = readdir(devices)) {
            if (entry->d_name[0] == '.') continue;
            if (++count > 256) { closedir(devices); throw std::runtime_error("Allocation PCI inventory bound exceeded"); }
            const auto path = std::string(directory) + '/' + entry->d_name;
            File candidate; candidate.open(path + "/config", O_RDONLY | O_CLOEXEC);
            std::uint32_t identity{};
            if (candidate.fd >= 0 && pread(candidate.fd, &identity, sizeof identity, 0) == sizeof identity && identity == Identity) {
                if (!found.empty()) { closedir(devices); throw std::runtime_error("Multiple allocation hubs"); }
                found = path;
            }
        }
        closedir(devices);
        if (found.empty()) throw std::runtime_error("Allocation hub absent");
        config.open(found + "/config", O_RDONLY | O_CLOEXEC);
        slotCount = word(0x44);
        if (word(0) != Identity || word(0x40) != Magic || !driver_bridge::validVendorCpuSlots(slotCount,sparseAperture) || word(0x48) != driver_bridge::VendorCpuSlotBytes)
            throw std::runtime_error("Allocation hub layout mismatch");
        File enable; enable.open(found + "/enable", O_WRONLY | O_CLOEXEC);
        if (enable.fd < 0 || write(enable.fd, "1", 1) != 1) throw std::runtime_error("Cannot enable allocation hub");
        resource.open(found + "/resource0", O_RDWR | O_CLOEXEC | O_SYNC);
        if (resource.fd < 0) throw std::runtime_error("Allocation BAR unavailable");
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuLayout=true slots=%zu stride=%llu\n", slotCount,
                     static_cast<unsigned long long>(driver_bridge::VendorCpuSlotBytes));
    }
public:
    Memory() = default;
    void enableSparseAperture(bool desktop=false) {
        if (sparseAperture || slotCount || !mappings.empty()) throw std::runtime_error("Sparse aperture must precede CPU mappings");
        sparseAperture=true;
        desktopSlots=desktop;
    }
    std::size_t active() const { return mappings.size(); }
    Memory(const Memory&) = delete;
    ~Memory() { if (resource.fd >= 0) for (const auto& item : mappings) munmap(item.second.data, item.second.bytes); }
    bool contains(std::uint32_t allocation) const { return mappings.count(allocation) != 0; }
    void* retain(std::uint32_t allocation) {
        auto& entry = mappings.at(allocation);
        if (entry.references >= 64) throw std::runtime_error("Allocation lock reference bound exceeded");
        ++entry.references; return entry.data;
    }
    void* map(std::uint32_t allocation, std::uint64_t offset, driver_bridge::VendorCpuReply reply) {
        if (!allocation || !driver_bridge::validVendorCpuReply(offset, reply,sparseAperture,desktopSlots) || mappings.count(allocation))
            throw std::runtime_error("Invalid allocation CPU lease");
        for (const auto& item : mappings)
            if (offset < item.second.offset + item.second.bytes && item.second.offset < offset + reply.bytes)
                throw std::runtime_error("Overlapping allocation CPU views");
        if (!slotCount) discover();
        const auto apertureBytes = slotCount * driver_bridge::VendorCpuSlotBytes;
        if (offset >= apertureBytes || reply.bytes > apertureBytes - offset || mappings.size() >= slotCount)
            throw std::runtime_error("Allocation CPU lease exceeds configured aperture");
        const auto beforeRead = word(0x4c), beforeWrite = word(0x50);
        auto data = kernel.enabled() ? kernel.map(WDDM_APERTURE_ALLOCATION, offset, reply.bytes, reply.generation) :
            mmap(nullptr, reply.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, resource.fd, static_cast<off_t>(offset));
        if (data == MAP_FAILED) throw std::runtime_error("Allocation CPU view failed");
        const auto first = static_cast<volatile const std::uint64_t*>(data);
        const auto last = reinterpret_cast<volatile const std::uint64_t*>(static_cast<const char*>(data) + reply.bytes - 8);
        guest_direct_read::allocationEndsProbe([&](unsigned end) { return *(end ? last : first); });
        try {
            if (word(0x4c) != beforeRead || word(0x50) != beforeWrite) throw std::runtime_error("Allocation CPU reads were emulated");
            mappings.emplace(allocation, Mapping{data, reply.bytes, 1, offset, reply.generation});
        } catch (...) {
            if (kernel.enabled()) kernel.unmap(data, reply.bytes); else munmap(data, reply.bytes);
            throw;
        }
        loads += guest_direct_read::ProbeLoads; ++created;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuMapped=true direct=true loads=%u bytes=%u offset=%llu generation=%llu\n",
                     guest_direct_read::ProbeLoads, reply.bytes, static_cast<unsigned long long>(offset), static_cast<unsigned long long>(reply.generation));
        return data;
    }
    // False means another process-local reference still owns the same view.
    bool release(std::uint32_t allocation, bool force = false) {
        const auto entry = mappings.find(allocation);
        if (entry == mappings.end()) return true;
        if (!force && entry->second.references > 1) { --entry->second.references; return false; }
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto storeTest = std::getenv("WDDM_BRIDGE_CPU_STORE_TEST");
        if (storeTest && !std::strcmp(storeTest, "1")) {
            auto first = static_cast<volatile std::uint64_t*>(entry->second.data);
            auto last = reinterpret_cast<volatile std::uint64_t*>(static_cast<char*>(entry->second.data) + entry->second.bytes - 8);
            *first = driver_bridge::CpuStoreFirstMarker; *last = driver_bridge::CpuStoreLastMarker;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (*first != driver_bridge::CpuStoreFirstMarker || *last != driver_bridge::CpuStoreLastMarker)
                throw std::runtime_error("Allocation diagnostic stores did not read back");
            guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuStoreTest=true firstLastReadback=true bytes=%u\n", entry->second.bytes);
        }
        if (word(0x4c) || word(0x50)) throw std::runtime_error("Allocation CPU access used MMIO emulation");
        if (kernel.enabled()) kernel.unmap(entry->second.data, entry->second.bytes);
        else if (munmap(entry->second.data, entry->second.bytes)) throw std::runtime_error("Allocation CPU unmap failed");
        const auto bytes = entry->second.bytes;
        mappings.erase(entry); ++unmapped;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0 bytes=%u\n", bytes);
        return true;
    }
    unsigned total() const { return created; }
    unsigned released() const { return unmapped; }
    std::uint64_t directLoads() const { return loads; }
};
} // namespace guest_allocation
