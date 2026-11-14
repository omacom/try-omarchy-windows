// SPDX-License-Identifier: MIT
// Process-owned authenticated broker channel; no privileged controller FD.
#pragma once
#include "guest_broker_peer.h"
#include "driver_wire.h"
#include "driver_coordinator.h"
#include "guest-kernel/wddm_aperture.h"
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sys/un.h>

namespace guest_broker {
struct ViewRequest {
    std::uint32_t kind, reserved;
    std::uint64_t offset, bytes, generation;
};
struct ViewReply { std::uint64_t cookie; };
struct SyncFileRequest { std::uint64_t cookie, offset, value; };
static_assert(sizeof(SyncFileRequest)==24,"Fixed optional sync-file request ABI");
struct ObjectRequest { std::uint32_t kind, ticket; std::uint64_t reserved; };
struct ObjectRef { std::uint32_t kind=0, ticket=0; std::uint64_t cookie=0; };
static_assert(sizeof(ObjectRequest)==16 && sizeof(ObjectRef)==16,"Fixed broker capability ABI");
inline std::uint32_t importKind(driver_bridge::Op op) {
    using driver_bridge::Op;
    if (op==Op::ImportResource || op==Op::QuerySharedResourceInfo) return WDDM_OBJECT_RESOURCE;
    if (op==Op::ImportSync || op==Op::QuerySharedSyncInfo) return WDDM_OBJECT_SYNC;
    return 0;
}
inline bool recoverableObjectError(Operation operation,int error) {
    // A valid capability can outlive a refused recipient alias. Keep the
    // authenticated channel usable for reaping/retry after capacity errors.
    // Protocol/driver failures still poison the connection on both endpoints.
    return (operation==Operation::ResolveObject && (error==EBADF || error==EINVAL || error==EPERM || error==ENODEV || error==ENOMEM || error==ENOSPC)) ||
           ((operation==Operation::ExportNativeSyncFile || operation==Operation::ResolveNativeSyncFile ||
             operation==Operation::ExportNativeBuffer || operation==Operation::NativeBufferInfo) &&
            (error==EBADF || error==EINVAL || error==EPERM || error==ENODEV || error==EOPNOTSUPP || error==ETIMEDOUT || error==EIO || error==ENOMEM || error==ENOSPC)) ||
           (operation==Operation::CreateObject && (error==ENOMEM || error==ENOSPC)) ||
           (operation==Operation::ExportSyncFile && (error==ENOMEM || error==ENOSPC || error==EOPNOTSUPP));
}
static_assert(sizeof(ViewRequest) == 32 && sizeof(ViewReply) == 8, "Fixed broker view ABI");
template<class T> std::vector<std::uint8_t> encode(const T& value) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(&value);
    return {data, data + sizeof(value)};
}
template<class T> T decode(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != sizeof(T)) throw std::system_error(EPROTO, std::generic_category(), "Broker payload size");
    T value{}; std::memcpy(&value, bytes.data(), sizeof(value)); return value;
}
inline bool clientEnabled() {
    const auto* path = std::getenv("WDDM_BRIDGE_BROKER_SOCKET");
    return path && *path;
}
class Client {
    std::mutex guard;
    std::unique_ptr<Peer> peer;
    std::uint64_t sequence = 0;
    bool stopped = false;
    struct Resolved { int fd=-1; ObjectRef ref; } resolved;
    struct RemoteError : std::system_error { using std::system_error::system_error; };
    [[noreturn]] static void failed(int code, const char* message) {
        throw std::system_error(code, std::generic_category(), message);
    }
    Message requestUnlocked(Operation operation, const std::vector<std::uint8_t>& payload, const std::vector<int>& fds={}) {
        if (!peer || !peer->connected() || sequence == UINT64_MAX) failed(ESHUTDOWN, "Broker channel unavailable");
        const auto current = ++sequence;
        peer->send(operation, current, payload,fds);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            auto result = peer->receive();
            if (result.state == ReceiveState::Disconnected) failed(EPIPE, "Broker disconnected");
            if (result.state == ReceiveState::Packet) {
                auto& message = result.message;
                if (message.frame.operation != operation || message.frame.sequence != current)
                    failed(EPROTO, "Mismatched broker response");
                if (message.frame.status) throw RemoteError(message.frame.status,std::generic_category(),"Broker operation rejected");
                return std::move(message);
            }
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (ms <= 0) failed(ETIMEDOUT, "Broker response deadline exceeded");
            peer->waitReadable(static_cast<int>(ms));
        }
    }
    void connectUnlocked() {
        if (stopped) failed(ESHUTDOWN, "Broker connection stopped");
        if (peer) return;
        const auto* path = std::getenv("WDDM_BRIDGE_BROKER_SOCKET");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        if (!path || path[0] != '/' || std::strlen(path) >= sizeof(address.sun_path))
            failed(EINVAL, "Broker socket must have a bounded absolute path");
        std::memcpy(address.sun_path, path, std::strlen(path) + 1);
        Fd channel(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        if (channel.get() < 0) failed(errno, "Broker socket creation failed");
        const int enabled = 1;
        if (setsockopt(channel.get(), SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) ||
            connect(channel.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)))
            failed(errno, "Broker connect failed");
        peer = std::make_unique<Peer>(std::move(channel), 0, 0, true);
        auto hello = requestUnlocked(Operation::Hello, {});
        if (!hello.fds.empty()) failed(EPROTO, "Broker hello carried descriptors");
        (void)decode<wddm_aperture_info>(hello.payload);
    }
public:
    Message request(Operation operation, const std::vector<std::uint8_t>& payload = {}, const std::vector<int>& fds={}) {
        std::lock_guard<std::mutex> lock(guard);
        try { connectUnlocked(); return requestUnlocked(operation, payload,fds); }
        catch (const RemoteError& error) {
            if (!recoverableObjectError(operation,error.code().value())) { stopped=true; if (peer) peer->disconnect(); }
            throw;
        }
        catch (...) { stopped = true; if (peer) peer->disconnect(); throw; }
    }
    std::vector<std::uint8_t> driver(const std::vector<std::uint8_t>& packet) {
        std::vector<int> fds;
        if (packet.size()>=sizeof(driver_bridge::Header)) {
            driver_bridge::Header h{}; std::memcpy(&h,packet.data(),sizeof(h));
            const auto kind=importKind(static_cast<driver_bridge::Op>(h.type));
            if (kind) {
                std::lock_guard<std::mutex> lock(guard);
                if (resolved.fd<0 || resolved.ref.ticket!=h.handle || resolved.ref.kind!=kind)
                    failed(EBADF,"Native import requires a resolved capability FD");
                fds.push_back(resolved.fd); resolved=Resolved{};
            }
        }
        auto reply = request(Operation::DriverCall, packet,fds);
        if (!reply.fds.empty()) { disconnect(); failed(EPROTO, "Driver reply carried unexpected descriptors"); }
        return std::move(reply.payload);
    }
    wddm_aperture_info info() {
        auto reply = request(Operation::Hello);
        if (!reply.fds.empty()) { disconnect(); failed(EPROTO, "Layout reply carried descriptors"); }
        return decode<wddm_aperture_info>(reply.payload);
    }
    wddm_aperture_caps capacity() {
        auto reply=request(Operation::ApertureCaps);
        const auto value=decode<wddm_aperture_caps>(reply.payload);
        if (!reply.fds.empty() || !wddm_aperture_valid_caps(value)) {
            disconnect(); failed(EPROTO,"Invalid kernel capacity receipt");
        }
        return value;
    }
    driver_bridge::coordinator::Info coordinatedProfile() {
        auto reply=request(Operation::CoordinatedProfile);
        const auto value=decode<driver_bridge::coordinator::Info>(reply.payload);
        if(!reply.fds.empty() || !driver_bridge::coordinator::validInfo(value)) {
            disconnect();failed(EPROTO,"Invalid coordinated native profile");
        }
        return value;
    }
    Message acquire(ViewRequest view) {
        auto reply = request(Operation::AcquireView, encode(view));
        if (reply.fds.size() != 1 || !decode<ViewReply>(reply.payload).cookie) {
            disconnect(); failed(EPROTO, "Broker view response mismatch");
        }
        return reply;
    }
    void release(std::uint64_t cookie) {
        auto reply = request(Operation::ReleaseView, encode(ViewReply{cookie}));
        if (!reply.fds.empty() || !reply.payload.empty()) { disconnect(); failed(EPROTO, "Broker release response mismatch"); }
    }
    Fd syncFile(SyncFileRequest requestValue) {
        auto reply=request(Operation::ExportSyncFile,encode(requestValue));
        if (reply.fds.size()!=1 || !reply.payload.empty()) { disconnect(); failed(EPROTO,"Sync-file export response mismatch"); }
        return std::move(reply.fds[0]);
    }
    Fd nativeSyncFile(SyncFileRequest requestValue,int nativeFd) {
        auto reply=request(Operation::ExportNativeSyncFile,encode(requestValue),{nativeFd});
        if (reply.fds.size()!=1 || !reply.payload.empty()) { disconnect(); failed(EPROTO,"Native sync-file export response mismatch"); }
        return std::move(reply.fds[0]);
    }
    std::pair<Fd,std::uint64_t> resolveSyncFile(int fd) {
        auto reply=request(Operation::ResolveNativeSyncFile,{}, {fd});
        const auto value=decode<std::uint64_t>(reply.payload);
        if(reply.fds.size()!=1 || value==UINT64_MAX) { disconnect(); failed(EPROTO,"Native sync-file lineage response mismatch"); }
        return {std::move(reply.fds[0]),value};
    }
    wddm_sync_file_stats syncFileInfo() {
        auto reply=request(Operation::SyncFileInfo);
        if (!reply.fds.empty()) { disconnect(); failed(EPROTO,"Sync-file info carried descriptors"); }
        return decode<wddm_sync_file_stats>(reply.payload);
    }
    Fd nativeBuffer(int fd,std::uint64_t bytes) {
        auto reply=request(Operation::ExportNativeBuffer,encode(bytes),{fd});
        if(reply.fds.size()!=1 || !reply.payload.empty()) { disconnect(); failed(EPROTO,"Native buffer export response mismatch"); }
        return std::move(reply.fds[0]);
    }
    std::uint64_t nativeBufferInfo(int fd) {
        auto reply=request(Operation::NativeBufferInfo,{}, {fd});
        if(!reply.fds.empty() || reply.payload.size()!=sizeof(std::uint64_t)) { disconnect(); failed(EPROTO,"Native buffer info response shape"); }
        const auto bytes=decode<std::uint64_t>(reply.payload);
        if(!bytes || bytes>WDDM_NATIVE_BUFFER_MAX_BYTES) { disconnect(); failed(EPROTO,"Native buffer info response mismatch"); }
        return bytes;
    }
    wddm_native_buffer_stats nativeBufferStats() {
        auto reply=request(Operation::NativeBufferStats);
        if(!reply.fds.empty() || reply.payload.size()!=sizeof(wddm_native_buffer_stats)) { disconnect(); failed(EPROTO,"Native buffer stats response shape"); }
        const auto result=decode<wddm_native_buffer_stats>(reply.payload);
        if(result.version!=WDDM_APERTURE_VERSION || result.held>WDDM_NATIVE_BUFFER_MAX ||
           result.bytes>WDDM_NATIVE_BUFFER_TOTAL_BYTES || result.released>result.created) {
            disconnect(); failed(EPROTO,"Native buffer stats response mismatch");
        }
        return result;
    }
    wddm_native_buffer_caps nativeBufferCaps() {
        auto reply=request(Operation::NativeBufferCaps);
        if(!reply.fds.empty() || reply.payload.size()!=sizeof(wddm_native_buffer_caps)) { disconnect(); failed(EPROTO,"Native buffer caps response shape"); }
        const auto result=decode<wddm_native_buffer_caps>(reply.payload);
        if(result.version!=WDDM_APERTURE_VERSION || (result.flags&~WDDM_NATIVE_BUFFER_ENABLED) || result.reserved[0] || result.reserved[1]) {
            disconnect(); failed(EPROTO,"Native buffer caps response mismatch");
        }
        return result;
    }
    wddm_object_stats objectInfo() {
        auto reply=request(Operation::ObjectInfo);
        if (!reply.fds.empty()) { disconnect(); failed(EPROTO,"Object info carried descriptors"); }
        return decode<wddm_object_stats>(reply.payload);
    }
    Message createObject(std::uint32_t ticket,std::uint32_t kind) {
        auto reply=request(Operation::CreateObject,encode(ObjectRequest{kind,ticket,0}));
        const auto ref=decode<ObjectRef>(reply.payload);
        if (reply.fds.size()!=1 || ref.ticket!=ticket || ref.kind!=kind || !ref.cookie) {
            disconnect(); failed(EPROTO,"Capability publication mismatch");
        }
        return reply;
    }
    ObjectRef resolveObject(int fd,std::uint32_t kind) {
        { std::lock_guard<std::mutex> lock(guard); resolved=Resolved{}; }
        if (fd<0 || fcntl(fd,F_GETFD)<0) failed(EBADF,"Capability descriptor is closed");
        auto reply=request(Operation::ResolveObject,encode(ObjectRequest{kind,0,0}),{fd});
        const auto ref=decode<ObjectRef>(reply.payload);
        if (!reply.fds.empty() || ref.kind!=kind || !ref.ticket || !ref.cookie) {
            disconnect(); failed(EPROTO,"Capability resolution mismatch");
        }
        std::lock_guard<std::mutex> lock(guard); resolved={fd,ref}; return ref;
    }
    ObjectRef reapObject() {
        auto reply=request(Operation::ReapObject);
        const auto ref=decode<ObjectRef>(reply.payload);
        if (!reply.fds.empty() || (ref.ticket ? (!ref.cookie || (ref.kind!=WDDM_OBJECT_SYNC && ref.kind!=WDDM_OBJECT_RESOURCE)) : (ref.kind || ref.cookie))) {
            disconnect(); failed(EPROTO,"Capability reap mismatch");
        }
        return ref;
    }
    void disconnect() {
        std::lock_guard<std::mutex> lock(guard);
        stopped = true; if (peer) peer->disconnect();
    }
};
inline Client& client() {
    // Keep the channel identity through all driver DSO finalizers. Explicit
    // shutdown closes only the channel; the server waits for kernel exit.
    static auto* instance = new Client;
    return *instance;
}
} // namespace guest_broker
