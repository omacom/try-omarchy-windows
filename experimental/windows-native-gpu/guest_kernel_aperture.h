// SPDX-License-Identifier: MIT
// Explicit guest-kernel path. Never falls back to raw sysfs BAR access.
#pragma once
#include "guest_bridge_log.h"
#include "guest-kernel/wddm_aperture.h"
#include "guest_broker_client.h"
#include "guest_async_cpu_wait.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

namespace guest_kernel {
class Aperture {
    int control = -1;
    struct View { std::uint64_t cookie; std::size_t bytes; guest_broker::Fd notification; };
    std::map<void*, View> views;
    static void failed(const char* operation) {
        throw std::runtime_error(std::string("Kernel aperture ") + operation + ": " + std::strerror(errno));
    }
    void open() {
        if (control >= 0) return;
        if (guest_broker::clientEnabled()) throw std::runtime_error("Broker client cannot open a kernel controller");
        control = static_cast<int>(syscall(SYS_openat, AT_FDCWD, "/dev/wddm-aperture", O_RDWR | O_CLOEXEC, 0));
        if (control < 0) failed("open");
    }
    void revoke(std::uint64_t cookie) {
        if (guest_broker::clientEnabled()) { guest_broker::client().release(cookie); return; }
        wddm_aperture_revoke arg{WDDM_APERTURE_VERSION, 0, cookie};
        if (syscall(SYS_ioctl, control, WDDM_APERTURE_REVOKE, &arg)) failed("revoke");
    }
public:
    Aperture() = default;
    Aperture(const Aperture&) = delete;
    ~Aperture() {
        for (const auto& entry : views) munmap(entry.first, entry.second.bytes);
        // Closing the controller revokes all of its remaining FDs and VMAs.
        if (control >= 0) syscall(SYS_close, control);
    }
    bool enabled() const {
        const auto value = std::getenv("WDDM_BRIDGE_KERNEL_APERTURE");
        if (!value || !*value) return false;
        if (std::strcmp(value, "1")) throw std::runtime_error("Invalid kernel aperture opt-in");
        return true;
    }
    wddm_aperture_caps capacity() {
        wddm_aperture_caps value{}; value.version=1;
        if (guest_broker::clientEnabled()) value=guest_broker::client().capacity();
        else {
            open();
            if (syscall(SYS_ioctl,control,WDDM_APERTURE_CAPS,&value)) failed("capacity");
        }
        if (!wddm_aperture_valid_caps(value)) throw std::runtime_error("Kernel aperture capacity mismatch");
        return value;
    }
    wddm_aperture_info info() {
        wddm_aperture_info value{}; value.version = WDDM_APERTURE_VERSION;
        if (guest_broker::clientEnabled()) value = guest_broker::client().info();
        else {
            open();
            if (syscall(SYS_ioctl, control, WDDM_APERTURE_INFO, &value)) failed("info");
        }
        // Reuse immutable negotiation receipts only within this validation.
        // Every info() still obtains fresh counters and authenticates its RPCs.
        wddm_aperture_caps caps{};
        driver_bridge::coordinator::Info agreement{};
        bool haveCaps=false,haveAgreement=false;
        const auto capacityReceipt=[&]() -> const wddm_aperture_caps& {
            if(!haveCaps) { caps=capacity();haveCaps=true; }
            return caps;
        };
        const auto coordinatedAgreement=[&]() -> const driver_bridge::coordinator::Info& {
            if(!haveAgreement) { agreement=guest_broker::client().coordinatedProfile();haveAgreement=true; }
            return agreement;
        };
        if (value.version != WDDM_APERTURE_VERSION ||
            !driver_bridge::validFenceSlots(value.fence_slots,true) ||
            value.allocation_slots < 16 || value.allocation_slots > 1024 ||
            (value.allocation_slots & (value.allocation_slots - 1)) ||
            value.cpu_slot_bytes != 1048576 || value.fence_bytes != value.fence_slots * 4096ull ||
            value.allocation_bytes != value.allocation_slots * value.cpu_slot_bytes ||
            value.active_cpu_bytes > WDDM_APERTURE_MULTIAPP_CPU_LIMIT ||
            (value.active_cpu_bytes>WDDM_APERTURE_CPU_LIMIT && value.active_cpu_bytes>capacityReceipt().cpu_byte_limit))
            throw std::runtime_error("Kernel aperture layout mismatch");
        if(value.active_leases>wddm_aperture_lease_limit(value.fence_slots)) {
            if(!guest_broker::clientEnabled() || !std::getenv("WDDM_BRIDGE_ACTOR_PROFILE") ||
               value.active_leases>coordinatedAgreement().objects.leaseLimit)
                throw std::runtime_error("Kernel coordinated lease agreement missing");
        }
        if(value.fence_slots==512) {
            const auto profile=std::getenv("WDDM_BRIDGE_ACTOR_PROFILE");
            if(!guest_broker::clientEnabled() || !profile || (std::strcmp(profile,"11") && std::strcmp(profile,"12") && std::strcmp(profile,"13") && std::strcmp(profile,"14")) ||
               coordinatedAgreement().capacity.mapping.aperture.fenceSlots!=512)
                throw std::runtime_error("Desktop fence aperture lacks coordinator agreement");
        }
        if(value.allocation_slots==1024) {
            const auto profile=std::getenv("WDDM_BRIDGE_ACTOR_PROFILE");
            if(!guest_broker::clientEnabled() || !profile || (std::strcmp(profile,"12") && std::strcmp(profile,"13") && std::strcmp(profile,"14")) || value.fence_slots!=512 ||
                coordinatedAgreement().capacity.mapping.aperture.cpuSlots!=1024)
                throw std::runtime_error("Desktop CPU slots lack coordinator agreement");
        }
        if (value.allocation_slots>=512 && ((value.fence_slots!=256 && value.fence_slots!=512) || (capacityReceipt().flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED) && capacityReceipt().flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED|WDDM_APERTURE_MULTIAPP_CPU_ENABLED))))
            throw std::runtime_error("Sparse kernel layout lacks exact capacity opt-in");
        if(capacityReceipt().flags&WDDM_APERTURE_MULTIAPP_CPU_ENABLED) {
            const auto profile=std::getenv("WDDM_BRIDGE_ACTOR_PROFILE");
            if(!guest_broker::clientEnabled() || !profile || std::strcmp(profile,"14") ||
               value.allocation_slots!=1024 || value.fence_slots!=512)
                throw std::runtime_error("Multi-application CPU mappings require profile 14");
            const auto group=coordinatedAgreement();
            if(!driver_bridge::coordinator::validInfo(group) ||
               group.gl.version!=14 || (group.objects.version!=9 && group.objects.version!=10) ||
               group.capacity.mapping.aperture.cpuByteLimit!=capacityReceipt().cpu_byte_limit)
                throw std::runtime_error("Multi-application CPU mappings lack exact coordinator agreement");
        }
        return value;
    }
    void* map(std::uint32_t kind, std::uint64_t offset, std::size_t bytes, std::uint64_t generation = 0) {
        wddm_aperture_create arg{};
        arg.version = WDDM_APERTURE_VERSION; arg.kind = kind;
        arg.offset = offset; arg.bytes = bytes; arg.host_generation = generation;
        arg.flags = kind == WDDM_APERTURE_ALLOCATION ? WDDM_APERTURE_WRITE : 0;
        // Only a receipt-authorized restricted view FD crosses the broker
        // boundary. The root controller and arbitrary aperture access do not.
        guest_broker::Message remote;
        if (guest_broker::clientEnabled()) {
            remote = guest_broker::client().acquire({kind,0,offset,bytes,generation});
            arg.cookie = guest_broker::decode<guest_broker::ViewReply>(remote.payload).cookie;
            arg.fd = remote.fds[0].get();
        } else {
            open();
            if (syscall(SYS_ioctl, control, WDDM_APERTURE_CREATE, &arg)) failed("create");
        }
        if (!arg.cookie || arg.fd < 0) throw std::runtime_error("Kernel aperture lease response mismatch");
        guest_broker::Fd notification;
        if (guest_async::Enabled && kind == WDDM_APERTURE_FENCE) {
            const int copy = static_cast<int>(syscall(SYS_fcntl, arg.fd, F_DUPFD_CLOEXEC, 3));
            if (copy < 0) {
                const int error = errno;
                if (!guest_broker::clientEnabled()) syscall(SYS_close, arg.fd);
                revoke(arg.cookie); errno = error; failed("retain fence capability");
            }
            notification = guest_broker::Fd(copy);
        }
        auto data = mmap(nullptr, bytes, PROT_READ | (arg.flags ? PROT_WRITE : 0), MAP_SHARED, arg.fd, 0);
        const auto saved = errno;
        if (!guest_broker::clientEnabled()) syscall(SYS_close, arg.fd);
        if (data == MAP_FAILED) { revoke(arg.cookie); errno = saved; failed("mmap"); }
        try {
            if (!views.emplace(data, View{arg.cookie, bytes, std::move(notification)}).second)
                throw std::runtime_error("Overlapping kernel aperture virtual mappings");
        } catch (...) { munmap(data, bytes); revoke(arg.cookie); throw; }
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE kernelApertureLease=true kind=%u bytes=%zu cookie=%llu\n", kind, bytes,
                     static_cast<unsigned long long>(arg.cookie));
        return data;
    }
    void unmap(void* data, std::size_t bytes) {
        const auto found = views.find(data);
        if (found == views.end() || found->second.bytes != bytes)
            throw std::runtime_error("Kernel aperture mapping absent");
        if (munmap(data, bytes)) failed("munmap");
        revoke(found->second.cookie);
        views.erase(found);
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE kernelApertureRevoked=true\n");
    }
    int syncFile(const void* word,std::uint64_t value,int nativeFd=-1) {
        const auto address=reinterpret_cast<std::uintptr_t>(word);
        if (!enabled() || !address || address%8 || (!value && nativeFd<0) || value==UINT64_MAX)
            throw std::system_error(EINVAL,std::generic_category(),"Invalid native sync-file target");
        for (const auto& entry:views) {
            const auto start=reinterpret_cast<std::uintptr_t>(entry.first);
            if (address<start || address-start>=entry.second.bytes || entry.second.bytes-(address-start)<8) continue;
            const guest_broker::SyncFileRequest request{entry.second.cookie,address-start,value};
            if (guest_broker::clientEnabled()) return nativeFd<0 ? guest_broker::client().syncFile(request).release() :
                guest_broker::client().nativeSyncFile(request,nativeFd).release();
            if (nativeFd>=0) {
                wddm_sync_file_native_create arg{}; arg.sync.version=WDDM_APERTURE_VERSION;
                arg.sync.cookie=request.cookie; arg.sync.offset=request.offset; arg.sync.value=value; arg.native_fd=nativeFd;
                if (syscall(SYS_ioctl,control,WDDM_SYNC_FILE_NATIVE_CREATE,&arg))
                    throw std::system_error(errno,std::generic_category(),"Native sync-file lineage export");
                return arg.sync.fd;
            }
            wddm_sync_file_create arg{}; arg.version=WDDM_APERTURE_VERSION;
            arg.cookie=request.cookie; arg.offset=request.offset; arg.value=value;
            if (syscall(SYS_ioctl,control,WDDM_SYNC_FILE_CREATE,&arg))
                throw std::system_error(errno,std::generic_category(),"Native sync-file export");
            return arg.fd;
        }
        throw std::system_error(EBADF,std::generic_category(),"Native sync-file word has no owned lease");
    }
    wddm_sync_file_stats syncFileInfo() {
        if (guest_broker::clientEnabled()) return guest_broker::client().syncFileInfo();
        open(); wddm_sync_file_stats value{}; value.version=WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl,control,WDDM_SYNC_FILE_STATS,&value))
            throw std::system_error(errno,std::generic_category(),"Native sync-file stats");
        return value;
    }
    wddm_fence_eventfd_point eventPoint(const void* word, std::uint64_t target) const {
        const auto address = reinterpret_cast<std::uintptr_t>(word);
        if (!guest_async::Enabled || !enabled())
            throw std::system_error(EOPNOTSUPP, std::generic_category());
        if (!address || address % 8 || target == UINT64_MAX)
            throw std::system_error(EINVAL, std::generic_category());
        for (const auto& entry : views) {
            const auto start = reinterpret_cast<std::uintptr_t>(entry.first);
            if (address < start || address - start >= entry.second.bytes ||
                entry.second.bytes - (address - start) < 8) continue;
            if (entry.second.notification.get() < 0)
                throw std::system_error(EBADF, std::generic_category(), "Not a retained fence capability");
            return {entry.second.notification.get(), 0, address - start, target};
        }
        throw std::system_error(EBADF, std::generic_category(), "Fence word has no owned lease");
    }
    bool eventStatsAvailable() const {
        for (const auto& entry : views) if (entry.second.notification.get() >= 0) return true;
        return false;
    }
    wddm_fence_eventfd_stats eventStats() const {
        for (const auto& entry : views) if (entry.second.notification.get() >= 0) {
            wddm_fence_eventfd_stats info{}; info.version = WDDM_APERTURE_VERSION;
            if (syscall(SYS_ioctl, entry.second.notification.get(), WDDM_FENCE_EVENTFD_STATS, &info))
                throw std::system_error(errno, std::generic_category(), "GPU eventfd statistics");
            if (info.version != WDDM_APERTURE_VERSION || info.reserved ||
                info.held > WDDM_FENCE_EVENTFD_MAX_WAITS || info.pending > WDDM_FENCE_EVENTFD_MAX_WAITS)
                throw std::system_error(EPROTO, std::generic_category());
            return info;
        }
        throw std::system_error(EBADF, std::generic_category(), "No fence capability for statistics");
    }
};
static_assert(sizeof(wddm_aperture_info) == 72 && sizeof(wddm_aperture_create) == 64 &&
              sizeof(wddm_aperture_revoke) == 16, "Fixed kernel aperture ABI");
} // namespace guest_kernel
