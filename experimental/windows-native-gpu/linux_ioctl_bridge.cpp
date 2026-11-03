// SPDX-License-Identifier: MIT
// Local runtime diagnostic, not a kernel driver or a usable graphics device.
// Interposes /dev/dxg only in the explicitly preloaded process. Unsupported
// calls fail here; they never fall through to the real WSL GPU device.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include "adapter_query_client.h"
#include "guest_paging_fence.h"
#include "guest_allocation_memory.h"
#include "guest_kernel_object.h"
#include "native_resource_preparation.h"
#include "native_buffer_preparation.h"
#include "native_buffer_admission_reply.h"
#include "guest_eventfd_handle.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include "guest_bridge_log.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

extern char** environ;
namespace {
using namespace driver_bridge;
int rawClose(int fd) { return static_cast<int>(syscall(SYS_close, fd)); }
int rawOpen(const char* path, int flags) {
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, 0));
}
int ntErrno(std::int32_t status) {
    switch (static_cast<std::uint32_t>(status)) {
        case 0xc0000008: return EBADF;
        case 0xc000000d: return EINVAL;
        case 0xc0000017: return ENOMEM;
        case 0xc0000024: return EPROTOTYPE; // libdxg preserves STATUS_OBJECT_TYPE_MISMATCH
        case 0xc00002b6: return ENODEV; // STATUS_DEVICE_REMOVED
        case 0xc0000034: return ENOENT;
        case 0xc00000bb: return ENOSYS;
        case 0xc00000b5: return EAGAIN;
        case 0xc000022d: return EAGAIN;
        default: return EIO;
    }
}
struct Error : std::runtime_error {
    int number;
    explicit Error(int error) : std::runtime_error("Linux driver bridge failed"), number(error) {}
};
void checkNt(std::int32_t status) { if (status < 0) throw Error(ntErrno(status)); }
int findNvidiaModule(struct dl_phdr_info* info, std::size_t bytes, void* data) {
    (void)bytes;
    const auto name = std::getenv("WDDM_BRIDGE_LINUX_UMD_NAME");
    const auto path = info->dlpi_name;
    if (name && path) {
        const auto slash = std::strrchr(path, '/');
        if (!std::strcmp(slash ? slash + 1 : path, name)) *static_cast<bool*>(data) = true;
    }
    return 0;
}
// Do not change the application's SIGPIPE disposition. Block the signal for
// this thread's write and consume only a signal that was not already pending.
struct PipeSignal {
    sigset_t previous{}, blocked{}; bool pending = false, changed = false;
    PipeSignal() {
        sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
        sigset_t before{}; sigpending(&before); pending = sigismember(&before, SIGPIPE) == 1;
        const auto result = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
        if (result) throw Error(result);
        changed = true;
    }
    ~PipeSignal() {
        if (!changed) return;
        if (!pending) { timespec immediate{}; sigtimedwait(&blocked, nullptr, &immediate); }
        pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    }
};
class Transport {
    int input = -1, output = -1;
    pid_t child = -1;
    bool started = false, broken = false;
    bool broker = false;
    void transfer(void* data, std::size_t size, bool reading,
                  std::chrono::steady_clock::time_point deadline) {
        auto bytes = static_cast<std::uint8_t*>(data);
        while (size) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) throw Error(ETIMEDOUT);
            pollfd ready{reading ? input : output, static_cast<short>(reading ? POLLIN : POLLOUT), 0};
            const auto result = poll(&ready, 1, static_cast<int>(remaining));
            if (result < 0 && errno == EINTR) continue;
            if (result == 0) throw Error(ETIMEDOUT);
            if (result < 0 || (ready.revents & (POLLERR | POLLNVAL))) throw Error(EIO);
            const auto count = reading ? read(input, bytes, size) : write(output, bytes, size);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw Error(EIO);
            bytes += count; size -= static_cast<std::size_t>(count);
        }
    }
    void disconnect() {
        if (input >= 0) rawClose(input);
        if (output >= 0 && output != input) rawClose(output);
        input = output = -1;
    }
public:
    ~Transport() { finish(); }
    void finish() {
        if (broker) guest_broker::client().disconnect();
        disconnect();
        if (child <= 0) return;
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child) {
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE workerReaped=true exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                child = -1;
                return;
            }
            if (result < 0 && errno != EINTR) {
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE workerReaped=false errno=%d\n", errno); return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // WSL interop owns the Windows process. Never claim that
                // killing the Linux relay necessarily kills that process.
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE workerReaped=false timeout=true\n"); return;
            }
            timespec pause{0, 10000000}; nanosleep(&pause, nullptr);
        }
    }
    void start() {
        if (started) { if (broken) throw Error(EIO); return; }
        started = true;
        try {
            const auto worker = std::getenv("WDDM_BRIDGE_WINDOWS_WORKER");
            const auto port = std::getenv("WDDM_BRIDGE_PORT");
            if (guest_broker::clientEnabled()) {
                const auto kernel = std::getenv("WDDM_BRIDGE_KERNEL_APERTURE");
                if ((worker && *worker) || (port && *port) || !kernel || std::strcmp(kernel,"1")) throw Error(EINVAL);
                broker = true;
                (void)guest_broker::client().info();
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE transport=authenticated-broker nativeWorkerClients=1\n"); return;
            }
            if ((!worker || !*worker) == (!port || !*port)) throw Error(EINVAL);
            if (port && *port) {
                input = output = rawOpen(port, O_RDWR | O_CLOEXEC | O_NONBLOCK);
                if (input < 0) throw Error(errno);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE transport=virtio-port\n"); return;
            }
            if (worker[0] != '/') throw Error(EINVAL);
            int toChild[2]{-1, -1}, fromChild[2]{-1, -1};
            if (pipe2(toChild, O_CLOEXEC | O_NONBLOCK)) throw Error(errno);
            if (pipe2(fromChild, O_CLOEXEC | O_NONBLOCK)) {
                const auto error = errno; rawClose(toChild[0]); rawClose(toChild[1]); throw Error(error);
            }
            // Only the parent ends are nonblocking. The native worker uses
            // synchronous binary stdio; the spawn actions close unused ends.
            int descriptorError = 0;
            for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]})
                if (fd < 3) descriptorError = EBADF;
            for (const auto fd : {toChild[0], fromChild[1]}) {
                const auto flags = fcntl(fd, F_GETFL);
                if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK)) descriptorError = errno;
            }
            if (descriptorError) {
                for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) rawClose(fd);
                throw Error(descriptorError);
            }
            posix_spawn_file_actions_t actions{};
            int error = posix_spawn_file_actions_init(&actions);
            if (error) {
                for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]}) rawClose(fd);
                throw Error(error);
            }
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, toChild[0], STDIN_FILENO);
            if (!error) error = posix_spawn_file_actions_adddup2(&actions, fromChild[1], STDOUT_FILENO);
            for (const auto fd : {toChild[0], toChild[1], fromChild[0], fromChild[1]})
                if (!error) error = posix_spawn_file_actions_addclose(&actions, fd);
            std::vector<char*> environment;
            for (auto item = environ; *item; ++item)
                if (std::strncmp(*item, "LD_PRELOAD=", 11) && std::strncmp(*item, "WDDM_BRIDGE_", 12)) environment.push_back(*item);
            environment.push_back(nullptr);
            char stdio[] = "--stdio", queries[] = "--driver-queries", contexts[] = "--driver-contexts";
            char* arguments[]{const_cast<char*>(worker), stdio, queries, contexts, nullptr};
            if (!error) error = posix_spawn(&child, worker, &actions, nullptr, arguments, environment.data());
            posix_spawn_file_actions_destroy(&actions);
            rawClose(toChild[0]); rawClose(fromChild[1]);
            if (error) { rawClose(toChild[1]); rawClose(fromChild[0]); throw Error(error); }
            output = toChild[1]; input = fromChild[0];
            guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE transport=owned-worker-stdio\n");
        } catch (...) { broken = true; disconnect(); throw; }
    }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        start();
        if (packet.size() < sizeof(Header) || packet.size() > native_gpu::MaxPacket) throw Error(EINVAL);
        try {
            if (broker) return guest_broker::client().driver(packet);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            PipeSignal signal;
            auto size = static_cast<std::uint32_t>(packet.size());
            transfer(&size, sizeof size, false, deadline);
            transfer(const_cast<std::uint8_t*>(packet.data()), packet.size(), false, deadline);
            transfer(&size, sizeof size, true, deadline);
            if (size < sizeof(Header) || size > native_gpu::MaxPacket) throw Error(EPROTO);
            std::vector<std::uint8_t> result(size); transfer(result.data(), size, true, deadline);
            return result;
        } catch (...) { broken = true; if (broker) guest_broker::client().disconnect(); disconnect(); throw; }
    }
    void fail() { broken = true; if (broker) guest_broker::client().disconnect(); disconnect(); }
};
struct Response { Header header; Reply result; std::vector<std::uint8_t> data; };
std::atomic<bool> bridgeStopped{false};
class Bridge {
    Transport transport;
    guest_paging::Fences pagingFences;
    guest_paging::Fences hwQueueFences;
    guest_paging::Fences syncFences;
    guest_allocation::Memory allocationMemory;
    bool sparseApertureNegotiated = false;
    bool desktopSlotsNegotiated = false;
    guest_kernel::Objects kernelObjects;
    bool sharedFencesNegotiated = false;
    std::set<std::uint32_t> exportedSyncTickets, importedShareableSyncs;
    std::uint64_t queuedCompositorFrames = 0;
    bool compositorQueueUsed = false;
    bool compositorQueueEnded = false;
    std::map<std::uint32_t,std::uint32_t> exportedSyncSources;
    bool sharedResourcesNegotiated = false;
    std::uint32_t resourceReferenceLimit=MaxSharedResourceHandles;
    ResourceSharingProfile coordinatedResourceProfile{};
    std::uint64_t coordinatedReportedGpuByteLimit=OpenGlDesktopPresentationReportedBytes;
    bool coordinatedApplicationMemory=false;
    std::set<std::uint32_t> exportedResourceTickets;
    std::set<std::uint32_t> cpuLocks;
    bool negotiated = false;
    bool streamingNegotiated = false;
    bool openglNegotiated = false;
    bool coordinatedProfileNegotiated = false;
    std::uint32_t coordinatedProfileVersion=0;
    std::uint64_t fenceApertureBytes=FenceApertureBytes;
    bool openglFencesNegotiated = false;
    bool openglCompositorNegotiated = false;
    bool openglRenderNegotiated = false;
    bool openglDesktopNegotiated = false;
    std::size_t reservationLimit() const { return coordinatedProfileVersion>=11 && openglDesktopNegotiated ? GuestOpenGlDesktopFenceProfile.reservationLimit : openglDesktopNegotiated ? GuestOpenGlDesktopProfile.reservationLimit : openglRenderNegotiated ? GuestOpenGlCompositorRenderProfile.reservationLimit : openglNegotiated ? GuestOpenGlProfile.reservationLimit : MaxGpuReservations; }
    std::size_t syncObjectLimit() const { return openglCompositorNegotiated ? OpenGlCompositorSyncObjects : MaxSyncObjects; }
    std::uint64_t consumedStreamFrames = 0;
    native_gpu::Capabilities caps{};
    unsigned counts[256]{}, completedQueries = 0, privateQueries = 0, contexts = 0;
    std::map<int, std::pair<dev_t, ino_t>> descriptors;
    std::map<std::uint32_t, std::uint32_t> vendorOwners;
    struct VendorGpuRange { std::uint64_t address, bytes; };
    std::map<std::uint32_t, VendorGpuRange> vendorGpuRanges;
    std::map<std::uint32_t, std::uint32_t> vendorWireIds, deviceAdapters;
    std::set<std::uint32_t> ownedAdapters;
    struct GpuReservation { std::uint32_t adapter; std::uint64_t address, bytes; };
    std::map<std::uint32_t, GpuReservation> gpuReservations;
    std::uint64_t gpuReservedBytes = 0;
    GpuStateRanges gpuStates;
    std::map<std::uint32_t, std::uint32_t> pagingOwners;
    struct Context { std::uint32_t device, flags; };
    std::map<std::uint32_t, Context> contextOwners;
    std::map<std::uint32_t, std::uint32_t> hwQueueContexts;
    std::map<std::uint32_t, std::uint32_t> vendorResources; // typed resource ID -> its sole allocation alias
    std::set<std::uint32_t> importedVendorAllocations; // Successful owned resource opens only.
    std::set<std::uint32_t> sharedResources;
    NativeTextureDesc pendingNativeTexture{};
    std::uint32_t pendingNativeDevice = 0;
    pthread_t pendingNativeThread{};
    NativeResourcePreparations nativePreparations;
    bool nativeInteropQueried=false, nativeInteropSupported=false;
    NativeBufferPreparations nativeBufferPreparations;
    bool nativeGpuBufferQueried = false, nativeGpuBufferSupported = false;
    bool nativeGpuBufferAdmissionUnsupported = false;
    std::uint32_t lastHwSignalSync = 0;
    struct BorrowedFence { std::uint32_t owner, device; bool hardware; };
    std::map<std::uint32_t, BorrowedFence> borrowedFences;
    struct Synchronization { std::uint32_t device, type, flags; std::uint64_t lastSignal = 0; };
    std::map<std::uint32_t, Synchronization> syncObjects;
    bool resolveNativeTicket(int fd,std::uint32_t& ticket,std::uint32_t kind=WDDM_OBJECT_SYNC) {
        if(coordinatedProfileNegotiated) {
            if(kind==WDDM_OBJECT_SYNC) beginSharedFences();else beginSharedResources();
        }
        if(!kernelObjects.resolve(fd,ticket,kind)) return false;
        auto& known=kind==WDDM_OBJECT_SYNC ? exportedSyncTickets : exportedResourceTickets;
        if(known.count(ticket)) return true;
        // Only this explicit root-broker profile may return a recipient alias
        // which was not exported by the current process. Kernel FD authority
        // and native same-GPU checks happened in the privileged coordinator.
        const auto referenceLimit=kind==WDDM_OBJECT_SYNC ? MaxSharedSyncHandles : resourceReferenceLimit;
        if(!coordinatedProfileNegotiated || !guest_broker::clientEnabled() || !ticket || known.size()>=referenceLimit) {
            transport.fail();throw Error(EPROTO);
        }
        known.insert(ticket);
        guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE recipientReferenceAdopted=true kind=%u ticket=%u referenceLimit=%zu liveReferences=%zu authenticatedBroker=true\n",kind,ticket,referenceLimit,known.size());
        return true;
    }
    void reapObjectTickets() {
        std::uint32_t ticket = 0, kind = 0;
        for (;;) {
            const auto found = kernelObjects.reap(ticket, &kind);
            if (found < 0) throw Error(errno);
            if (!found) return;
            auto& tickets = kind == WDDM_OBJECT_SYNC ? exportedSyncTickets : exportedResourceTickets;
            if (!tickets.count(ticket)) { transport.fail(); throw Error(EPROTO); }
            const auto result = call(request(kind == WDDM_OBJECT_SYNC ? Op::CloseSharedSync : Op::CloseSharedResource, ticket), 0, true);
            if (result.header.handle != ticket || result.result.ntstatus > 0 || result.result.value) { transport.fail(); throw Error(EPROTO); }
            checkNt(result.result.ntstatus);
            tickets.erase(ticket);
            if (kind==WDDM_OBJECT_SYNC) exportedSyncSources.erase(ticket);
            guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE kernelObjectReaped=true ticket=%u\n", ticket);
        }
    }
    void beginSharedFences() {
        if (sharedFencesNegotiated) return;
        if (!kernelObjects.available()) throw Error(errno);
        const auto packet = transport.exchange(request(Op::BeginSyncSharing, 0));
        if (packet.size() != sizeof(Header) + sizeof(SyncSharingProfile)) { transport.fail(); throw Error(EPROTO); }
        Header h{}; SyncSharingProfile profile{};
        std::memcpy(&h, packet.data(), sizeof h); std::memcpy(&profile, packet.data() + sizeof h, sizeof profile);
        if (h.type != static_cast<std::uint32_t>(Op::BeginSyncSharing) || h.handle || h.status || h.padding || !validSyncSharingProfile(profile)) {
            transport.fail(); throw Error(EPROTO);
        }
        sharedFencesNegotiated = true;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeSyncSharingNegotiated=true kernelObjectFds=true\n");
    }
    void beginSharedResources() {
        if (sharedResourcesNegotiated) return;
        if (!kernelObjects.available()) throw Error(errno);
        const auto packet = transport.exchange(request(Op::BeginResourceSharing, 0));
        if (packet.size() != sizeof(Header) + sizeof(ResourceSharingProfile)) { transport.fail(); throw Error(EPROTO); }
        Header h{}; ResourceSharingProfile profile{};
        std::memcpy(&h, packet.data(), sizeof h); std::memcpy(&profile, packet.data() + sizeof h, sizeof profile);
        const bool validProfile=
#if defined(WDDM_BRIDGE_APPLICATION_REFERENCES) && WDDM_BRIDGE_APPLICATION_REFERENCES
            validApplicationResourceSharingProfile(profile);
#else
            validResourceSharingProfile(profile);
#endif
        if (h.type != static_cast<std::uint32_t>(Op::BeginResourceSharing) || h.handle || h.status || h.padding || !validProfile ||
            (coordinatedProfileNegotiated && std::memcmp(&profile,&coordinatedResourceProfile,sizeof profile))) {
            transport.fail(); throw Error(EPROTO);
        }
        sharedResourcesNegotiated = true;
        resourceReferenceLimit=profile.handleLimit;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeResourceSharingNegotiated=true kernelObjectFds=true dmaBuf=false\n");
    }
    static constexpr std::uint32_t GuestLuidLow = 0x57475055;
    static constexpr std::int32_t GuestLuidHigh = 0;
    Response call(const std::vector<std::uint8_t>& requestPacket, std::uint32_t bytes = 0, bool preserveNativeFailure = false) {
        const auto packet = transport.exchange(requestPacket);
        Header wanted{}, header{}; std::memcpy(&wanted, requestPacket.data(), sizeof wanted);
        std::memcpy(&header, packet.data(), sizeof header);
        if (header.type != wanted.type || header.padding) { transport.fail(); throw Error(EPROTO); }
        if (header.status) {
            if (header.status > 0 || header.status < -4095 || packet.size() != sizeof(Header)) {
                transport.fail(); throw Error(EPROTO);
            }
            throw Error(-header.status);
        }
        if (packet.size() < sizeof(Header) + sizeof(Reply)) { transport.fail(); throw Error(EPROTO); }
        Reply reply{}; std::memcpy(&reply, packet.data() + sizeof(Header), sizeof reply);
        if (reply.reserved) { transport.fail(); throw Error(EPROTO); }
        if (!preserveNativeFailure) checkNt(reply.ntstatus);
        if (packet.size() != sizeof(Header) + sizeof(Reply) + bytes) { transport.fail(); throw Error(EPROTO); }
        return {header, reply, {packet.begin() + sizeof(Header) + sizeof(Reply), packet.end()}};
    }
    void negotiate() {
        if (negotiated) { transport.start(); return; }
        const auto packet = transport.exchange(request(Op::Hello, 0, Version));
        if (packet.size() != sizeof(Header) + sizeof caps) { transport.fail(); throw Error(EPROTO); }
        Header header{}; std::memcpy(&header, packet.data(), sizeof header);
        std::memcpy(&caps, packet.data() + sizeof header, sizeof caps);
        if (header.type != static_cast<unsigned>(Op::Hello) || header.handle || header.status || header.padding ||
            caps.version != Version || caps.vendor != 0x10de || (caps.flags & (QueryCapability | ContextCapability)) != (QueryCapability | ContextCapability)) {
            transport.fail(); throw Error(EPROTO);
        }
        negotiated = true;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE vendor=%u device=%u realDxgForwarding=false\n", caps.vendor, caps.device);
        const auto coordinated=std::getenv("WDDM_BRIDGE_ACTOR_PROFILE");
        if(coordinated && *coordinated) {
            if(!gpu_diagnostic::HighResolution || (std::strcmp(coordinated,"10") && std::strcmp(coordinated,"11") && std::strcmp(coordinated,"12") && std::strcmp(coordinated,"13") && std::strcmp(coordinated,"14")) || !guest_broker::clientEnabled()) throw Error(EINVAL);
            const auto group=guest_broker::client().coordinatedProfile();
            coordinatedProfileVersion=!std::strcmp(coordinated,"14") ? 14 : !std::strcmp(coordinated,"13") ? 13 : !std::strcmp(coordinated,"12") ? 12 : !std::strcmp(coordinated,"11") ? 11 : 10;
            if(group.gl.version!=coordinatedProfileVersion || std::memcmp(&group.caps,&caps,sizeof caps)) { transport.fail();throw Error(EPROTO); }
            coordinatedResourceProfile=group.resource;
            coordinatedApplicationMemory=(group.flags&coordinator::ApplicationMemoryFlag)!=0;
#if !defined(WDDM_BRIDGE_APPLICATION_MEMORY) || !WDDM_BRIDGE_APPLICATION_MEMORY
            if(coordinatedApplicationMemory) throw Error(ENOTSUP);
#endif
            coordinatedReportedGpuByteLimit=group.capacity.reportedGpuByteLimit;
            beginOpenGl(coordinatedProfileVersion);
            coordinatedProfileNegotiated=true;
            guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE coordinatedNativeProfile=true version=%u cpuAndGpuBudgetsShared=true\n",coordinatedProfileVersion);
        }
    }
    std::uint32_t create(Op op, std::uint32_t parent = 0) {
        const auto out = call(request(op, parent));
        if (!out.header.handle) { transport.fail(); throw Error(EPROTO); }
        if (op == Op::OpenAdapter && !ownedAdapters.emplace(out.header.handle).second) { transport.fail(); throw Error(EPROTO); }
        return out.header.handle;
    }
    void destroy(Op op, std::uint32_t handle) {
        const auto out = call(request(op, handle));
        if (out.header.handle != handle) { transport.fail(); throw Error(EPROTO); }
    }
    static void adaptUmdName(std::vector<std::uint8_t>& data) {
        // DXCore does the UTF-16/WCHAR conversion and maps DriverStore paths
        // to /usr/lib/wsl/drivers. Change only the module basename selected
        // by the user, never vendor-private adapter initialization data.
        const auto name = std::getenv("WDDM_BRIDGE_LINUX_UMD_NAME");
        if (!name || !*name || std::strlen(name) > 128) throw Error(EINVAL);
        for (auto p = name; *p; ++p)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')) throw Error(EINVAL);
        if (data.size() != 524) throw Error(EPROTO);
        std::uint16_t path[260]{}; std::memcpy(path, data.data() + 4, sizeof path);
        const auto end = std::find(std::begin(path), std::end(path), std::uint16_t{0});
        if (end == std::end(path)) throw Error(EPROTO);
        std::size_t prefix = 0;
        for (auto p = std::begin(path); p != end; ++p) if (*p == '\\' || *p == '/') prefix = static_cast<std::size_t>(p - path) + 1;
        if (!prefix || prefix + std::strlen(name) >= 260) throw Error(EOVERFLOW);
        std::fill(path + prefix, std::end(path), std::uint16_t{0});
        for (std::size_t n = 0; name[n]; ++n) path[prefix + n] = static_cast<unsigned char>(name[n]);
        std::memcpy(data.data() + 4, path, sizeof path);
    }
    template<class T> static T& args(unsigned long requestNumber, void* pointer) {
        if (_IOC_SIZE(requestNumber) != sizeof(T)) throw Error(EINVAL);
        return *static_cast<T*>(pointer);
    }
    std::uint32_t wireAllocation(std::uint32_t alias) const {
        const auto found = vendorWireIds.find(alias);
        if (found == vendorWireIds.end()) throw Error(EBADF);
        return found->second;
    }
    void unlockCpu(std::uint32_t allocation, std::uint32_t device, bool force = false) {
        if (!cpuLocks.count(allocation)) throw Error(EBADF);
        try { if (!allocationMemory.release(allocation, force)) return; }
        catch (...) { transport.fail(); throw; }
        const auto wire = wireAllocation(allocation);
        const auto result = call(request(Op::UnlockVendorAllocation, wire, VendorCpuDesc{device, 0}), 0, true);
        if (result.header.handle != wire || result.result.value || result.result.ntstatus > 0) {
            transport.fail(); throw Error(EPROTO);
        }
        checkNt(result.result.ntstatus); cpuLocks.erase(allocation);
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorCpuUnlocked=true\n");
    }
public:
    std::mutex guard;
    void finish() {
        guest_bridge_log::report();
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE summary completedQueries=%u privateQueries=%u nativeContexts=%u realDxgForwarding=false\n",
                     completedQueries, privateQueries, contexts);
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE pagingSummary queues=%u directLoads=%llu\n", pagingFences.total(),
                     static_cast<unsigned long long>(pagingFences.directLoads()));
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuSummary mapped=%u unmapped=%u directLoads=%llu\n",
                     allocationMemory.total(), allocationMemory.released(), static_cast<unsigned long long>(allocationMemory.directLoads()));
        for (unsigned n = 0; n < 256; ++n)
            if (counts[n]) guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE ioctlSummary nr=%u count=%u\n", n, counts[n]);
        // Driver DSO finalizers and other threads can still read existing CPU
        // views after this atexit hook. Keep mappings/controllers and client
        // descriptors alive; exit_group closes/revokes them after all finalizers.
        // Closing client FDs here also permits reuse before a late DSO closes
        // its original descriptor. The bounded bridge allocation is process-owned.
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE processExitViewsRetained=true pagingFences=%zu hwQueueFences=%zu syncFences=%zu allocationViews=%zu release=kernel-process-exit\n",
                     pagingFences.active(), hwQueueFences.active(), syncFences.active(), allocationMemory.active());
        transport.finish();
    }
    int openDevice(int flags) {
        std::lock_guard<std::mutex> lock(guard);
        if (bridgeStopped) throw Error(ESHUTDOWN);
        if ((flags & O_CREAT) || (flags & O_TRUNC) || (flags & O_TMPFILE) == O_TMPFILE) throw Error(EINVAL);
        negotiate();
        const auto fd = static_cast<int>(syscall(SYS_memfd_create, "wddm-bridge-diagnostic", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0));
        if (fd < 0) throw Error(errno);
        struct stat information{};
        if (fstat(fd, &information)) { const auto error = errno; rawClose(fd); throw Error(error); }
        descriptors.emplace(fd, std::make_pair(information.st_dev, information.st_ino));
        return fd;
    }
    bool owns(int fd) {
        const auto found = descriptors.find(fd);
        if (found == descriptors.end()) return false;
        struct stat information{};
        if (fstat(fd, &information) || found->second != std::make_pair(information.st_dev, information.st_ino)) {
            descriptors.erase(found); return false;
        }
        return true;
    }
    void closed(int fd) { descriptors.erase(fd); }
    int dispatch(unsigned long requestNumber, void* pointer) {
        if (_IOC_TYPE(requestNumber) != 'G' || !pointer || _IOC_DIR(requestNumber) != (_IOC_READ | _IOC_WRITE)) throw Error(EINVAL);
        reapObjectTickets();
        const auto nr = static_cast<unsigned>(_IOC_NR(requestNumber)); ++counts[nr];
        switch (nr) {
            case 14: {
                auto& a = args<D3DKMT_GETDEVICESTATE>(requestNumber, pointer);
                if(a.StateType!=D3DKMT_DEVICESTATE_EXECUTION) throw Error(ENOSYS);
                if(!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                Response result{};
                try { result=call(request(Op::QueryDeviceExecutionState,a.hDevice),0,true); }
                catch(const Error& error) {
                    // Older native sessions reject the optional opcode.
                    if(error.number==EOPNOTSUPP) throw Error(ENOSYS);
                    throw;
                }
                if(result.header.handle!=a.hDevice || result.result.ntstatus>0 ||
                   (result.result.ntstatus==0 ? result.result.value<1 || result.result.value>7 : result.result.value!=0)) {
                    transport.fail();throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                // The execution-state union arm is out-only. Leave all other
                // fields/padding untouched; never copy a host union or pointer.
                a.ExecutionState=static_cast<D3DKMT_DEVICEEXECUTION_STATE>(result.result.value);
                return 0;
            }
            case 63: {
                const auto& a = args<D3DKMT_SHAREOBJECTS>(requestNumber, pointer);
                if (a.ObjectCount != 1 || !a.ObjectHandleArray || !a.pSharedNtHandle || a.DesiredAccess != UINT32_C(0x10000000)) throw Error(EINVAL);
                if (a.pObjectAttributes) {
                    struct Attributes { std::uint32_t length, pad; std::uint64_t root, name; std::uint32_t flags, pad2; std::uint64_t security, qos; } attributes{};
                    static_assert(sizeof attributes == 48, "x64 object attributes");
                    std::memcpy(&attributes, a.pObjectAttributes, sizeof attributes);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE shareAttributes length=%u flags=%u hasRoot=%u hasName=%u hasSecurity=%u hasQos=%u\n",
                        attributes.length, attributes.flags, attributes.root ? 1u : 0u,
                        attributes.name ? 1u : 0u, attributes.security ? 1u : 0u, attributes.qos ? 1u : 0u);
                    // The real Linux D3D12 runtime passes length zero for
                    // empty attributes and leaves C ABI padding undefined.
                    // Normalize only that empty request; names, security
                    // descriptors, inheritance and rooted opens stay denied.
                    if ((attributes.length != 0 && attributes.length != 48) || attributes.root || attributes.name || attributes.flags || attributes.security || attributes.qos) throw Error(ENOSYS);
                }
                const auto object = a.ObjectHandleArray[0];
                const auto source = syncObjects.find(object);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeShareObjectInput resource=%s sync=%s\n",
                    vendorResources.count(object) ? "true" : "false", source != syncObjects.end() ? "true" : "false");
                if (vendorResources.count(object)) {
                    if (!sharedResources.count(object)) throw Error(EINVAL);
                    beginSharedResources();
                    const auto result = call(request(Op::ExportResource, object), 0, true);
                    if (result.result.ntstatus > 0 || result.result.value ||
                        (result.result.ntstatus < 0 ? result.header.handle != 0 : !result.header.handle ||
                         exportedResourceTickets.count(result.header.handle) || exportedSyncTickets.count(result.header.handle))) {
                        transport.fail(); throw Error(EPROTO);
                    }
                    checkNt(result.result.ntstatus);
                    const auto ticket = result.header.handle;
                    int fd = -1;
                    try {
                        if (!exportedResourceTickets.insert(ticket).second) { transport.fail(); throw Error(EPROTO); }
                        fd = kernelObjects.create(ticket, WDDM_OBJECT_RESOURCE);
                        if (fd < 0) throw Error(errno);
                    } catch (...) {
                        if (fd >= 0) rawClose(fd);
                        exportedResourceTickets.erase(ticket);
                        destroy(Op::CloseSharedResource, ticket);
                        throw;
                    }
                    *a.pSharedNtHandle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd));
                    return 0;
                }
                if (source == syncObjects.end() || source->second.type != 5 ||
                    ((source->second.flags & 3) != 3 && !importedShareableSyncs.count(object))) throw Error(EINVAL);
                beginSharedFences();
                const auto result = call(request(Op::ExportSync, object), 0, true);
                if (result.result.ntstatus > 0 || result.result.value ||
                    (result.result.ntstatus < 0 ? result.header.handle != 0 : !result.header.handle || exportedSyncTickets.count(result.header.handle))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                const auto ticket = result.header.handle;
                int fd = -1;
                try {
                    // Allocate bookkeeping before publishing a kernel FD:
                    // allocation failure must not queue a ticket already
                    // closed by rollback for a second native close.
                    if (!exportedSyncTickets.insert(ticket).second) { transport.fail(); throw Error(EPROTO); }
                    if (!exportedSyncSources.emplace(ticket,object).second) { transport.fail(); throw Error(EPROTO); }
                    fd = kernelObjects.create(ticket);
                    if (fd < 0) throw Error(errno);
                } catch (...) {
                    if (fd >= 0) rawClose(fd);
                    exportedSyncTickets.erase(ticket);
                    exportedSyncSources.erase(ticket);
                    destroy(Op::CloseSharedSync, ticket);
                    throw;
                }
                *a.pSharedNtHandle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd));
                return 0;
            }
            case 65: {
                auto& a = args<D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE>(requestNumber, pointer);
                const auto handle = reinterpret_cast<std::intptr_t>(a.hNtHandle);
                if (handle < 0 || handle > INT32_MAX) throw Error(EINVAL);
                if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                std::uint32_t ticket = 0;
                if (resolveNativeTicket(static_cast<int>(handle), ticket, WDDM_OBJECT_RESOURCE)) {
                    if (!exportedResourceTickets.count(ticket)) { transport.fail(); throw Error(EPROTO); }
                    if ((a.pPrivateRuntimeData != nullptr) != (a.PrivateRuntimeDataSize != 0) || a.PrivateRuntimeDataSize > MaxSharedResourceRuntimeBytes)
                        throw Error(EINVAL);
                    const auto capacity = a.PrivateRuntimeDataSize;
                    const auto result = call(request(Op::QuerySharedResourceInfo, ticket, SharedResourceQueryDesc{a.hDevice, capacity}), sizeof(SharedResourceInfo) + capacity, true);
                    SharedResourceInfo info{}; std::memcpy(&info, result.data.data(), sizeof info);
                    if (result.header.handle != ticket || result.result.ntstatus > 0 || result.result.value ||
                        (result.result.ntstatus < 0 ? (info.runtimeBytes || info.driverBytes || info.resourceDriverBytes || info.allocations) :
                         (!validSharedResourceInfo(info) || (capacity && capacity != info.runtimeBytes)))) { transport.fail(); throw Error(EPROTO); }
                    checkNt(result.result.ntstatus);
                    if (capacity) std::memcpy(a.pPrivateRuntimeData, result.data.data() + sizeof info, capacity);
                    a.PrivateRuntimeDataSize = info.runtimeBytes; a.TotalPrivateDriverDataSize = info.driverBytes;
                    a.ResourcePrivateDriverDataSize = info.resourceDriverBytes; a.NumAllocations = info.allocations;
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeResourceInfoQueried=true runtimeBytes=%u driverBytes=%u resourceDriverBytes=%u allocations=%u copied=%u\n",
                        info.runtimeBytes, info.driverBytes, info.resourceDriverBytes, info.allocations, capacity);
                    return 0;
                }
                if (a.pPrivateRuntimeData || a.PrivateRuntimeDataSize || a.TotalPrivateDriverDataSize || a.ResourcePrivateDriverDataSize || a.NumAllocations)
                    throw Error(EINVAL);
                beginSharedFences();
                if (!resolveNativeTicket(static_cast<int>(handle), ticket)) throw Error(errno);
                if (!exportedSyncTickets.count(ticket)) { transport.fail(); throw Error(EPROTO); }
                const auto result = call(request(Op::QuerySharedSyncInfo, ticket, SharedSyncInfoDesc{a.hDevice, 0}), 0, true);
                if (result.header.handle != ticket || result.result.ntstatus >= 0 || result.result.value) {
                    transport.fail(); throw Error(EPROTO);
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeSyncResourceQueryRejected=true ntstatus=%d\n", result.result.ntstatus);
                checkNt(result.result.ntstatus);
                throw Error(EPROTO);
            }
            case 66: {
                auto& a = args<D3DKMT_OPENRESOURCEFROMNTHANDLE>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeResourceImportInput allocations=%u runtimeBytes=%u driverBytes=%u resourceDriverBytes=%u hasAllocationInfo=%u hasRuntime=%u hasDriver=%u hasResourceDriver=%u keyedRuntimeBytes=%u\n",
                    a.NumAllocations, a.PrivateRuntimeDataSize, a.TotalPrivateDriverDataBufferSize, a.ResourcePrivateDriverDataSize,
                    a.pOpenAllocationInfo2 ? 1u : 0u, a.pPrivateRuntimeData ? 1u : 0u, a.pTotalPrivateDriverDataBuffer ? 1u : 0u, a.pResourcePrivateDriverData ? 1u : 0u, a.KeyedMutexPrivateRuntimeDataSize);
                const SharedResourceOpenDesc desc{a.hDevice, a.PrivateRuntimeDataSize, a.TotalPrivateDriverDataBufferSize, a.ResourcePrivateDriverDataSize};
                const auto handle = reinterpret_cast<std::intptr_t>(a.hNtHandle);
                // The Linux D3D12 runtime supplies a four-byte optional mutex
                // initializer even for ordinary textures. This owned profile
                // cannot create keyed mutexes; the native open must return no
                // mutex or sync handle. Do not apply Linux private metadata
                // to a Windows mutex or advertise keyed-mutex support.
                if (handle < 0 || handle > INT32_MAX || !validSharedResourceOpen(desc) || a.NumAllocations != 1 || !a.pOpenAllocationInfo2 ||
                    !a.pPrivateRuntimeData || !a.pTotalPrivateDriverDataBuffer || (desc.resourceDriverBytes && !a.pResourcePrivateDriverData) ||
                    (a.KeyedMutexPrivateRuntimeDataSize && (a.KeyedMutexPrivateRuntimeDataSize != 4 || !a.pKeyedMutexPrivateRuntimeData))) throw Error(EINVAL);
                if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                std::uint32_t ticket = 0;
                if (!resolveNativeTicket(static_cast<int>(handle), ticket, WDDM_OBJECT_RESOURCE)) throw Error(errno);
                if (!exportedResourceTickets.count(ticket)) { transport.fail(); throw Error(EPROTO); }
                const auto dataBytes = desc.runtimeBytes + desc.driverBytes + desc.resourceDriverBytes;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeResourceImportForwarded=true boundedBytes=%u\n", dataBytes);
                const auto result = call(request(Op::ImportResource, ticket, desc), sizeof(SharedResourceOpenReply) + dataBytes, true);
                SharedResourceOpenReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.result.ntstatus > 0 || output.reserved ||
                    (result.result.ntstatus < 0 ? (result.header.handle || result.result.value || output.resource || output.allocationDriverBytes || output.allocationDriverOffset) :
                     (!result.header.handle || !output.resource || output.resource == result.header.handle || vendorResources.count(output.resource) ||
                      result.result.value % 4096 || result.result.value >= MaxGpuAddress || !output.allocationDriverBytes ||
                      output.allocationDriverOffset > desc.driverBytes || output.allocationDriverBytes > desc.driverBytes - output.allocationDriverOffset))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                std::uint32_t alias = result.header.handle;
                bool ownerAdded = false, wireAdded = false, resourceAdded = false, sharedAdded = false, importedAdded = false;
                try {
                    if (caps.flags & VendorTranslationCapability) {
                        const auto translated = call(request(Op::TranslateVendorAllocation, result.header.handle,
                            VendorTranslationDesc{a.hDevice, deviceAdapters.at(a.hDevice), 0, 0}), 0, true);
                        if (translated.header.handle != result.header.handle || translated.result.ntstatus > 0 ||
                            (translated.result.ntstatus < 0 ? translated.result.value != 0 : !translated.result.value || translated.result.value > UINT32_MAX)) {
                            transport.fail(); throw Error(EPROTO);
                        }
                        checkNt(translated.result.ntstatus); alias = static_cast<std::uint32_t>(translated.result.value);
                    }
                    if (vendorOwners.count(alias) || vendorWireIds.count(alias)) { transport.fail(); throw Error(EPROTO); }
                    ownerAdded = vendorOwners.emplace(alias, a.hDevice).second;
                    wireAdded = vendorWireIds.emplace(alias, result.header.handle).second;
                    resourceAdded = vendorResources.emplace(output.resource, alias).second;
                    sharedAdded = sharedResources.insert(output.resource).second;
                    importedAdded = importedVendorAllocations.insert(alias).second;
                    if (!ownerAdded || !wireAdded || !resourceAdded || !sharedAdded || !importedAdded) { transport.fail(); throw Error(EPROTO); }
                } catch (...) {
                    // EOF cleanup remains the backstop if the native worker
                    // or malformed response has already closed the transport.
                    if (ownerAdded) vendorOwners.erase(alias);
                    if (wireAdded) vendorWireIds.erase(alias);
                    if (resourceAdded) vendorResources.erase(output.resource);
                    if (sharedAdded) sharedResources.erase(output.resource);
                    if (importedAdded) importedVendorAllocations.erase(alias);
                    try { const auto released = call(request(Op::DestroyVendorResource, output.resource, a.hDevice));
                          if (released.header.handle != output.resource || released.result.value) transport.fail(); }
                    catch (...) { transport.fail(); }
                    throw;
                }
                auto data = result.data.data() + sizeof output;
                std::memcpy(a.pPrivateRuntimeData, data, desc.runtimeBytes); data += desc.runtimeBytes;
                if (desc.resourceDriverBytes) std::memcpy(a.pResourcePrivateDriverData, data, desc.resourceDriverBytes);
                data += desc.resourceDriverBytes; std::memcpy(a.pTotalPrivateDriverDataBuffer, data, desc.driverBytes);
                auto& allocation = a.pOpenAllocationInfo2[0]; allocation.hAllocation = alias;
                allocation.pPrivateDriverData = static_cast<std::uint8_t*>(a.pTotalPrivateDriverDataBuffer) + output.allocationDriverOffset;
                allocation.PrivateDriverDataSize = output.allocationDriverBytes; allocation.GpuVirtualAddress = result.result.value;
                std::memset(allocation.Reserved, 0, sizeof allocation.Reserved);
                a.hResource = output.resource; a.hKeyedMutex = 0; a.hSyncObject = 0;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeResourceFdImported=true allocations=1 rawHostHandle=false\n");
                return 0;
            }
            case 64: {
                auto& a = args<D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2>(requestNumber, pointer);
                const auto handle = reinterpret_cast<std::intptr_t>(a.hNtHandle);
                SharedSyncOpenDesc desc{a.hDevice, a.Flags.Value, a.MonitoredFence.EngineAffinity, 0};
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeSyncFdImportInput flags=%u affinity=%u outputFieldsIgnored=true\n", desc.flags, desc.affinity);
                // The active MonitoredFence union contains two out-only
                // addresses and input EngineAffinity; its inactive Reserved
                // arm and ABI padding are not input validation fields.
                if (handle < 0 || handle > INT32_MAX || !validSharedSyncOpen(desc)) throw Error(EINVAL);
                if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                beginSharedFences();
                std::uint32_t ticket = 0;
                if (!resolveNativeTicket(static_cast<int>(handle), ticket)) throw Error(errno);
                if (!exportedSyncTickets.count(ticket)) { transport.fail(); throw Error(EPROTO); }
                if (syncObjects.size() >= syncObjectLimit()) throw Error(EMFILE);
                const auto result = call(request(Op::ImportSync, ticket, desc), sizeof(SyncReply), true);
                SyncReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.result.ntstatus > 0 || result.result.value ||
                    (result.result.ntstatus < 0 ? (result.header.handle || output.offset || output.gpuAddress) :
                     (!result.header.handle || syncObjects.count(result.header.handle) || output.offset % 8 || output.offset >= fenceApertureBytes ||
                      !validSyncGpuAddress({5, desc.flags, desc.affinity, 0, 0}, output.gpuAddress)))) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                void* fence = nullptr;
                try {
                    fence = syncFences.map(result.header.handle, output.offset, "imported-monitored");
                    syncObjects.emplace(result.header.handle, Synchronization{desc.device,5,desc.flags,0});
                    importedShareableSyncs.insert(result.header.handle);
                } catch (...) {
                    if (fence) syncFences.unmap(result.header.handle);
                    syncObjects.erase(result.header.handle); importedShareableSyncs.erase(result.header.handle);
                    destroy(Op::DestroySync, result.header.handle); throw;
                }
                a.hSyncObject = result.header.handle;
                a.MonitoredFence.FenceValueCPUVirtualAddress = fence;
                a.MonitoredFence.FenceValueGPUVirtualAddress = output.gpuAddress;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeSyncFdImported=true gpuMapped=%u\n", output.gpuAddress ? 1u : 0u);
                return 0;
            }
            case 62: {
                auto& a = args<D3DKMT_ENUMADAPTERS3>(requestNumber, pointer);
                if (a.Filter.Value & ~7ull) throw Error(EINVAL);
                if (!a.pAdapters) { a.NumAdapters = 1; return 0; }
                if (!a.NumAdapters) { a.NumAdapters = 1; throw Error(EOVERFLOW); }
                D3DKMT_ADAPTERINFO info{}; info.hAdapter = create(Op::OpenAdapter);
                info.AdapterLuid.LowPart = GuestLuidLow; info.AdapterLuid.HighPart = GuestLuidHigh;
                // Physical Windows outputs are not guest outputs. QEMU
                // scanout is a separate, currently unimplemented interface.
                info.NumOfSources = 0; info.bPrecisePresentRegionsPreferred = 0;
                a.pAdapters[0] = info; a.NumAdapters = 1; return 0;
            }
            case 1: {
                auto& a = args<D3DKMT_OPENADAPTERFROMLUID>(requestNumber, pointer);
                if (a.AdapterLuid.LowPart != GuestLuidLow || a.AdapterLuid.HighPart != GuestLuidHigh) throw Error(ENODEV);
                a.hAdapter = create(Op::OpenAdapter); return 0;
            }
            case 21: {
                auto& a = args<D3DKMT_CLOSEADAPTER>(requestNumber, pointer);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                destroy(Op::CloseAdapter, a.hAdapter); ownedAdapters.erase(a.hAdapter); return 0;
            }
            case 9: {
                auto& a = args<D3DKMT_QUERYADAPTERINFO>(requestNumber, pointer);
                const auto type = static_cast<unsigned>(a.Type);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE query type=%u bytes=%u\n", type, a.PrivateDriverDataSize);
                if (!type) {
                    bool loaded = false; dl_iterate_phdr(findNvidiaModule, &loaded);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nvidiaUmdPresentDuringPrivateQuery=%s\n", loaded ? "true" : "false");
                }
                if (!a.pPrivateDriverData || !validQuery({type, a.PrivateDriverDataSize, 0, 0})) throw Error(ENOSYS);
                const auto begin = static_cast<const std::uint8_t*>(a.pPrivateDriverData);
                const auto result = adapterQuery([&](const auto& packet) { return transport.exchange(packet); },
                                                 a.hAdapter, type, std::vector<std::uint8_t>(begin, begin + a.PrivateDriverDataSize));
                auto data = result.data;
                // Preserve native in/out bytes even when the driver fails.
                if (result.ntstatus >= 0) {
                    if (type == 1) adaptUmdName(data);
                    if (type == 15) {
                        std::uint32_t flags{}; std::memcpy(&flags, data.data(), sizeof flags);
                        flags |= 1u << 7; // Paravirtualized.
                        flags &= ~((1u << 1) | (1u << 3) | (1u << 6) | (1u << 8) | (1u << 9) | (1u << 11));
                        std::memcpy(data.data(), &flags, sizeof flags);
                    }
                }
                std::memcpy(a.pPrivateDriverData, data.data(), data.size()); checkNt(result.ntstatus);
                ++completedQueries; if (type == 0) ++privateQueries;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE queryCompleted type=%u bytes=%zu\n", type, data.size()); return 0;
            }
            case 10: {
                auto& a=args<D3DKMT_QUERYVIDEOMEMORYINFO>(requestNumber,pointer);
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE memoryBudgetInput ownedAdapter=%u group=%u physicalAdapter=%u currentProcess=%u\n",
                    ownedAdapters.count(a.hAdapter)?1u:0u,static_cast<unsigned>(a.MemorySegmentGroup),a.PhysicalAdapterIndex,
                    !a.hProcess || a.hProcess==reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1))?1u:0u);
                if (!(caps.flags & VendorMemoryManagementCapability)) throw Error(ENOSYS);
                if (a.hProcess && a.hProcess!=reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1))) throw Error(EACCES);
                const MemoryBudgetDesc desc{static_cast<std::uint32_t>(a.MemorySegmentGroup),a.PhysicalAdapterIndex};
                if (!validMemoryBudget(desc)) throw Error(EINVAL);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                const auto result=call(request(Op::QueryMemoryBudget,a.hAdapter,desc),sizeof(MemoryBudgetReply),true);
                MemoryBudgetReply info{}; std::memcpy(&info,result.data.data(),sizeof info);
                if (result.header.handle!=a.hAdapter || result.result.value || result.result.ntstatus>0 ||
                    (result.result.ntstatus<0 && (info.budget || info.usage || info.reservation || info.available))) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                a.Budget=info.budget; a.CurrentUsage=info.usage; a.CurrentReservation=info.reservation; a.AvailableForReservation=info.available;
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE nativeMemoryBudget=true group=%u budget=%llu usage=%llu reservation=%llu available=%llu currentProcessOnly=true\n",
                    desc.group,(unsigned long long)info.budget,(unsigned long long)info.usage,(unsigned long long)info.reservation,(unsigned long long)info.available);
                return 0;
            }
            case 30: {
                auto& a=args<D3DKMT_EVICT>(requestNumber,pointer);
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE evictionInput count=%u flags=%u ownedDevice=%u hasAllocations=%u\n",
                    a.NumAllocations,a.Flags.Value,deviceAdapters.count(a.hDevice)?1u:0u,a.AllocationList?1u:0u);
                if (!(caps.flags & VendorMemoryManagementCapability)) throw Error(ENOSYS);
                const EvictDesc desc{a.NumAllocations,a.Flags.Value};
                if (!validEvict(desc) || !a.AllocationList) throw Error(EINVAL);
                if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                std::vector<std::uint32_t> ids; ids.reserve(desc.count);
                for (std::uint32_t n=0;n<desc.count;++n) {
                    const auto found=vendorOwners.find(a.AllocationList[n]);
                    if (found==vendorOwners.end() || found->second!=a.hDevice) throw Error(EBADF);
                    if (std::find(a.AllocationList,a.AllocationList+n,a.AllocationList[n])!=a.AllocationList+n) throw Error(EINVAL);
                    guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE evictionAllocation index=%u cpuLease=%u\n",n,cpuLocks.count(a.AllocationList[n])?1u:0u);
                    ids.push_back(wireAllocation(a.AllocationList[n]));
                }
                auto packet=request(Op::EvictVendorAllocations,a.hDevice,desc);
                const auto bytes=reinterpret_cast<const std::uint8_t*>(ids.data()); packet.insert(packet.end(),bytes,bytes+ids.size()*4);
                const auto result=call(packet,0,true);
                if (result.header.handle!=a.hDevice || result.result.ntstatus>0 || (result.result.ntstatus<0 && result.result.value)) { transport.fail(); throw Error(EPROTO); }
                a.NumBytesToTrim=result.result.value; checkNt(result.result.ntstatus);
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE nativeVendorEviction=true count=%u flags=%u bytesToTrim=%llu\n",desc.count,desc.flags,(unsigned long long)a.NumBytesToTrim);
                return 0;
            }
            case 2: {
                auto& a = args<D3DKMT_CREATEDEVICE>(requestNumber, pointer);
                unsigned flags{}; static_assert(sizeof a.Flags == sizeof flags); std::memcpy(&flags, &a.Flags, sizeof flags);
                if (flags != 2) throw Error(ENOSYS);
                a.hDevice = create(Op::CreateDevice, a.hAdapter);
                if (!deviceAdapters.emplace(a.hDevice, a.hAdapter).second) { transport.fail(); throw Error(EPROTO); }
                a.pCommandBuffer = nullptr; a.CommandBufferSize = 0; a.pAllocationList = nullptr;
                a.AllocationListSize = 0; a.pPatchLocationList = nullptr; a.PatchLocationListSize = 0;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE deviceCreated=true\n"); return 0;
            }
            case 25: {
                auto& a = args<D3DKMT_DESTROYDEVICE>(requestNumber, pointer);
                destroy(Op::DestroyDevice, a.hDevice); deviceAdapters.erase(a.hDevice);
                if (pendingNativeDevice == a.hDevice) pendingNativeDevice = 0;
                nativePreparations.destroyDevice(a.hDevice);
                nativeBufferPreparations.destroyDevice(a.hDevice);
                return 0;
            }
            case 7: {
                auto& a = args<D3DKMT_CREATEPAGINGQUEUE>(requestNumber, pointer);
                if (!(caps.flags & GuestPagingCapability) || a.Priority != D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL || a.PhysicalAdapterIndex) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=7 bytes=%zu\n", sizeof a);
                    throw Error(ENOSYS);
                }
                const auto result = call(request(Op::CreateGuestPagingQueue, a.hDevice), sizeof(PagingReply));
                PagingReply reply{}; std::memcpy(&reply, result.data.data(), sizeof reply);
                if (!result.header.handle || !reply.sync || reply.sync == result.header.handle || reply.reserved || result.result.value ||
                    reply.offset % 8 || reply.offset >= fenceApertureBytes) { transport.fail(); throw Error(EPROTO); }
                try { a.FenceValueCPUVirtualAddress = pagingFences.map(result.header.handle, reply.offset); }
                catch (...) {
                    // The failed mapping never escapes to the runtime. The
                    // native owner acknowledges unmap before freeing its page.
                    destroy(Op::DestroyPagingQueue, result.header.handle);
                    throw;
                }
                a.hPagingQueue = result.header.handle; a.hSyncObject = reply.sync;
                pagingOwners.emplace(a.hPagingQueue, a.hDevice);
                if (!borrowedFences.emplace(reply.sync, BorrowedFence{a.hPagingQueue, a.hDevice, false}).second) { transport.fail(); throw Error(EPROTO); }
                return 0;
            }
            case 28: {
                auto& a = args<D3DDDI_DESTROYPAGINGQUEUE>(requestNumber, pointer);
                pagingFences.unmap(a.hPagingQueue);
                destroy(Op::DestroyPagingQueue, a.hPagingQueue); pagingOwners.erase(a.hPagingQueue);
                for (auto fence = borrowedFences.begin(); fence != borrowedFences.end();) {
                    if (!fence->second.hardware && fence->second.owner == a.hPagingQueue) fence = borrowedFences.erase(fence); else ++fence;
                }
                return 0;
            }
            case 4: {
                auto& a = args<D3DKMT_CREATECONTEXTVIRTUAL>(requestNumber, pointer);
                const ContextDesc desc{a.NodeOrdinal, a.EngineAffinity, a.Flags.Value, static_cast<unsigned>(a.ClientHint), a.PrivateDriverDataSize, 0};
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE contextInput node=%u engine=%u flags=%u clientHint=%u privateBytes=%u\n",
                    desc.node, desc.engine, desc.flags, desc.clientHint, desc.privateBytes);
                if (!validContext(desc) || (desc.privateBytes && !a.pPrivateDriverData)) throw Error(ENOSYS);
                auto packet = request(Op::CreateContext, a.hDevice, desc);
                if (desc.privateBytes) {
                    const auto begin = static_cast<const std::uint8_t*>(a.pPrivateDriverData); packet.insert(packet.end(), begin, begin + desc.privateBytes);
                }
                const auto result = call(packet, desc.privateBytes);
                if (!result.header.handle) { transport.fail(); throw Error(EPROTO); }
                a.hContext = result.header.handle;
                if (!contextOwners.emplace(a.hContext, Context{a.hDevice, desc.flags}).second) { transport.fail(); throw Error(EPROTO); }
                if (desc.privateBytes) std::memcpy(a.pPrivateDriverData, result.data.data(), result.data.size());
                ++contexts; guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeContextCreated=true bytes=%u flags=%u\n", desc.privateBytes, desc.flags); return 0;
            }
            case 5: {
                auto& a = args<D3DKMT_DESTROYCONTEXT>(requestNumber, pointer);
                for (const auto& queue : hwQueueContexts) if (queue.second == a.hContext) throw Error(EBUSY);
                destroy(Op::DestroyContext, a.hContext); contextOwners.erase(a.hContext); return 0;
            }
            case 6: {
                auto& a = args<D3DKMT_CREATEALLOCATION>(requestNumber, pointer);
                unsigned flags{}; static_assert(sizeof a.Flags == sizeof flags);
                std::memcpy(&flags, &a.Flags, sizeof flags);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationInput flags=%u count=%u runtimeBytes=%u privateBytes=%u resourceReuse=%u\n",
                             flags, a.NumAllocations, a.PrivateRuntimeDataSize, a.PrivateDriverDataSize, a.hResource ? 1u : 0u);
                if (a.NumAllocations <= 16 && a.pAllocationInfo2)
                    for (unsigned n = 0; n < a.NumAllocations; ++n) {
                        const auto& item = a.pAllocationInfo2[n];
                        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationItem index=%u flags=%u privateBytes=%u hasSystemMemory=%u priority=%u source=%u\n",
                                     n, item.Flags.Value, item.PrivateDriverDataSize, item.pSystemMem ? 1u : 0u, item.Priority, item.VidPnSourceId);
                    }
                const bool sharedResource = flags == 71; // CreateResource | CreateShared | NonSecure | NtSecuritySharing
                const bool withResource = flags == 1 || sharedResource;
                const auto thread=static_cast<std::uint64_t>(syscall(SYS_gettid));
                const bool nativeInterop=sharedResource && nativePreparations.contains(thread);
                const bool nativeTexture = sharedResource && (pendingNativeDevice || nativeInterop);
                const bool nativeGpuBuffer = sharedResource && nativeBufferPreparations.contains(thread);
                if (sharedResource && !nativeGpuBuffer && nativeBufferPreparations.pendingForDevice(a.hDevice)) throw Error(EBUSY);
                if (nativeTexture && nativeGpuBuffer) { nativeBufferPreparations.cancel(thread); throw Error(EBUSY); }
                NativeGpuBufferDesc bufferProfile{};
                if (nativeGpuBuffer) {
                    const auto prepared = nativeBufferPreparations.take(thread, a.hDevice, deviceAdapters);
                    if (!prepared) throw Error(EBADF);
                    bufferProfile = *prepared;
                }
                auto profile = pendingNativeTexture;
                if(nativeInterop) {
                    const auto prepared=nativePreparations.take(thread,a.hDevice,deviceAdapters);
                    if(!prepared) throw Error(EBADF);
                    profile=*prepared;
                } else if (nativeTexture) {
                    const auto preparedDevice = pendingNativeDevice; pendingNativeDevice = 0;
                    if (a.hDevice != preparedDevice || !pthread_equal(pendingNativeThread, pthread_self())) throw Error(EBADF);
                }
                if (!(caps.flags & VendorAllocationCapability) || (flags != 0 && !withResource) ||
                    (withResource && !(caps.flags & VendorResourceCapability)) ||
                    (sharedResource && !(caps.flags & SharedVendorResourceCapability)) || a.hResource ||
                    (!sharedResource && a.PrivateRuntimeDataSize) ||
                    a.PrivateDriverDataSize || a.NumAllocations != 1 || !a.pAllocationInfo2) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=6 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                auto& item = a.pAllocationInfo2[0];
                const VendorAllocationDesc desc{item.Flags.Value, item.Priority, item.VidPnSourceId, item.PrivateDriverDataSize, 0, 0};
                if (!validVendorAllocation(desc) || item.pSystemMem || !item.pPrivateDriverData ||
                    std::any_of(std::begin(item.Reserved), std::end(item.Reserved), [](auto value) { return value != 0; })) throw Error(EINVAL);
                if (vendorOwners.size() >= (openglDesktopNegotiated ? OpenGlDesktopVendorAllocations : openglCompositorNegotiated ? OpenGlCompositorVendorAllocations : openglFencesNegotiated ? OpenGlFenceVendorAllocations : vendorAllocationObjectLimit(caps.flags))) throw Error(EMFILE);
                const SharedVendorAllocationDesc sharedDesc{desc, a.PrivateRuntimeDataSize, 0};
                if (sharedResource && (!validSharedVendorAllocation(sharedDesc) || !a.pPrivateRuntimeData)) throw Error(EINVAL);
                const NativeSharedVendorAllocationDesc nativeDesc{sharedDesc, profile};
                if (nativeTexture && (nativeInterop ? !validNativeInteropAllocation(nativeDesc) : !validNativeSharedVendorAllocation(nativeDesc))) throw Error(EINVAL);
                const NativeSharedGpuBufferAllocationDesc bufferDesc{sharedDesc, bufferProfile};
                if (nativeGpuBuffer && !validNativeSharedGpuBufferAllocation(bufferDesc)) throw Error(EINVAL);
                auto packet = nativeGpuBuffer ? request(Op::CreateNativeSharedGpuBufferAllocation, a.hDevice, bufferDesc) :
                    nativeInterop ? request(Op::CreateNativeInteropResource,a.hDevice,nativeDesc) :
                    nativeTexture ? request(Op::CreateNativeSharedVendorResourceAllocation, a.hDevice, nativeDesc) :
                    sharedResource ? request(Op::CreateSharedVendorResourceAllocation, a.hDevice, sharedDesc) :
                    request(withResource ? Op::CreateVendorResourceAllocation : Op::CreateVendorAllocation, a.hDevice, desc);
                const auto begin = static_cast<const std::uint8_t*>(item.pPrivateDriverData);
                packet.insert(packet.end(), begin, begin + desc.privateBytes);
                if (sharedResource) {
                    const auto runtime = static_cast<const std::uint8_t*>(a.pPrivateRuntimeData);
                    packet.insert(packet.end(), runtime, runtime + sharedDesc.runtimeBytes);
                }
                const auto result = call(packet, desc.privateBytes + (sharedResource ? sharedDesc.runtimeBytes : 0) +
                                         (withResource ? sizeof(VendorResourceReply) : 0), true);
                if (sharedResource && std::memcmp(static_cast<const std::uint8_t*>(a.pPrivateRuntimeData),
                        result.data.data() + sizeof(VendorResourceReply) + desc.privateBytes, sharedDesc.runtimeBytes)) {
                    transport.fail(); throw Error(EPROTO);
                }
                VendorResourceReply resource{};
                if (withResource) std::memcpy(&resource, result.data.data(), sizeof resource);
                const bool reusedWireId = std::any_of(vendorWireIds.begin(), vendorWireIds.end(), [&](const auto& owned) {
                    return owned.second == result.header.handle;
                });
                if (result.result.ntstatus > 0 || resource.reserved ||
                    (result.result.ntstatus >= 0 && (!result.header.handle || result.result.value % 4096 || reusedWireId ||
                        (withResource && (!resource.resource || resource.resource == result.header.handle || vendorResources.count(resource.resource))))) ||
                    (result.result.ntstatus < 0 && (result.header.handle || result.result.value || resource.resource))) { transport.fail(); throw Error(EPROTO); }
                std::memcpy(item.pPrivateDriverData, result.data.data() + (withResource ? sizeof resource : 0), desc.privateBytes); checkNt(result.result.ntstatus);
                std::uint32_t alias = result.header.handle;
                if (caps.flags & VendorTranslationCapability) {
                    const auto parent = deviceAdapters.find(a.hDevice);
                    if (parent == deviceAdapters.end()) { transport.fail(); throw Error(EPROTO); }
                    const auto translated = call(request(Op::TranslateVendorAllocation, result.header.handle,
                                                        VendorTranslationDesc{a.hDevice, parent->second, 0, 0}), 0, true);
                    if (translated.header.handle != result.header.handle || translated.result.ntstatus > 0 ||
                        (translated.result.ntstatus >= 0 && (!translated.result.value || translated.result.value > UINT32_MAX)) ||
                        (translated.result.ntstatus < 0 && translated.result.value)) { transport.fail(); throw Error(EPROTO); }
                    if (translated.result.ntstatus < 0) {
                        auto cleanup = request(Op::DestroyVendorAllocations, a.hDevice, DestroyVendorDesc{1, 0});
                        const auto id = reinterpret_cast<const std::uint8_t*>(&result.header.handle);
                        cleanup.insert(cleanup.end(), id, id + 4);
                        const auto released = call(cleanup);
                        if (released.header.handle != a.hDevice || released.result.value) { transport.fail(); throw Error(EPROTO); }
                        checkNt(translated.result.ntstatus);
                    }
                    alias = static_cast<std::uint32_t>(translated.result.value);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationDriverAliasCreated=true typedWireIdentityRetained=true\n");
                }
                if (!vendorOwners.emplace(alias, a.hDevice).second || !vendorWireIds.emplace(alias, result.header.handle).second) {
                    transport.fail(); throw Error(EPROTO);
                }
                if (withResource && !vendorResources.emplace(resource.resource, alias).second) { transport.fail(); throw Error(EPROTO); }
                item.hAllocation = alias; item.GpuVirtualAddress = result.result.value;
                a.hResource = resource.resource; a.hGlobalShare = 0;
                if (sharedResource) sharedResources.insert(resource.resource);
                if (nativeGpuBuffer) guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuBufferDeclared=true bytes=%llu flags=%u guestRuntimePreserved=true\n",
                    static_cast<unsigned long long>(bufferProfile.bytes), bufferProfile.flags);
                if (nativeTexture) guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeTextureDeclared=true width=%u height=%u format=%u flags=%u guestRuntimePreserved=true\n",
                                               profile.width, profile.height, profile.format, profile.flags);
                if(nativeInterop) guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE nativeInteropResource=true threadOwned=true width=%u height=%u format=%u\n",profile.width,profile.height,profile.format);
                if (withResource) guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorResourceCreated=true allocationCount=1 shared=%s systemMemory=false\n", sharedResource ? "true" : "false");
                if (desc.source == UninitializedDisplaySource)
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE standaloneSourceUninitializedAccepted=true primary=false privateDataPreserved=true\n");
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorAllocationCreated=true privateBytes=%u\n", desc.privateBytes); return 0;
            }
            case 19: {
                auto& a = args<D3DKMT_DESTROYALLOCATION2>(requestNumber, pointer);
                if (!(caps.flags & VendorAllocationCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=19 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if (a.Flags.Value & ~3u) throw Error(EINVAL);
                unsigned ownedQueues = 0;
                for (const auto& queue : hwQueueContexts)
                    if (contextOwners.at(queue.second).device == a.hDevice) ++ownedQueues;
                if (ownedQueues && !(caps.flags & VendorRetirementCapability)) throw Error(EBUSY);
                if (a.hResource) {
                    if (!(caps.flags & VendorResourceCapability)) throw Error(ENOSYS);
                    if (a.AllocationCount || a.phAllocationList) throw Error(EINVAL);
                    const auto resource = vendorResources.find(a.hResource);
                    if (resource == vendorResources.end() || vendorOwners.at(resource->second) != a.hDevice) throw Error(EBADF);
                    const auto alias = resource->second;
                    if (cpuLocks.count(alias)) unlockCpu(alias, a.hDevice, true);
                    const auto result = call(request(Op::DestroyVendorResource, a.hResource, a.hDevice), 0, true);
                    if (result.header.handle != a.hResource || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                    checkNt(result.result.ntstatus);
                    sharedResources.erase(a.hResource); vendorResources.erase(resource); vendorOwners.erase(alias); vendorWireIds.erase(alias); vendorGpuRanges.erase(alias);
                    importedVendorAllocations.erase(alias);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorResourceDestroyed=true allocationCount=1 hardwareQueuesAlive=%u\n", ownedQueues);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorAllocationsDestroyed=true count=1 hardwareQueuesAlive=%u\n", ownedQueues); return 0;
                }
                if (!a.AllocationCount || a.AllocationCount > MaxVendorAllocations || !a.phAllocationList) throw Error(EINVAL);
                // Accept the public destruction hints, but always request
                // synchronous native destruction rather than trusting the
                // guest's AssumeNotInUse hint to shorten object lifetime.
                for (unsigned n = 0; n < a.AllocationCount; ++n) {
                    const auto found = vendorOwners.find(a.phAllocationList[n]);
                    if (found == vendorOwners.end() || found->second != a.hDevice) throw Error(EBADF);
                    if (a.AllocationCount != 1 && std::any_of(vendorResources.begin(), vendorResources.end(), [&](const auto& resource) {
                            return resource.second == a.phAllocationList[n]; })) throw Error(EINVAL);
                    for (unsigned previous = 0; previous < n; ++previous)
                        if (a.phAllocationList[previous] == a.phAllocationList[n]) throw Error(EINVAL);
                }
                auto packet = request(Op::DestroyVendorAllocations, a.hDevice, DestroyVendorDesc{a.AllocationCount, 0});
                for (unsigned n = 0; n < a.AllocationCount; ++n)
                    if (cpuLocks.count(a.phAllocationList[n])) unlockCpu(a.phAllocationList[n], a.hDevice, true);
                std::vector<std::uint32_t> wireIds;
                for (unsigned n = 0; n < a.AllocationCount; ++n) wireIds.push_back(wireAllocation(a.phAllocationList[n]));
                const auto begin = reinterpret_cast<const std::uint8_t*>(wireIds.data());
                packet.insert(packet.end(), begin, begin + a.AllocationCount * sizeof(wireIds[0]));
                const auto result = call(packet, 0, true);
                if (result.header.handle != a.hDevice || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                for (unsigned n = 0; n < a.AllocationCount; ++n) {
                    for (auto resource = vendorResources.begin(); resource != vendorResources.end();) {
                        if (resource->second == a.phAllocationList[n]) { sharedResources.erase(resource->first); resource = vendorResources.erase(resource); } else ++resource;
                    }
                    vendorOwners.erase(a.phAllocationList[n]); vendorWireIds.erase(a.phAllocationList[n]); vendorGpuRanges.erase(a.phAllocationList[n]);
                    importedVendorAllocations.erase(a.phAllocationList[n]);
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorAllocationsDestroyed=true count=%u hardwareQueuesAlive=%u\n", a.AllocationCount, ownedQueues); return 0;
            }
            case 11: {
                auto& a = args<D3DDDI_MAKERESIDENT>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE residentInput count=%u flags=%u hasAllocations=%u hasPriorities=%u\n",
                             a.NumAllocations, a.Flags.Value, a.AllocationList ? 1u : 0u, a.PriorityList ? 1u : 0u);
                if (!(caps.flags & VendorResidencyCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=11 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const ResidentDesc desc{a.NumAllocations, a.Flags.Value, a.PriorityList ? 1u : 0u, 0};
                if (!validResident(desc) || !a.AllocationList) throw Error(EINVAL);
                const auto queue = pagingOwners.find(a.hPagingQueue);
                if (queue == pagingOwners.end()) throw Error(EBADF);
                for (unsigned n = 0; n < desc.count; ++n) {
                    const auto allocation = vendorOwners.find(a.AllocationList[n]);
                    if (allocation == vendorOwners.end() || allocation->second != queue->second) throw Error(EBADF);
                    for (unsigned previous = 0; previous < n; ++previous)
                        if (a.AllocationList[previous] == a.AllocationList[n]) throw Error(EINVAL);
                }
                auto packet = request(Op::MakeVendorResident, a.hPagingQueue, desc);
                std::vector<std::uint32_t> wireIds;
                for (unsigned n = 0; n < desc.count; ++n) wireIds.push_back(wireAllocation(a.AllocationList[n]));
                const auto ids = reinterpret_cast<const std::uint8_t*>(wireIds.data());
                packet.insert(packet.end(), ids, ids + desc.count * sizeof(wireIds[0]));
                if (desc.priorities) {
                    const auto priorities = reinterpret_cast<const std::uint8_t*>(a.PriorityList);
                    packet.insert(packet.end(), priorities, priorities + desc.count * sizeof(a.PriorityList[0]));
                }
                const auto result = call(packet, sizeof(ResidentReply), true);
                ResidentReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.header.handle != a.hPagingQueue || output.reserved || output.count > desc.count ||
                    (result.result.ntstatus >= 0 && result.result.ntstatus != 0 && result.result.ntstatus != 0x103)) {
                    transport.fail(); throw Error(EPROTO);
                }
                // Windows returns in/out count, fence and bytes-to-trim on
                // budget failure too. Copy them before translating NTSTATUS.
                a.NumAllocations = output.count; a.PagingFenceValue = result.result.value; a.NumBytesToTrim = output.bytesToTrim;
                checkNt(result.result.ntstatus);
                pagingFences.verifyRetired(a.hPagingQueue, a.PagingFenceValue, "residency");
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorResident=true count=%u status=%d fence=%llu bytesToTrim=%llu\n",
                             a.NumAllocations, result.result.ntstatus, static_cast<unsigned long long>(a.PagingFenceValue),
                             static_cast<unsigned long long>(a.NumBytesToTrim));
                return result.result.ntstatus;
            }
            case 8: {
                auto& a = args<D3DDDI_RESERVEGPUVIRTUALADDRESS>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE reserveGpuVaInput ownedAdapter=%u ownedPagingQueue=%u base=%llu minimum=%llu maximum=%llu bytes=%llu reserved0=%u reserved1=%llu reserved2=%llu\n",
                    ownedAdapters.count(a.hAdapter) ? 1u : 0u,
                    pagingOwners.count(a.hPagingQueue) ? 1u : 0u,
                    static_cast<unsigned long long>(a.BaseAddress), static_cast<unsigned long long>(a.MinimumAddress),
                    static_cast<unsigned long long>(a.MaximumAddress), static_cast<unsigned long long>(a.Size), a.Reserved0,
                    static_cast<unsigned long long>(a.Reserved1), static_cast<unsigned long long>(a.Reserved2));
                if (!(caps.flags & GpuReservationCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=8 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const GpuReservationDesc desc{a.BaseAddress, a.MinimumAddress, a.MaximumAddress, a.Size};
                if (a.Reserved0 || a.Reserved1 || a.Reserved2 || !validGpuReservation(desc)) throw Error(EINVAL);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                if (gpuReservations.size() >= reservationLimit() ||
                    desc.bytes > MaxGpuReservedBytes - gpuReservedBytes) throw Error(EMFILE);
                if (desc.base) for (const auto& range : gpuReservations)
                    if (gpuRangesOverlap(desc.base, desc.bytes, range.second.address, range.second.bytes)) throw Error(EBUSY);
                const auto result = call(request(Op::ReserveGpuAddress, a.hAdapter, desc), 0, true);
                if ((result.result.ntstatus >= 0 && (result.result.ntstatus != 0 || !result.header.handle ||
                        gpuReservations.count(result.header.handle) || !validGpuReservationOutput(desc, result.result.value))) ||
                    (result.result.ntstatus < 0 && (result.header.handle || result.result.value))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                for (const auto& range : gpuReservations)
                    if (gpuRangesOverlap(result.result.value, desc.bytes, range.second.address, range.second.bytes)) {
                        transport.fail(); throw Error(EPROTO);
                    }
                try { gpuReservations.emplace(result.header.handle, GpuReservation{a.hAdapter, result.result.value, desc.bytes}); }
                catch (...) { transport.fail(); throw; } // Host retains ownership until its VM has exited.
                gpuReservedBytes += desc.bytes; a.VirtualAddress = result.result.value;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuReserved=true bytes=%llu\n", static_cast<unsigned long long>(desc.bytes));
                const auto eofTest = std::getenv("WDDM_BRIDGE_RESERVATION_EOF_TEST");
                if (eofTest && !std::strcmp(eofTest, "1")) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE reservationEofTest=true exitingWithGpuReservationOwned=true\n"); _exit(1);
                }
                return 0;
            }
            case 32: {
                const auto& a = args<D3DKMT_FREEGPUVIRTUALADDRESS>(requestNumber, pointer);
                if (!(caps.flags & GpuReservationCapability)) throw Error(ENOSYS);
                if (!a.BaseAddress || a.BaseAddress % 4096 || !a.Size || a.Size % 4096 ||
                    a.Size > MaxGpuAddress || a.BaseAddress > MaxGpuAddress - a.Size) throw Error(EINVAL);
                if (!ownedAdapters.count(a.hAdapter)) throw Error(EBADF);
                const auto range = std::find_if(gpuReservations.begin(), gpuReservations.end(), [&a](const auto& item) {
                    return item.second.adapter == a.hAdapter && item.second.address == a.BaseAddress && item.second.bytes == a.Size;
                });
                if (range == gpuReservations.end()) throw Error(EBADF);
                for (const auto& mapped : vendorGpuRanges)
                    if (gpuRangesOverlap(range->second.address, range->second.bytes, mapped.second.address, mapped.second.bytes) &&
                        !gpuRangeContains(range->second.address, range->second.bytes, mapped.second.address, mapped.second.bytes)) throw Error(EBUSY);
                const auto result = call(request(Op::FreeGpuReservation, range->first, FreeGpuReservationDesc{a.hAdapter, 0}), 0, true);
                if (result.header.handle != range->first || result.result.ntstatus > 0 || result.result.value) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                for (auto mapped = vendorGpuRanges.begin(); mapped != vendorGpuRanges.end();)
                    if (gpuRangesOverlap(range->second.address, range->second.bytes, mapped->second.address, mapped->second.bytes)) mapped = vendorGpuRanges.erase(mapped);
                    else ++mapped;
                gpuStates.release(range->first); gpuReservedBytes -= range->second.bytes; gpuReservations.erase(range);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuReservationFreed=true bytes=%llu\n", static_cast<unsigned long long>(a.Size));
                return 0;
            }
            case 12: {
                auto& a = args<D3DDDI_MAPGPUVIRTUALADDRESS>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE gpuVaAllocationInput allocationIsNull=%u ownedAllocation=%u\n",
                    a.hAllocation ? 0u : 1u, vendorOwners.count(a.hAllocation) ? 1u : 0u);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE gpuVaInput base=%llu minimum=%llu maximum=%llu offsetPages=%llu sizePages=%llu protection=%llu driverProtection=%llu reserved0=%u reserved1=%llu\n",
                    static_cast<unsigned long long>(a.BaseAddress), static_cast<unsigned long long>(a.MinimumAddress),
                    static_cast<unsigned long long>(a.MaximumAddress), static_cast<unsigned long long>(a.OffsetInPages),
                    static_cast<unsigned long long>(a.SizeInPages), static_cast<unsigned long long>(a.Protection.Value),
                    static_cast<unsigned long long>(a.DriverProtection), a.Reserved0, static_cast<unsigned long long>(a.Reserved1));
                if (!(caps.flags & VendorGpuVaCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=12 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const GpuVaDesc desc{a.hPagingQueue, a.Reserved0, a.BaseAddress, a.MinimumAddress, a.MaximumAddress,
                                     a.OffsetInPages, a.SizeInPages, a.Protection.Value, a.DriverProtection};
                if (a.Protection.Value & 12) {
                    if (!(caps.flags & GpuStateCapability)) {
                        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=12 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                    }
                    if (a.hAllocation || a.Reserved1 || !validGpuState(desc)) throw Error(EINVAL);
                    const auto queue = pagingOwners.find(a.hPagingQueue);
                    if (queue == pagingOwners.end() || !deviceAdapters.count(queue->second)) throw Error(EBADF);
                    const auto adapter = deviceAdapters.at(queue->second);
                    const auto range = std::find_if(gpuReservations.begin(), gpuReservations.end(), [&](const auto& item) {
                        return item.second.adapter == adapter && gpuRangeContains(item.second.address, item.second.bytes, desc.base, desc.sizePages * 4096);
                    });
                    if (range == gpuReservations.end()) throw Error(EBADF);
                    for (const auto& mapped : vendorGpuRanges)
                        if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped.second.address, mapped.second.bytes) &&
                            (vendorOwners.at(mapped.first) != queue->second || !gpuRangeContains(desc.base, desc.sizePages * 4096, mapped.second.address, mapped.second.bytes))) throw Error(EBUSY);
                    GpuStateRanges::Plan plan;
                    if (!gpuStates.prepare(range->first, desc.base, desc.sizePages * 4096, desc.protection, plan)) throw Error(EMFILE);
                    const auto result = call(request(Op::MapGpuState, range->first, desc), sizeof(GpuVaReply), true);
                    GpuVaReply fence{}; std::memcpy(&fence, result.data.data(), sizeof fence);
                    if (result.header.handle != range->first || (result.result.ntstatus >= 0 &&
                        ((result.result.ntstatus != 0 && result.result.ntstatus != 259) || result.result.value != desc.base)) ||
                        (result.result.ntstatus < 0 && (result.result.value || fence.fence))) { transport.fail(); throw Error(EPROTO); }
                    checkNt(result.result.ntstatus); pagingFences.verifyRetired(a.hPagingQueue, fence.fence, "gpu-state");
                    for (auto mapped = vendorGpuRanges.begin(); mapped != vendorGpuRanges.end();)
                        if (gpuRangesOverlap(desc.base, desc.sizePages * 4096, mapped->second.address, mapped->second.bytes)) mapped = vendorGpuRanges.erase(mapped);
                        else ++mapped;
                    gpuStates.commit(plan); a.VirtualAddress = result.result.value; a.PagingFenceValue = fence.fence;
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuStateMapped=true pages=%llu protection=%llu status=%d fence=%llu allocationIsNull=true\n",
                        static_cast<unsigned long long>(desc.sizePages), static_cast<unsigned long long>(desc.protection), result.result.ntstatus,
                        static_cast<unsigned long long>(fence.fence));
                    const auto eofTest = std::getenv("WDDM_BRIDGE_GPU_STATE_EOF_TEST");
                    if (eofTest && !std::strcmp(eofTest, "1")) {
                        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE gpuStateEofTest=true exitingWithGpuStateOwned=true\n"); _exit(1);
                    }
                    return result.result.ntstatus;
                }
                if (a.Reserved1 || !validImportedGpuVa(desc, importedVendorAllocations.count(a.hAllocation) != 0)) throw Error(EINVAL);
                const auto allocation = vendorOwners.find(a.hAllocation), queue = pagingOwners.find(a.hPagingQueue);
                if (allocation == vendorOwners.end() || queue == pagingOwners.end() || allocation->second != queue->second) throw Error(EBADF);
                if (desc.driverProtection && !std::any_of(vendorResources.begin(), vendorResources.end(), [&](const auto& resource) {
                    return resource.second == a.hAllocation; })) throw Error(EINVAL);
                const auto wire = wireAllocation(a.hAllocation);
                GpuStateRanges::Plan statePlan;
                if (desc.base && !gpuStates.prepareRemove(desc.base, desc.sizePages * 4096, statePlan)) throw Error(EMFILE);
                const auto result = call(request(Op::MapVendorAllocation, wire, desc), sizeof(GpuVaReply), true);
                GpuVaReply fence{}; std::memcpy(&fence, result.data.data(), sizeof fence);
                if (result.header.handle != wire ||
                    (result.result.ntstatus >= 0 && ((result.result.ntstatus != 0 && result.result.ntstatus != 0x103) ||
                        !validImportedGpuVaOutput(desc, result.result.value, importedVendorAllocations.count(a.hAllocation) != 0))) ||
                    (result.result.ntstatus < 0 && (result.result.value || fence.fence))) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                pagingFences.verifyRetired(a.hPagingQueue, fence.fence, "gpuva");
                if (!desc.base && !gpuStates.prepareRemove(result.result.value, desc.sizePages * 4096, statePlan)) { transport.fail(); throw Error(EIO); }
                gpuStates.commit(statePlan);
                vendorGpuRanges.emplace(a.hAllocation, VendorGpuRange{result.result.value, desc.sizePages * 4096});
                a.VirtualAddress = result.result.value; a.PagingFenceValue = fence.fence;
                if (desc.driverProtection) guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorDriverProtectionMapped=true value=%llu resourceOwned=true\n",
                    static_cast<unsigned long long>(desc.driverProtection));
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorGpuVaMapped=true pages=%llu status=%d fence=%llu\n",
                             static_cast<unsigned long long>(desc.sizePages), result.result.ntstatus, static_cast<unsigned long long>(fence.fence));
                return result.result.ntstatus;
            }
            case 37: {
                auto& a = args<D3DKMT_LOCK2>(requestNumber, pointer);
                const auto allocation = vendorOwners.find(a.hAllocation);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE lock2Input flags=%u ownedAllocation=%u hasInputData=%u\n",
                             a.Flags.Value, allocation != vendorOwners.end() && allocation->second == a.hDevice ? 1u : 0u,
                             a.pData ? 1u : 0u);
                if (!(caps.flags & VendorCpuCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=37 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if (a.Flags.Value) throw Error(EINVAL);
                if (allocation == vendorOwners.end() || allocation->second != a.hDevice) throw Error(EBADF);
                if (allocationMemory.contains(a.hAllocation)) {
                    a.pData = allocationMemory.retain(a.hAllocation); return 0;
                }
                if (cpuLocks.count(a.hAllocation)) throw Error(EBUSY);
                const auto wire = wireAllocation(a.hAllocation);
                const auto result = call(request(Op::LockVendorAllocation, wire, VendorCpuDesc{a.hDevice, 0}), sizeof(VendorCpuReply), true);
                VendorCpuReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.header.handle != wire || result.result.ntstatus > 0 ||
                    (result.result.ntstatus >= 0 && !validVendorCpuReply(result.result.value, output,sparseApertureNegotiated,desktopSlotsNegotiated)) ||
                    (result.result.ntstatus < 0 && (result.result.value || output.bytes || output.reserved || output.generation))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                try {
                    cpuLocks.insert(a.hAllocation);
                    a.pData = allocationMemory.map(a.hAllocation, result.result.value, output);
                } catch (...) { transport.fail(); throw; }
                const auto referenceTest = std::getenv("WDDM_BRIDGE_CPU_REFERENCE_TEST");
                if (referenceTest && !std::strcmp(referenceTest, "1")) {
                    D3DKMT_LOCK2 repeated = a;
                    D3DKMT_UNLOCK2 release{}; release.hDevice = a.hDevice; release.hAllocation = a.hAllocation;
                    if (dispatch(_IOWR('G', 37, D3DKMT_LOCK2), &repeated) || repeated.pData != a.pData ||
                        dispatch(_IOWR('G', 55, D3DKMT_UNLOCK2), &release) || !allocationMemory.contains(a.hAllocation)) {
                        transport.fail(); throw Error(EPROTO);
                    }
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuReferenceTest=true sameGuestPointer=true intermediateUnlockRetained=true\n");
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeVendorCpuLocked=true bytes=%u\n", output.bytes);
                const auto spanEofTest = std::getenv("WDDM_BRIDGE_CPU_SPAN_EOF_TEST");
                if (spanEofTest && !std::strcmp(spanEofTest, "1") && output.bytes > VendorCpuSlotBytes) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuSpanEofTest=true exitingWithSpanOwned=true\n"); _exit(1);
                }
                const auto eofTest = std::getenv("WDDM_BRIDGE_CPU_EOF_TEST");
                if (eofTest && !std::strcmp(eofTest, "1")) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE allocationCpuEofTest=true exitingWithLockOwned=true\n");
                    _exit(1); // Deliberately bypass C++/UMD teardown in this private diagnostic.
                }
                return 0;
            }
            case 55: {
                auto& a = args<D3DKMT_UNLOCK2>(requestNumber, pointer);
                if (!(caps.flags & VendorCpuCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=55 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const auto allocation = vendorOwners.find(a.hAllocation);
                if (allocation == vendorOwners.end() || allocation->second != a.hDevice) throw Error(EBADF);
                unlockCpu(a.hAllocation, a.hDevice); return 0;
            }
            case 13: {
                auto& a = args<D3DKMT_ESCAPE>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE escapeInput type=%u flags=%u privateBytes=%u hasContext=%u\n",
                             static_cast<unsigned>(a.Type), a.Flags.Value, a.PrivateDriverDataSize, a.hContext ? 1u : 0u);
                if (a.Type == D3DKMT_ESCAPE_DRIVERPRIVATE && a.Flags.Value == 64 && a.pPrivateDriverData &&
                    a.PrivateDriverDataSize == sizeof(D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE)) {
                    D3DDDI_DRIVERESCAPE_TRANSLATEALLOCATIONEHANDLE known{}; std::memcpy(&known, a.pPrivateDriverData, sizeof known);
                    const auto allocation = vendorOwners.find(known.hAllocation);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE knownEscapeInput type=%u ownedAllocation=%u\n",
                                 static_cast<unsigned>(known.EscapeType), allocation != vendorOwners.end() && allocation->second == a.hDevice ? 1u : 0u);
                    if ((caps.flags & VendorTranslationCapability) && known.EscapeType == D3DDDI_DRIVERESCAPETYPE_TRANSLATEALLOCATIONHANDLE) {
                        if (a.hContext) throw Error(EINVAL);
                        const auto parent = deviceAdapters.find(a.hDevice);
                        if (allocation == vendorOwners.end() || allocation->second != a.hDevice || parent == deviceAdapters.end() || parent->second != a.hAdapter)
                            throw Error(EBADF);
                        // Allocation aliases already carry the documented driver
                        // token. This known escape is a validated identity operation,
                        // matching WSL's host/guest token contract. No private rewrite.
                        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE knownAllocationTranslation=true aliasAlreadyTranslated=true\n"); return 0;
                    }
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=13 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
            }
            case 24: {
                auto& a = args<D3DKMT_CREATEHWQUEUE>(requestNumber, pointer);
                const HwQueueDesc desc{a.Flags.Value, a.PrivateDriverDataSize, 0, 0};
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE hwQueueDescriptor flags=%u privateBytes=%u hasPrivateData=%u\n",
                             desc.flags, desc.privateBytes, a.pPrivateDriverData ? 1u : 0u);
                if (!validHwQueue(desc) || !a.pPrivateDriverData) throw Error(EINVAL);
                unsigned allocationReferences = 0;
                if (a.pPrivateDriverData && a.PrivateDriverDataSize <= MaxContextPrivateBytes) {
                    for (std::size_t offset = 0; offset + 4 <= a.PrivateDriverDataSize; offset += 4) {
                        std::uint32_t value{}; std::memcpy(&value, static_cast<const char*>(a.pPrivateDriverData) + offset, sizeof value);
                        if (vendorOwners.count(value)) ++allocationReferences;
                    }
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE hwQueueInput flags=%u privateBytes=%u hasPrivateData=%u guestAllocationReferenceMatches=%u\n",
                             a.Flags.Value, a.PrivateDriverDataSize, a.pPrivateDriverData ? 1u : 0u, allocationReferences);
                if (!(caps.flags & HwQueueCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=24 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if ((desc.flags & NoBroadcastSignalHwQueueFlag) && !(caps.flags & NoBroadcastSignalHwQueueCapability)) throw Error(ENOSYS);
                if ((desc.flags & NoBroadcastWaitHwQueueFlag) && !(caps.flags & NoBroadcastWaitHwQueueCapability)) throw Error(ENOSYS);
                const auto parent = contextOwners.find(a.hHwContext);
                if (parent == contextOwners.end()) throw Error(EBADF);
                if (parent->second.flags != 16) throw Error(ENOSYS);
                if (hwQueueContexts.size() >= (openglFencesNegotiated ? OpenGlFenceHwQueues : MaxHwQueues)) throw Error(EMFILE);
                auto packet = request(Op::CreateHwQueue, a.hHwContext, desc);
                const auto begin = static_cast<const std::uint8_t*>(a.pPrivateDriverData);
                packet.insert(packet.end(), begin, begin + desc.privateBytes);
                const auto result = call(packet, sizeof(HwQueueReply) + desc.privateBytes, true);
                HwQueueReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.result.ntstatus > 0 || result.result.value || output.reserved ||
                    (result.result.ntstatus == 0 && (!result.header.handle || hwQueueContexts.count(result.header.handle) ||
                        !output.sync || output.sync == result.header.handle || output.offset % 8 || output.offset >= fenceApertureBytes ||
                        !output.gpuAddress || output.gpuAddress % 8 || output.gpuAddress >= MaxGpuAddress)) ||
                    (result.result.ntstatus < 0 && (result.header.handle || output.sync || output.offset || output.gpuAddress))) {
                    transport.fail(); throw Error(EPROTO);
                }
                std::memcpy(a.pPrivateDriverData, result.data.data() + sizeof output, desc.privateBytes);
                checkNt(result.result.ntstatus);
                void* fence = nullptr;
                try { fence = hwQueueFences.map(result.header.handle, output.offset, "hwQueue"); }
                catch (...) { destroy(Op::DestroyHwQueue, result.header.handle); throw; }
                if (!hwQueueContexts.emplace(result.header.handle, a.hHwContext).second) { transport.fail(); throw Error(EPROTO); }
                if (!borrowedFences.emplace(output.sync, BorrowedFence{result.header.handle, parent->second.device, true}).second) { transport.fail(); throw Error(EPROTO); }
                a.hHwQueue = result.header.handle; a.hHwQueueProgressFence = output.sync;
                a.HwQueueProgressFenceCPUVirtualAddress = fence; a.HwQueueProgressFenceGPUVirtualAddress = output.gpuAddress;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeHwQueueCreated=true privateBytes=%u progressFenceDirect=true flags=%u\n", desc.privateBytes, desc.flags);
                const auto eofTest = std::getenv("WDDM_BRIDGE_HWQUEUE_EOF_TEST");
                if (eofTest && (std::strcmp(eofTest, "1") == 0 ||
                    (std::strcmp(eofTest, "2") == 0 && desc.flags == NoBroadcastSignalHwQueueFlag) ||
                    (std::strcmp(eofTest, "3") == 0 && desc.flags == InternalNoBroadcastHwQueueFlags))) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags=%u\n", desc.flags);
                    _exit(1);
                }
                return 0;
            }
            case 16: {
                auto& a = args<D3DKMT_CREATESYNCHRONIZATIONOBJECT2>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE synchronizationInput type=%u flags=%u ownedDevice=%u hasSharedInput=%u\n",
                             static_cast<unsigned>(a.Info.Type), a.Info.Flags.Value, deviceAdapters.count(a.hDevice) ? 1u : 0u,
                             a.Info.SharedHandle ? 1u : 0u);
                if (a.Info.Type == D3DDDI_MONITORED_FENCE)
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE monitoredFenceInput initial=%llu affinity=%u padding=%u hasCpuInput=%u hasGpuInput=%u\n",
                                 static_cast<unsigned long long>(a.Info.MonitoredFence.InitialFenceValue), a.Info.MonitoredFence.EngineAffinity,
                                 a.Info.MonitoredFence.Padding, a.Info.MonitoredFence.FenceValueCPUVirtualAddress ? 1u : 0u,
                                 a.Info.MonitoredFence.FenceValueGPUVirtualAddress ? 1u : 0u);
                if (a.Info.Type == D3DDDI_CPU_NOTIFICATION)
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE cpuNotificationInput hasEvent=%u\n", a.Info.CPUNotification.Event ? 1u : 0u);
                if (!(caps.flags & SyncCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=16 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                SyncDesc desc{static_cast<unsigned>(a.Info.Type), a.Info.Flags.Value, 0, 0, 0};
                if (desc.type == 1) desc.initial = static_cast<std::uint64_t>(a.Info.SynchronizationMutex.InitialState);
                else if (desc.type == 5) {
                    desc.initial = a.Info.MonitoredFence.InitialFenceValue; desc.affinity = a.Info.MonitoredFence.EngineAffinity;
                    if (a.Info.MonitoredFence.Padding) throw Error(EINVAL);
                } else throw Error(ENOSYS);
                if (!validSync(desc) || a.Info.SharedHandle) throw Error(EINVAL);
                if (desc.flags == NtSharedGpuSyncFlags) beginSharedFences();
                if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                if (syncObjects.size() >= syncObjectLimit()) throw Error(EMFILE);
                const auto result = call(request(Op::CreateSync, a.hDevice, desc), sizeof(SyncReply), true);
                SyncReply output{}; std::memcpy(&output, result.data.data(), sizeof output);
                if (result.result.ntstatus > 0 || result.result.value ||
                    (result.result.ntstatus == 0 && (!result.header.handle || syncObjects.count(result.header.handle) ||
                        (desc.type == 1 && (output.offset || output.gpuAddress)) ||
                        (desc.type == 5 && (output.offset % 8 || output.offset >= fenceApertureBytes ||
                            !validSyncGpuAddress(desc, output.gpuAddress))))) ||
                    (result.result.ntstatus < 0 && (result.header.handle || output.offset || output.gpuAddress))) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                void* fence = nullptr;
                if (desc.type == 5) {
                    try { fence = syncFences.map(result.header.handle, output.offset, "monitored"); }
                    catch (...) { destroy(Op::DestroySync, result.header.handle); throw; }
                }
                if (!syncObjects.emplace(result.header.handle, Synchronization{a.hDevice, desc.type, desc.flags, desc.initial}).second) {
                    transport.fail(); throw Error(EPROTO);
                }
                a.hSyncObject = result.header.handle; a.Info.SharedHandle = 0;
                if (desc.type == 5) {
                    a.Info.MonitoredFence.FenceValueCPUVirtualAddress = fence;
                    a.Info.MonitoredFence.FenceValueGPUVirtualAddress = output.gpuAddress;
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeSynchronizationCreated=true type=%u flags=%u gpuMapped=%u\n",
                             desc.type, desc.flags, output.gpuAddress ? 1u : 0u);
                const auto noMaxEofTest = std::getenv("WDDM_BRIDGE_SYNC_NO_MAX_EOF_TEST");
                if (desc.flags == NoSignalMaxValueOnTdrSyncFlag && noMaxEofTest && std::strcmp(noMaxEofTest, "1") == 0) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE syncNoMaxEofTest=true exitingWithNoMaxFenceOwned=true\n"); _exit(1);
                }
                const auto eofTest = std::getenv("WDDM_BRIDGE_SYNC_EOF_TEST");
                if (desc.flags == NoGpuAccessSyncFlag && eofTest && std::strcmp(eofTest, "1") == 0) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE syncEofTest=true exitingWithNoGpuAccessFenceOwned=true\n");
                    _exit(1);
                }
                return 0;
            }
            case 47: {
                const auto& a = args<D3DKMT_SETCONTEXTINPROCESSSCHEDULINGPRIORITY>(requestNumber, pointer);
                const auto owner = contextOwners.find(a.hContext);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE contextPriorityInput priority=%d ownedContext=%u inProcessOnly=true\n", a.Priority, owner != contextOwners.end() ? 1u : 0u);
                if (!(caps.flags & ContextCapability)) throw Error(ENOSYS);
                if (!validContextPriority(a.Priority)) throw Error(EINVAL);
                if (owner == contextOwners.end()) throw Error(EBADF);
                if (owner->second.flags != 16) throw Error(ENOSYS);
                const auto result = call(request(Op::SetContextInProcessPriority, a.hContext, static_cast<std::int32_t>(a.Priority)), 0, true);
                if (result.header.handle != a.hContext || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeContextPriorityChanged=true priority=%d inProcessOnly=true\n", a.Priority);
                return 0;
            }
            case 58: {
                const auto& a = args<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE cpuFenceWaitInput count=%u flags=%u asyncEvent=%u ownedDevice=%u hasObjects=%u hasValues=%u\n",
                    a.ObjectCount, a.Flags.Value, a.hAsyncEvent ? 1u : 0u, deviceAdapters.count(a.hDevice) ? 1u : 0u,
                    a.ObjectHandleArray ? 1u : 0u, a.FenceValueArray ? 1u : 0u);
                if (guest_async::Enabled && a.hAsyncEvent) {
                    if (!a.ObjectCount || a.ObjectCount > WDDM_FENCE_EVENTFD_MAX_POINTS ||
                        (a.Flags.Value & ~WDDM_FENCE_EVENTFD_WAIT_ANY) ||
                        !a.ObjectHandleArray || !a.FenceValueArray) throw Error(EINVAL);
                    if (!deviceAdapters.count(a.hDevice)) throw Error(EBADF);
                    try {
                        std::vector<wddm_fence_eventfd_point> points;
                        points.reserve(a.ObjectCount);
                        for (unsigned index = 0; index < a.ObjectCount; ++index) {
                            const auto id = a.ObjectHandleArray[index];
                            const auto target = a.FenceValueArray[index];
                            if (target == UINT64_MAX) throw Error(EINVAL);
                            const auto borrowed = borrowedFences.find(id);
                            guest_paging::Fences* source = nullptr;
                            std::uint32_t owner = id;
                            if (borrowed != borrowedFences.end()) {
                                if (borrowed->second.device != a.hDevice) throw Error(EBADF);
                                owner = borrowed->second.owner;
                                source = borrowed->second.hardware ? &hwQueueFences : &pagingFences;
                            } else {
                                const auto sync = syncObjects.find(id);
                                if (sync == syncObjects.end() || sync->second.device != a.hDevice ||
                                    sync->second.type != 5) throw Error(EBADF);
                                source = &syncFences;
                            }
                            points.push_back(source->eventPoint(owner, target));
                        }
                        guest_async::submit(points, reinterpret_cast<std::uintptr_t>(a.hAsyncEvent), a.Flags.Value);
                    } catch (const std::system_error& error) { throw Error(error.code().value()); }
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=%u any=%u kernelEventfd=true\n",
                        a.ObjectCount, a.Flags.Value ? 1u : 0u);
                    return 0;
                }
                if (a.hAsyncEvent || a.ObjectCount != 1 || a.Flags.Value) throw Error(ENOSYS);
                if (!a.ObjectHandleArray || !a.FenceValueArray || a.FenceValueArray[0] == UINT64_MAX) throw Error(EINVAL);
                const auto id = a.ObjectHandleArray[0]; const auto target = a.FenceValueArray[0];
                const auto borrowed = borrowedFences.find(id);
                guest_paging::Fences* fences = nullptr; std::uint32_t owner = id; const char* category = "independent";
                if (borrowed != borrowedFences.end()) {
                    if (borrowed->second.device != a.hDevice) throw Error(EBADF);
                    owner = borrowed->second.owner;
                    fences = borrowed->second.hardware ? &hwQueueFences : &pagingFences;
                    category = borrowed->second.hardware ? "hardware" : "paging";
                } else {
                    const auto sync = syncObjects.find(id);
                    if (sync == syncObjects.end() || sync->second.device != a.hDevice || sync->second.type != 5) throw Error(EBADF);
                    fences = &syncFences;
                }
                if (!fences->waitRetired(owner, target, category)) throw Error(EAGAIN);
                return 0;
            }
            case 51: {
                const auto& a = args<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2>(requestNumber, pointer);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE contextSignalInput objects=%u flags=%u contexts=%u hasObjects=%u hasContexts=%u hasValues=%u\n",
                    a.ObjectCount, a.Flags.Value, a.BroadcastContextCount, a.ObjectHandleArray ? 1u : 0u,
                    a.BroadcastContextArray ? 1u : 0u, a.MonitoredFenceValueArray ? 1u : 0u);
                if (!(caps.flags & ContextSignalCapability) || a.ObjectCount != 1 || a.BroadcastContextCount != 1 || a.Flags.Value != 4)
                    throw Error(ENOSYS);
                if (!a.ObjectHandleArray || !a.BroadcastContextArray || !a.MonitoredFenceValueArray ||
                    std::any_of(std::begin(a.Reserved) + 1, std::end(a.Reserved), [](auto value) { return value != 0; })) throw Error(EINVAL);
                if (a.ObjectCount == 1 && a.ObjectHandleArray && a.MonitoredFenceValueArray) {
                    const auto sync = syncObjects.find(a.ObjectHandleArray[0]);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE contextSignalObject owned=%u type=%u flags=%u value=%llu\n", sync != syncObjects.end() ? 1u : 0u,
                        sync == syncObjects.end() ? 0u : sync->second.type, sync == syncObjects.end() ? 0u : sync->second.flags,
                        static_cast<unsigned long long>(a.MonitoredFenceValueArray[0]));
                }
                if (a.BroadcastContextCount == 1 && a.BroadcastContextArray) {
                    const auto context = contextOwners.find(a.BroadcastContextArray[0]);
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE contextSignalContext owned=%u flags=%u\n", context != contextOwners.end() ? 1u : 0u,
                        context == contextOwners.end() ? 0u : context->second.flags);
                }
                const ContextSignalDesc desc{a.ObjectHandleArray[0],a.Flags.Value,a.MonitoredFenceValueArray[0]};
                if (!validContextSignal(desc)) throw Error(EINVAL);
                const auto context = contextOwners.find(a.BroadcastContextArray[0]); const auto sync = syncObjects.find(a.ObjectHandleArray[0]);
                if (context == contextOwners.end() || !contextCanSignal(context->second.flags) || sync == syncObjects.end() ||
                    sync->second.type != 5 || !packetSignalSyncFlags(sync->second.flags) || sync->second.device != context->second.device) throw Error(EBADF);
                if (desc.fence <= sync->second.lastSignal) throw Error(EINVAL);
                const bool asynchronous = sharedFencesNegotiated && (caps.flags & AsyncHwSubmitCapability);
                const auto result = call(request(asynchronous ? Op::QueueContextSync : Op::SignalContextSync, context->first, desc), 0, true);
                if (result.header.handle != context->first || result.result.ntstatus > 0 ||
                    (result.result.ntstatus == 0 && ((!asynchronous && result.result.value < desc.fence) || result.result.value == UINT64_MAX)) ||
                    (result.result.ntstatus < 0 && result.result.value)) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                if (!asynchronous) syncFences.verifyRetired(sync->first, desc.fence, "context-signal");
                sync->second.lastSignal = desc.fence;
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeContextSignalSubmitted=true flags=%u target=%llu observed=%llu noGpuAccess=true asynchronous=%u\n",
                    desc.flags,static_cast<unsigned long long>(desc.fence),static_cast<unsigned long long>(result.result.value), asynchronous ? 1u : 0u);
                return 0;
            }
            case 54: case 59: {
                const bool contextStream = nr == 59;
                std::uint32_t owner = 0, count = 0;
                const D3DKMT_HANDLE* objects = nullptr; const UINT64* values = nullptr;
                if (contextStream) {
                    const auto& a = args<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU>(requestNumber, pointer);
                    owner = a.hContext; count = a.ObjectCount; objects = a.ObjectHandleArray; values = a.MonitoredFenceValueArray;
                    // The pointer arm is active. Do not interpret inactive
                    // union bytes; the native structure is rebuilt as zeroed.
                } else {
                    const auto& a = args<D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE>(requestNumber, pointer);
                    owner = a.hHwQueue; count = a.ObjectCount; objects = a.ObjectHandleArray; values = a.FenceValueArray;
                }
                const auto required = (contextStream ? ContextCapability : HwQueueCapability) | AsyncHwSubmitCapability | SyncCapability;
                if ((caps.flags & required) != required) throw Error(ENOSYS);
                if (!count || count > MaxHwQueueWaitObjects || !objects || !values)
                    throw Error(EINVAL);
                const auto queue = hwQueueContexts.find(owner);
                const auto context = contextOwners.find(contextStream ? owner : queue == hwQueueContexts.end() ? 0 : queue->second);
                const auto firstBorrowed=borrowedFences.find(objects[0]);
                if(coordinatedProfileNegotiated) guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE gpuWaitOwnership contextStream=%u owner=%u count=%u knownContext=%u knownQueue=%u firstSync=%u firstSyncKnown=%u firstSyncBorrowed=%u borrowedHardware=%u sourceDevice=%u targetDevice=%u contextFlags=%u\n",
                    contextStream ? 1u : 0u,owner,count,context!=contextOwners.end() ? 1u : 0u,queue!=hwQueueContexts.end() ? 1u : 0u,objects[0],syncObjects.count(objects[0]) ? 1u : 0u,firstBorrowed!=borrowedFences.end() ? 1u : 0u,
                    firstBorrowed!=borrowedFences.end() && firstBorrowed->second.hardware ? 1u : 0u,firstBorrowed==borrowedFences.end() ? 0u : firstBorrowed->second.device,context==contextOwners.end() ? 0u : context->second.device,context==contextOwners.end() ? UINT32_MAX : context->second.flags);
                if (context == contextOwners.end() || !contextCanWait(context->second.flags,coordinatedProfileNegotiated)) throw Error(EBADF);
                const auto device = context->second.device;
                std::vector<HwQueueWaitItem> items; items.reserve(count);
                for (std::uint32_t n = 0; n < count; ++n) {
                    const auto sync = syncObjects.find(objects[n]);
                    const auto paging=borrowedFences.find(objects[n]);
                    if (values[n] == UINT64_MAX) throw Error(EINVAL);
                    const bool independent=sync!=syncObjects.end() && sync->second.type==5 && sync->second.device==device;
                    const bool borrowed=coordinatedProfileNegotiated && paging!=borrowedFences.end() && paging->second.device==device;
                    if(!independent && !borrowed) throw Error(EBADF);
                    for (const auto& previous : items) if (previous.sync == objects[n]) throw Error(EINVAL);
                    items.push_back({objects[n], 0, values[n]});
                }
                beginSharedFences();
                auto packet = request(contextStream ? Op::QueueContextWait : Op::QueueHwQueueWait, owner, HwQueueWaitDesc{count, 0});
                const auto bytes = reinterpret_cast<const std::uint8_t*>(items.data());
                packet.insert(packet.end(), bytes, bytes + items.size() * sizeof(items[0]));
                const auto result = call(packet, 0, true);
                if (result.header.handle != owner || result.result.ntstatus > 0 || result.result.value) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuQueueWaitQueued=true objects=%u firstTarget=%llu contextStream=%u cpuWait=false\n",
                    count, static_cast<unsigned long long>(items[0].fence), contextStream ? 1u : 0u);
                return 0;
            }
            case 29: {
                auto& a = args<D3DKMT_DESTROYSYNCHRONIZATIONOBJECT>(requestNumber, pointer);
                if (!(caps.flags & SyncCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=29 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const auto owned = syncObjects.find(a.hSyncObject);
                // Paging and queue progress synchronization handles are borrowed.
                // Only independent synchronization objects reach this native destroy.
                if (owned == syncObjects.end()) throw Error(EBADF);
                if (syncFences.contains(a.hSyncObject)) syncFences.unmap(a.hSyncObject);
                const auto result = call(request(Op::DestroySync, a.hSyncObject), 0, true);
                if (result.header.handle != a.hSyncObject || result.result.value || result.result.ntstatus > 0) { transport.fail(); throw Error(EPROTO); }
                checkNt(result.result.ntstatus);
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeSynchronizationDestroyed=true type=%u\n", owned->second.type);
                importedShareableSyncs.erase(a.hSyncObject); syncObjects.erase(owned); return 0;
            }
            case 53: {
                const auto& a=args<D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE>(requestNumber,pointer);
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE hwQueueSignalInput objects=%u flags=%u queues=%u hasObjects=%u hasQueues=%u hasValues=%u\n",
                    a.ObjectCount,a.Flags.Value,a.BroadcastHwQueueCount,a.ObjectHandleArray?1u:0u,
                    a.BroadcastHwQueueArray?1u:0u,a.FenceValueArray?1u:0u);
                if (!(caps.flags & HwQueueSignalCapability)) throw Error(ENOSYS);
                if (a.ObjectCount != 1 || !a.BroadcastHwQueueCount || a.BroadcastHwQueueCount > MaxHwQueues ||
                    !a.ObjectHandleArray || !a.BroadcastHwQueueArray || !a.FenceValueArray) throw Error(EINVAL);
                const HwQueueSignalDesc desc{a.Flags.Value,a.BroadcastHwQueueCount,a.FenceValueArray[0]};
                if (!validHwQueueSignal(desc)) throw Error(EINVAL);
                const auto sync=syncObjects.find(a.ObjectHandleArray[0]);
                if (sync == syncObjects.end() || sync->second.type != 5 || !packetSignalSyncFlags(sync->second.flags)) throw Error(EBADF);
                if (desc.fence <= sync->second.lastSignal) throw Error(EINVAL);
                std::vector<std::uint32_t> queues(a.BroadcastHwQueueArray,a.BroadcastHwQueueArray+desc.count);
                for (std::size_t n=0;n<queues.size();++n) {
                    const auto queue=hwQueueContexts.find(queues[n]);
                    if (queue == hwQueueContexts.end() || !contextOwners.count(queue->second) ||
                        contextOwners.at(queue->second).device != sync->second.device ||
                        std::find(queues.begin(),queues.begin()+n,queues[n]) != queues.begin()+n) throw Error(EBADF);
                }
                auto packet=request(Op::QueueHwQueueSignal,sync->first,desc);
                const auto data=reinterpret_cast<const std::uint8_t*>(queues.data());
                packet.insert(packet.end(),data,data+queues.size()*sizeof(queues[0]));
                const auto result=call(packet,0,true);
                if (result.header.handle != sync->first || !validHwSubmitReply({result.result.ntstatus,0,result.result.value},desc.fence,AsyncHwSubmitCapability)) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus); sync->second.lastSignal=desc.fence; lastHwSignalSync=sync->first;
                guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE nativeHwQueueSignalQueued=true flags=%u queues=%u target=%llu observedAtReturn=%llu noGpuAccess=true\n",
                    desc.flags,desc.count,static_cast<unsigned long long>(desc.fence),static_cast<unsigned long long>(result.result.value));
                return 0;
            }
            case 52: {
                auto& a = args<D3DKMT_SUBMITCOMMANDTOHWQUEUE>(requestNumber, pointer);
                unsigned commandOwners = 0, commandCpuLocked = 0;
                std::uint64_t commandOffset = 0, mappedBytes = 0;
                const auto queue = hwQueueContexts.find(a.hHwQueue);
                if (queue != hwQueueContexts.end()) {
                    const auto device = contextOwners.at(queue->second).device;
                    for (const auto& range : vendorGpuRanges) {
                        if (vendorOwners.at(range.first) != device || a.CommandBuffer < range.second.address) continue;
                        const auto offset = a.CommandBuffer - range.second.address;
                        if (offset > range.second.bytes || a.CommandLength > range.second.bytes - offset) continue;
                        ++commandOwners; commandOffset = offset; mappedBytes = range.second.bytes;
                        if (cpuLocks.count(range.first)) ++commandCpuLocked;
                    }
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE submitInput ownedQueue=%u commandBytes=%u privateBytes=%u primaries=%u hasCommand=%u hasPrivateData=%u hasPrimariesPointer=%u fence=%llu\n",
                             hwQueueContexts.count(a.hHwQueue) ? 1u : 0u, a.CommandLength, a.PrivateDriverDataSize, a.NumPrimaries,
                             a.CommandBuffer ? 1u : 0u, a.pPrivateDriverData ? 1u : 0u, a.WrittenPrimaries ? 1u : 0u,
                             static_cast<unsigned long long>(a.HwQueueProgressFenceId));
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE submitOwnership commandOwners=%u commandCpuLocked=%u commandOffset=%llu mappedBytes=%llu addressAlignment=%llu lengthAlignment=%u\n",
                             commandOwners, commandCpuLocked, static_cast<unsigned long long>(commandOffset), static_cast<unsigned long long>(mappedBytes),
                             static_cast<unsigned long long>(a.CommandBuffer % 4096), a.CommandLength % 4096);
                if (!(caps.flags & HwSubmitCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=52 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                const HwSubmitDesc desc{a.CommandBuffer, a.HwQueueProgressFenceId, a.CommandLength, a.PrivateDriverDataSize, a.NumPrimaries, 0};
                // With zero primaries the pointer is ignored, as in dxgkrnl;
                // it is never read or forwarded to the native process.
                if (!validHwSubmit(desc) || (desc.privateBytes && !a.pPrivateDriverData)) throw Error(EINVAL);
                if (queue == hwQueueContexts.end() || commandOwners != 1 || commandCpuLocked != 1) throw Error(EBADF);
                auto packet = request(caps.flags & AsyncHwSubmitCapability ? Op::QueueHwCommand : Op::SubmitHwQueue, a.hHwQueue, desc);
                if (desc.privateBytes) {
                    const auto data = static_cast<const std::uint8_t*>(a.pPrivateDriverData);
                    packet.insert(packet.end(), data, data + desc.privateBytes);
                }
                const auto result = call(packet, 0, true);
                if (result.header.handle != a.hHwQueue ||
                    !validHwSubmitReply({result.result.ntstatus,0,result.result.value},desc.fence,caps.flags)) {
                    transport.fail(); throw Error(EPROTO);
                }
                checkNt(result.result.ntstatus);
                if (caps.flags & AsyncHwSubmitCapability) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeCommandQueued=true bytes=%u privateBytes=%u target=%llu observedAtReturn=%llu\n",
                                 desc.bytes, desc.privateBytes, static_cast<unsigned long long>(desc.fence), static_cast<unsigned long long>(result.result.value));
                    return 0;
                }
                hwQueueFences.verifyRetired(a.hHwQueue, desc.fence, "command");
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeCommandSubmitted=true bytes=%u privateBytes=%u target=%llu observed=%llu\n",
                             desc.bytes, desc.privateBytes, static_cast<unsigned long long>(desc.fence), static_cast<unsigned long long>(result.result.value));
                return 0;
            }
            case 27: {
                auto& a = args<D3DKMT_DESTROYHWQUEUE>(requestNumber, pointer);
                if (!(caps.flags & HwQueueCapability)) {
                    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=27 bytes=%zu\n", sizeof a); throw Error(ENOSYS);
                }
                if (!hwQueueContexts.count(a.hHwQueue)) throw Error(EBADF);
                if (hwQueueFences.contains(a.hHwQueue)) hwQueueFences.unmap(a.hHwQueue);
                const auto result = call(request(Op::DestroyHwQueue, a.hHwQueue));
                if (result.header.handle != a.hHwQueue || result.result.value || result.result.ntstatus) { transport.fail(); throw Error(EPROTO); }
                hwQueueContexts.erase(a.hHwQueue);
                for (auto fence = borrowedFences.begin(); fence != borrowedFences.end();) {
                    if (fence->second.hardware && fence->second.owner == a.hHwQueue) fence = borrowedFences.erase(fence); else ++fence;
                }
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeHwQueueDestroyed=true\n"); return 0;
            }
            default:
                guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE unsupported nr=%u bytes=%u\n", nr, static_cast<unsigned>(_IOC_SIZE(requestNumber)));
                throw Error(ENOSYS);
        }
    }
    void prepareNativeTexture(NativeTextureDesc profile) {
        if (!(caps.flags & NativeSharedTextureCapability) || !(caps.flags & SharedVendorResourceCapability)) throw Error(ENOSYS);
        if (gpu_diagnostic::HighResolution && !(caps.flags & gpu_diagnostic::Capability)) throw Error(ENOSYS);
        if (!validNativeTexture(profile)) throw Error(EINVAL);
        if (pendingNativeDevice || !nativePreparations.empty() || !nativeBufferPreparations.empty() || !sharedResources.empty() || deviceAdapters.size() != 1) throw Error(EBUSY);
        pendingNativeTexture = profile; pendingNativeDevice = deviceAdapters.begin()->first; pendingNativeThread = pthread_self();
    }
    bool nativeInteropAvailable() {
        if(nativeInteropQueried) return nativeInteropSupported;
        negotiate();
        const auto packet=transport.exchange(request(Op::QueryNativeInteropCaps,0));
        Header header{}; std::memcpy(&header,packet.data(),sizeof header);
        if(header.type!=static_cast<unsigned>(Op::QueryNativeInteropCaps) || header.handle || header.padding) {
            transport.fail(); throw Error(EPROTO);
        }
        if(header.status==-95 && packet.size()==sizeof header) { nativeInteropQueried=true; return false; }
        NativeInteropCaps profile{};
        if(header.status || packet.size()!=sizeof header+sizeof profile) { transport.fail(); throw Error(EPROTO); }
        std::memcpy(&profile,packet.data()+sizeof header,sizeof profile);
        if(!validNativeInteropCaps(profile)) { transport.fail(); throw Error(EPROTO); }
        nativeInteropQueried=nativeInteropSupported=true;
        guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE nativeInteropCaps=true formats=7 maxWidth=4096 maxHeight=4096 maxLogicalBytes=67108864 implicitOpaque=true\n");
        return true;
    }
    void prepareNativeResource(NativeTextureDesc profile) {
        if(!validNativeInteropTexture(profile)) throw Error(EINVAL);
        if(nativeBufferEnabled()!=1 || !nativeInteropAvailable()) throw Error(ENOSYS);
        const auto required=NativeSharedTextureCapability|SharedVendorResourceCapability;
        if((caps.flags & required)!=required) throw Error(ENOSYS);
        const auto thread = static_cast<std::uint64_t>(syscall(SYS_gettid));
        if(pendingNativeDevice || nativeBufferPreparations.contains(thread) || !nativePreparations.stage(thread,profile,deviceAdapters)) throw Error(EBUSY);
    }
    void cancelNativeResource() { nativePreparations.cancel(static_cast<std::uint64_t>(syscall(SYS_gettid))); }
    bool nativeGpuBufferAvailable() {
        const auto enabled = std::getenv("WDDM_BRIDGE_NATIVE_GPU_BUFFERS");
        if (!enabled || std::strcmp(enabled, "1")) return false;
        if (nativeGpuBufferQueried) return nativeGpuBufferSupported;
        negotiate();
        const auto packet = transport.exchange(request(Op::QueryNativeGpuBufferCaps, 0));
        Header header{}; std::memcpy(&header, packet.data(), sizeof header);
        if (header.type != static_cast<unsigned>(Op::QueryNativeGpuBufferCaps) || header.handle || header.padding) {
            transport.fail(); throw Error(EPROTO);
        }
        if (header.status == -95 && packet.size() == sizeof header) { nativeGpuBufferQueried = true; return false; }
        NativeGpuBufferCaps profile{};
        if (header.status || packet.size() != sizeof header + sizeof profile) { transport.fail(); throw Error(EPROTO); }
        std::memcpy(&profile, packet.data() + sizeof header, sizeof profile);
        if (!validNativeGpuBufferCaps(profile)) { transport.fail(); throw Error(EPROTO); }
        nativeGpuBufferQueried = nativeGpuBufferSupported = true;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuBufferCaps=true version=1 maxBytes=67108864 allowedFlags=4\n");
        return true;
    }
    void checkNativeGpuBufferAdmission(NativeGpuBufferDesc profile) {
        if (nativeGpuBufferAdmissionUnsupported) return;
        const auto packet = transport.exchange(request(Op::QueryNativeGpuBufferAdmission, 0, profile));
        NativeBufferAdmissionReply response{};
        try { response = parseNativeBufferAdmissionReply(packet); }
        catch (...) { transport.fail(); throw Error(EPROTO); }
        if (!response.supported) {
            nativeGpuBufferAdmissionUnsupported = true;
            guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuBufferAdmissionUnsupported=true legacyProfileRetained=true\n");
            return;
        }
        if (response.transportStatus) throw Error(-response.transportStatus);
        checkNt(response.nativeStatus);
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE nativeGpuBufferAdmissionAccepted=true bytes=%llu advisoryOnly=true\n",
            static_cast<unsigned long long>(profile.bytes));
    }
    void prepareNativeGpuBuffer(NativeGpuBufferDesc profile) {
        if (!validNativeGpuBuffer(profile)) throw Error(EINVAL);
        if (!nativeGpuBufferAvailable()) throw Error(ENOSYS);
        const auto required = VendorAllocationCapability | VendorResourceCapability | SharedVendorResourceCapability;
        if ((caps.flags & required) != required) throw Error(ENOSYS);
        const auto thread = static_cast<std::uint64_t>(syscall(SYS_gettid));
        if (pendingNativeDevice || nativePreparations.contains(thread) || !nativeBufferPreparations.stage(thread, profile, deviceAdapters)) throw Error(EBUSY);
        try { checkNativeGpuBufferAdmission(profile); }
        catch (...) { nativeBufferPreparations.cancel(thread); throw; }
    }
    void cancelNativeGpuBuffer() { nativeBufferPreparations.cancel(static_cast<std::uint64_t>(syscall(SYS_gettid))); }
    void beginStreaming() {
        if (!gpu_diagnostic::Streaming || streamingNegotiated || openglNegotiated || !sharedResources.empty() || deviceAdapters.size() != 1) throw Error(EINVAL);
        const auto required = NativeSharedTextureCapability | SharedTextureConsumeCapability | AsyncHwSubmitCapability | UINT32_C(0x80000000);
        if ((caps.flags & required) != required) throw Error(ENOSYS);
        const auto packet = transport.exchange(request(Op::BeginStreaming, 0));
        if (packet.size() != sizeof(Header) + sizeof(StreamProfile)) { transport.fail(); throw Error(EPROTO); }
        Header header{}; StreamProfile profile{};
        std::memcpy(&header, packet.data(), sizeof header);
        std::memcpy(&profile, packet.data() + sizeof header, sizeof profile);
        if (header.type != static_cast<unsigned>(Op::BeginStreaming) || header.handle || header.status || header.padding ||
            !validStreamProfile(profile)) { transport.fail(); throw Error(EPROTO); }
        streamingNegotiated = true;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE guestStreamNegotiated=true frames=64 pendingCommands=16 pendingSignals=16\n");
    }
    void beginOpenGl(std::uint32_t version = 1) {
        if(coordinatedProfileNegotiated) { if(version==coordinatedProfileVersion) return;throw Error(EINVAL); }
        if (!gpu_diagnostic::HighResolution || openglNegotiated || streamingNegotiated || !ownedAdapters.empty() || !deviceAdapters.empty()) throw Error(EINVAL);
        negotiate();
        if(coordinatedProfileNegotiated) { if(version==coordinatedProfileVersion) return;throw Error(EINVAL); }
        const auto required = AsyncHwSubmitCapability | SyncCapability | GpuReservationCapability | UINT32_C(0x80000000) |
            (version>=6 ? ExpandedVendorGpuCapability : 0u) | (version>=7 ? ExpandedVendorCpuCapability : 0u);
        if ((caps.flags & required) != required) throw Error(ENOSYS);
        auto begin=version>=2 ? request(Op::BeginOpenGl,0,version) : request(Op::BeginOpenGl,0);
        if (version>=7) {
            guest_kernel::Aperture aperture;
            if (!aperture.enabled()) throw Error(ENOSYS);
            const auto capacity=aperture.capacity(); const auto layout=aperture.info();
            const OpenGlDesktop4kCapacity contract{version,capacity.version,capacity.cpu_byte_limit,layout.allocation_slots,layout.fence_slots,0};
            const OpenGlDesktopLargeMapCapacity large{contract,OpenGlDesktopLargeMapBytes};
            const OpenGlDesktopPresentationCapacity presentation{large,coordinatedReportedGpuByteLimit};
            if (capacity.flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED | (version>=8 ? WDDM_APERTURE_SPARSE_ENABLED : 0u) | (version==14 ? WDDM_APERTURE_MULTIAPP_CPU_ENABLED : 0u)) ||
                (version==7 ? !validOpenGlDesktop4kCapacity(contract) : version==8 ? !validOpenGlDesktopSparseCapacity(contract) : version==9 ? !validOpenGlDesktopLargeMapCapacity(large) : version==14 ? (coordinatedApplicationMemory ? !validApplicationMemoryCapacity(presentation) : !validOpenGlMultiappCapacity(presentation)) : version==13 ? !validOpenGlDesktopReferenceCapacity(presentation) : version==12 ? !validOpenGlDesktopSlotCapacity(presentation) : version==11 ? !validOpenGlDesktopFenceCapacity(presentation) : version!=10 || !validOpenGlDesktopPresentationCapacity(presentation))) throw Error(ENOSYS);
            begin=version>=10 ? request(Op::BeginOpenGl,0,presentation) : version==9 ? request(Op::BeginOpenGl,0,large) : request(Op::BeginOpenGl,0,contract);
            guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE kernelCpuCapacityVerified=true profile=%u cpuByteLimit=%llu cpuSlots=%u fenceSlots=%u\n",version,
                static_cast<unsigned long long>(contract.cpuByteLimit),contract.cpuSlots,contract.fenceSlots);
        }
        const auto packet=transport.exchange(begin);
        Header header{}; std::memcpy(&header, packet.data(), sizeof header);
        if (header.type != static_cast<unsigned>(Op::BeginOpenGl) || header.handle || header.padding ||
            (header.status && (header.status > 0 || header.status < -4095 || packet.size() != sizeof header))) {
            transport.fail(); throw Error(EPROTO);
        }
        if (header.status) throw Error(-header.status);
        if (packet.size() != sizeof header + sizeof(OpenGlProfile)) { transport.fail(); throw Error(EPROTO); }
        OpenGlProfile profile{}; std::memcpy(&profile, packet.data() + sizeof header, sizeof profile);
        if (version == 1 ? !validOpenGlProfile(profile) : version == 2 ? !validOpenGlFenceProfile(profile) : version == 3 ? !validOpenGlCompositorProfile(profile) : version == 4 ? !validOpenGlCompositorRenderProfile(profile) : version == 5 ? !validOpenGlDesktopProfile(profile) : version==6 ? !validOpenGlDesktop4kProfile(profile) : version==7 ? !validOpenGlDesktop4kCpuProfile(profile) : version==8 ? !validOpenGlDesktopSparseProfile(profile) : version==9 ? !validOpenGlDesktopLargeMapProfile(profile) : version==14 ? !validOpenGlMultiappProfile(profile) : version==13 ? !validOpenGlDesktopReferenceProfile(profile) : version==12 ? !validOpenGlDesktopSlotProfile(profile) : version==11 ? !validOpenGlDesktopFenceProfile(profile) : version!=10 || !validOpenGlDesktopPresentationProfile(profile)) { transport.fail(); throw Error(EPROTO); }
        if (version>=8) { allocationMemory.enableSparseAperture(version>=12); sparseApertureNegotiated=true;desktopSlotsNegotiated=version>=12; }
        fenceApertureBytes=version>=11 ? DesktopFenceApertureBytes : FenceApertureBytes;
        openglNegotiated = true;
        openglFencesNegotiated = version >= 2;
        openglCompositorNegotiated = version >= 3;
        openglRenderNegotiated = version >= 4;
        openglDesktopNegotiated = version >= 5;
        guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE openGlProfileNegotiated=true version=%u frames=%u commandLimit=%u reservationLimit=%u reservedByteLimit=17179869184 syncObjectLimit=%zu\n", version, profile.frames, profile.commandLimit, profile.reservationLimit, syncObjectLimit());
    }
    std::uint64_t captureCompositorBuffer(int bufferFd, int syncFileFd, std::uint32_t width, std::uint32_t height,
                                        std::uint32_t format, std::uint32_t stride) {
        if (compositorQueueUsed) throw Error(EBUSY);
        if (!openglRenderNegotiated || !(caps.flags & SharedTextureConsumeCapability)) throw Error(ENOSYS);
        std::uint64_t bytes = 0, target = 0;
        if (nativeBufferInfo(bufferFd, bytes)) throw Error(errno);
        if (bytes != std::uint64_t(stride)*height) throw Error(EINVAL);
        std::uint32_t resource = 0, sync = 0;
        if (!kernelObjects.resolve(bufferFd, resource, WDDM_OBJECT_RESOURCE)) throw Error(errno);
        guest_broker::Fd nativeSync(kernelObjects.resolveSyncFile(syncFileFd, target));
        if (nativeSync.get() < 0 || !kernelObjects.resolve(nativeSync.get(), sync)) throw Error(errno);
        if (!exportedResourceTickets.count(resource) || !exportedSyncTickets.count(sync)) throw Error(EBADF);
        const CompositorCaptureDesc desc{sync,width,height,format,stride,0,target};
        if (!validCompositorCapture(desc)) throw Error(EINVAL);
        const auto result = call(request(Op::CaptureCompositorResource, resource, desc), 0, true);
        if (result.header.handle != resource || result.result.ntstatus > 0 || (result.result.ntstatus < 0 && result.result.value)) {
            transport.fail(); throw Error(EPROTO);
        }
        checkNt(result.result.ntstatus);
        guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE compositorHostCapture=true width=%u height=%u format=%u fence=%llu windowsPresentation=false\n",
            width,height,format,static_cast<unsigned long long>(target));
        return result.result.value;
    }
    std::uint64_t queueCompositorBuffer(int bufferFd,int syncFileFd,std::uint32_t width,std::uint32_t height,
                                       std::uint32_t format,std::uint32_t stride,std::uint32_t flags) {
        if(compositorQueueEnded) throw Error(ESHUTDOWN);
        if (!openglRenderNegotiated || !(caps.flags&SharedTextureConsumeCapability) || queuedCompositorFrames==UINT64_MAX-1) throw Error(ENOSYS);
        std::uint64_t bytes=0,target=0;
        if (nativeBufferInfo(bufferFd,bytes)) throw Error(errno);
        if (bytes!=std::uint64_t(stride)*height) throw Error(EINVAL);
        std::uint32_t resource=0,sync=0;
        if (!kernelObjects.resolve(bufferFd,resource,WDDM_OBJECT_RESOURCE)) throw Error(errno);
        guest_broker::Fd nativeSync(kernelObjects.resolveSyncFile(syncFileFd,target));
        if (nativeSync.get()<0 || !kernelObjects.resolve(nativeSync.get(),sync)) throw Error(errno);
        if (!exportedResourceTickets.count(resource) || !exportedSyncTickets.count(sync)) throw Error(EBADF);
        const CompositorCaptureDesc desc{sync,width,height,format,stride,flags,target};
        if (!validCompositorFrame(desc)) throw Error(EINVAL);
        compositorQueueUsed=true;
        const auto result=call(request(Op::QueueCompositorFrame,resource,desc),0,true);
        if (result.header.handle!=resource || result.result.ntstatus>0 ||
            (result.result.ntstatus<0 ? result.result.value!=0 : result.result.value!=queuedCompositorFrames+1)) {
            transport.fail(); throw Error(EPROTO);
        }
        checkNt(result.result.ntstatus);
        ++queuedCompositorFrames;
        if(flags&CompositorFrameFinal) compositorQueueEnded=true;
        guest_bridge_log::diagnostic(stderr,"LINUX_BRIDGE compositorQueued=true frame=%llu sourceCopyRetired=true cpuUpload=false\n",
            static_cast<unsigned long long>(queuedCompositorFrames));
        return queuedCompositorFrames;
    }
    std::uint64_t consumeSharedTexture(std::uint64_t target, bool streaming = false) {
        if (streaming != streamingNegotiated || (streaming && (target != consumedStreamFrames + 1 || target > GuestStreamProfile.frames))) throw Error(EINVAL);
        if (!(caps.flags & SharedTextureConsumeCapability)) throw Error(ENOSYS);
        if (sharedResources.size() != 1 || !lastHwSignalSync) throw Error(EINVAL);
        const auto resource = *sharedResources.begin();
        const auto allocation = vendorResources.find(resource); const auto sync = syncObjects.find(lastHwSignalSync);
        if (allocation == vendorResources.end() || sync == syncObjects.end() || sync->second.type != 5 ||
            sync->second.flags != NoGpuAccessSyncFlag || sync->second.lastSignal != target ||
            !vendorOwners.count(allocation->second) || vendorOwners.at(allocation->second) != sync->second.device) throw Error(EBADF);
        const SharedTextureConsumeDesc desc{sync->first,0,target};
        if (!validSharedTextureConsume(desc)) throw Error(EINVAL);
        const auto result = call(request(streaming ? Op::ConsumeStreamingTexture : Op::ConsumeSharedTexture,resource,desc),0,true);
        if (result.header.handle != resource || result.result.ntstatus > 0 ||
            (result.result.ntstatus < 0 && result.result.value)) { transport.fail(); throw Error(EPROTO); }
        checkNt(result.result.ntstatus);
        if (streaming) {
            if (!streamAuditRound(target) && result.result.value) { transport.fail(); throw Error(EPROTO); }
            ++consumedStreamFrames;
        }
        return result.result.value;
    }
    void protocolFailed() { transport.fail(); }
    int exportSyncFile(const void* word,std::uint64_t value) { return syncFences.syncFile(word,value); }
    int exportNativeSyncFile(int fd,std::uint64_t value) {
        const auto word=nativeFenceWord(fd);
        return syncFences.syncFile(word,value,fd);
    }
    int resolveSyncFile(int fd,std::uint64_t& value) { return kernelObjects.resolveSyncFile(fd,value); }
    int exportNativeBuffer(int fd,std::uint64_t bytes) { return kernelObjects.nativeBuffer(fd,bytes); }
    int nativeBufferInfo(int fd,std::uint64_t& bytes) { return kernelObjects.nativeBufferInfo(fd,bytes); }
    int nativeBufferStats(wddm_native_buffer_stats& value) { return kernelObjects.nativeBufferStats(value); }
    int nativeBufferEnabled() { return kernelObjects.nativeBufferEnabled(); }
    int nativeBufferSupported() {
        const auto enabled=nativeBufferEnabled();
        return enabled==1 ? (nativeInteropAvailable() ? 1 : 0) : enabled;
    }
    const void* nativeFenceWord(int fd) {
        std::uint32_t ticket=0;
        if (!kernelObjects.resolve(fd,ticket,WDDM_OBJECT_SYNC)) throw Error(errno);
        const auto source=exportedSyncSources.find(ticket);
        if (source==exportedSyncSources.end() || !syncObjects.count(source->second)) throw Error(EBADF);
        return syncFences.mappedWord(source->second);
    }
    wddm_sync_file_stats syncFileInfo() { return syncFences.syncFileInfo(); }
    wddm_fence_eventfd_stats asyncCpuWaitInfo() {
        if (!guest_async::Enabled) throw Error(EOPNOTSUPP);
        for (const auto fences : {&syncFences, &hwQueueFences, &pagingFences})
            if (fences->eventStatsAvailable()) return fences->eventStats();
        throw Error(EBADF);
    }
};
// The process owns the bridge until the kernel destroys its address space.
// Stop forwarding under the same mutex used by every hook, but preserve state
// and CPU views for late DSO finalizers. Explicit object destruction during
// normal execution still releases its mappings and native references promptly.
std::atomic<Bridge*> bridgeInstance{nullptr};
void stopBridge() {
    auto* state=bridgeInstance.load(std::memory_order_acquire);
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->guard);
    if (bridgeStopped.exchange(true)) return;
    state->finish();
}
Bridge& bridge() {
    static Bridge* instance = [] {
        auto result = new Bridge;
        bridgeInstance.store(result,std::memory_order_release);
        std::atexit(stopBridge);
        return result;
    }();
    return *instance;
}
int deviceOpen(const char* path, int flags) {
    if (!path || std::strcmp(path, "/dev/dxg")) return -2;
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { return bridge().openDevice(flags); }
    catch (const Error& error) { errno = error.number; }
    catch (...) { errno = EIO; }
    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE openFailed errno=%d\n", errno); return -1;
}
bool modeArgument(int flags) { return (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE; }
}
// The opt-in Windows-style event handle ABI cannot represent an eventfd zero.
extern "C" int eventfd(unsigned int initial, int flags) noexcept {
    return guest_eventfd::create(initial, flags, guest_async::Enabled);
}
// Explicit diagnostic declaration for the next shared allocation on this thread.
// The sole owned device binds the declaration; a failed attempt consumes it.
extern "C" int wddm_bridge_prepare_native_texture(std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t flags) {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.prepareNativeTexture({width,height,format,flags}); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
// Lab handoff hook; it chooses only the unique owned shared resource and the
// most recently signaled owned workload fence. No host pointer/handle is exposed.
extern "C" int wddm_bridge_begin_streaming() {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.beginStreaming(); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_export_sync_file(const void* word,std::uint64_t value) {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.exportSyncFile(word,value); }
    catch (const std::system_error& error) { errno=error.code().value(); }
    catch (...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_async_cpu_wait_info(wddm_fence_eventfd_stats* info) {
    if (!info) { errno = EINVAL; return -1; }
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { *info = state.asyncCpuWaitInfo(); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (const std::system_error& error) { errno = error.code().value(); }
    catch (...) { errno = EIO; }
    return -1;
}
extern "C" const void* wddm_bridge_native_fence_word(int fd) {
    if (bridgeStopped) { errno=ESHUTDOWN; return nullptr; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return nullptr; }
    try { return state.nativeFenceWord(fd); }
    catch (const Error& error) { errno=error.number; }
    catch (const std::system_error& error) { errno=error.code().value(); }
    catch (...) { errno=EIO; }
    return nullptr;
}
// Additive opt-in hooks: caller owns returned CLOEXEC descriptors. The native
// descriptor is an opaque kernel capability, never a Windows HANDLE/ticket.
extern "C" int wddm_bridge_export_native_sync_file(int fd,std::uint64_t value) {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.exportNativeSyncFile(fd,value); }
    catch (const Error& error) { errno=error.number; }
    catch (const std::system_error& error) { errno=error.code().value(); }
    catch (...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_resolve_sync_file(int fd,std::uint64_t* value) {
    if (!value) { errno=EINVAL; return -1; }
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.resolveSyncFile(fd,*value); }
    catch (const Error& error) { errno=error.number; }
    catch (const std::system_error& error) { errno=error.code().value(); }
    catch (...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_export_native_buffer(int fd,std::uint64_t bytes) {
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.exportNativeBuffer(fd,bytes); }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_capture_compositor_buffer(int bufferFd,int syncFileFd,std::uint32_t width,std::uint32_t height,
    std::uint32_t format,std::uint32_t stride,std::uint64_t* hash) {
    if (bridgeStopped || bufferFd<0 || syncFileFd<0 || !hash) { errno=EINVAL; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { *hash=state.captureCompositorBuffer(bufferFd,syncFileFd,width,height,format,stride); return 0; }
    catch(const Error& error) { errno=error.number; }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_queue_compositor_buffer(int bufferFd,int syncFileFd,std::uint32_t width,std::uint32_t height,
    std::uint32_t format,std::uint32_t stride,std::uint32_t flags,std::uint64_t* frame) {
    if (bufferFd<0 || syncFileFd<0 || !frame) { errno=EINVAL; return -1; }
    *frame=0;
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { *frame=state.queueCompositorBuffer(bufferFd,syncFileFd,width,height,format,stride,flags); return 0; }
    catch(const Error& error) { errno=error.number; }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_native_buffer_info(int fd,std::uint64_t* bytes) {
    if(!bytes) { errno=EINVAL; return -1; }
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.nativeBufferInfo(fd,*bytes); }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_native_buffer_stats(wddm_native_buffer_stats* info) {
    if(!info) { errno=EINVAL; return -1; }
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.nativeBufferStats(*info); }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_native_buffer_supported(void) {
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { return state.nativeBufferSupported(); }
    catch(const Error& error) { errno=error.number; }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_prepare_native_resource(std::uint32_t width,std::uint32_t height,std::uint32_t format,std::uint32_t flags) {
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.prepareNativeResource({width,height,format,flags}); return 0; }
    catch(const Error& error) { errno=error.number; }
    catch(const std::system_error& error) { errno=error.code().value(); }
    catch(...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_cancel_native_resource(void) {
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if(bridgeStopped) { errno=ESHUTDOWN; return -1; }
    state.cancelNativeResource(); return 0;
}
extern "C" int wddm_bridge_native_gpu_buffer_supported(void) {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { return state.nativeGpuBufferAvailable() ? 1 : 0; }
    catch (const Error& error) { errno = error.number; }
    catch (const std::system_error& error) { errno = error.code().value(); }
    catch (...) { errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_prepare_native_gpu_buffer(std::uint64_t bytes, std::uint32_t flags) {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.prepareNativeGpuBuffer({bytes, flags, 0}); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (const std::system_error& error) { errno = error.code().value(); }
    catch (...) { errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_cancel_native_gpu_buffer(void) {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    state.cancelNativeGpuBuffer(); return 0;
}
extern "C" int wddm_bridge_sync_file_info(wddm_sync_file_stats* info) {
    if (bridgeStopped || !info) { errno=EINVAL; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { *info=state.syncFileInfo(); return 0; }
    catch (const std::system_error& error) { errno=error.code().value(); }
    catch (...) { errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl() {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.beginOpenGl(); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_fences() {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.beginOpenGl(2); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_compositor() {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.beginOpenGl(3); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_compositor_render() {
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { state.beginOpenGl(4); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_desktop_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(5); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_desktop_4k_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(6); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_desktop_4k_cpu_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(7); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_desktop_sparse_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(8); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_desktop_large_map_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(9); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_begin_opengl_desktop_presentation_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(10); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
extern "C" int wddm_bridge_consume_streaming_texture(std::uint64_t target, std::uint64_t* hash) {
    if (bridgeStopped || !hash) { errno = EINVAL; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { *hash = state.consumeSharedTexture(target, true); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int wddm_bridge_consume_shared_texture(std::uint64_t target, std::uint64_t* hash) {
    if (bridgeStopped || !hash) { errno = EINVAL; return -1; }
    auto& state = bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    try { *hash = state.consumeSharedTexture(target); return 0; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    return -1;
}
extern "C" int open(const char* path, int flags, ...) {
    const auto result = deviceOpen(path, flags); if (result != -2) return result;
    mode_t mode = 0; if (modeArgument(flags)) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags, mode));
}
extern "C" int open64(const char* path, int flags, ...) {
    const auto result = deviceOpen(path, flags); if (result != -2) return result;
    mode_t mode = 0; if (modeArgument(flags)) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return static_cast<int>(syscall(SYS_openat, AT_FDCWD, path, flags | O_LARGEFILE, mode));
}
extern "C" int openat(int directory, const char* path, int flags, ...) {
    const auto result = deviceOpen(path, flags); if (result != -2) return result;
    mode_t mode = 0; if (modeArgument(flags)) { va_list a; va_start(a, flags); mode = va_arg(a, mode_t); va_end(a); }
    return static_cast<int>(syscall(SYS_openat, directory, path, flags, mode));
}
extern "C" int close(int fd) {
    if (bridgeStopped) return rawClose(fd);
    // An inherited preload must leave ordinary processes idle. A bridge-owned
    // descriptor cannot be returned before its fully constructed state is
    // published by the explicit GPU open/hook path.
    auto* existing=bridgeInstance.load(std::memory_order_acquire);
    if (!existing) return rawClose(fd);
    const auto saved = errno;
    { auto& state = *existing; std::lock_guard<std::mutex> lock(state.guard); if (!bridgeStopped) state.closed(fd); }
    errno = saved; return rawClose(fd);
}
extern "C" int ioctl(int fd, unsigned long requestNumber, ...) noexcept {
    // This diagnostic is for this runtime's three-argument ioctl calls only.
    // A general libc interposer must also handle no-argument ioctl variants.
    va_list a; va_start(a, requestNumber); void* pointer = va_arg(a, void*); va_end(a);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    auto* existing=bridgeInstance.load(std::memory_order_acquire);
    if (!existing) return static_cast<int>(syscall(SYS_ioctl, fd, requestNumber, pointer));
    auto& state = *existing; std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno = ESHUTDOWN; return -1; }
    const auto saved = errno;
    if (!state.owns(fd)) { errno = saved; return static_cast<int>(syscall(SYS_ioctl, fd, requestNumber, pointer)); }
    try { return state.dispatch(requestNumber, pointer); }
    catch (const QueryProtocolError& error) { state.protocolFailed(); errno = error.code < 0 ? -error.code : EPROTO; }
    catch (const Error& error) { errno = error.number; }
    catch (...) { state.protocolFailed(); errno = EIO; }
    // A full, closed or broken diagnostic sink must not replace the GPU error
    // that libdxg translates back to NTSTATUS for the caller.
    const int failure = errno;
    guest_bridge_log::diagnostic(stderr, "LINUX_BRIDGE ioctlFailed nr=%u errno=%d\n", static_cast<unsigned>(_IOC_NR(requestNumber)), failure);
    errno = failure;
    return -1;
}

extern "C" int wddm_bridge_begin_opengl_desktop_fence_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(11); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}

extern "C" int wddm_bridge_begin_opengl_desktop_slot_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(12); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}

extern "C" int wddm_bridge_begin_opengl_multiapp_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(14); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}

extern "C" int wddm_bridge_begin_opengl_desktop_reference_render() {
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    auto& state=bridge(); std::lock_guard<std::mutex> lock(state.guard);
    if (bridgeStopped) { errno=ESHUTDOWN; return -1; }
    try { state.beginOpenGl(13); return 0; }
    catch (const Error& error) { errno=error.number; }
    catch (...) { state.protocolFailed(); errno=EIO; }
    return -1;
}
