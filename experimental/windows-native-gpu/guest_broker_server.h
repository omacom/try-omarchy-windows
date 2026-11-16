// SPDX-License-Identifier: MIT
// Single authenticated GPU client; multiple native workers remain separate work.
#pragma once
#include "guest_broker_views.h"
#include "guest_broker_objects.h"
#include <cstdio>
#include <optional>
#include <sys/syscall.h>

namespace guest_broker {
class Kernel {
    Fd control;
    void open() {
        if (control.get() >= 0) return;
        control = Fd(static_cast<int>(syscall(SYS_openat,AT_FDCWD,"/dev/wddm-aperture",O_RDWR|O_CLOEXEC,0)));
        if (control.get() < 0) throw std::system_error(errno,std::generic_category(),"Broker kernel controller open");
    }
public:
    void close() { control=Fd(); }
    wddm_aperture_desktop_slots desktopSlotCapacity() {
        open();wddm_aperture_desktop_slots value{};value.version=1;
        if(syscall(SYS_ioctl,control.get(),WDDM_APERTURE_DESKTOP_SLOTS,&value))
            throw std::system_error(errno,std::generic_category(),"Broker desktop slot capacity");
        if(!wddm_aperture_valid_desktop_slots(value)) throw std::system_error(EPROTO,std::generic_category(),"Broker desktop slot receipt");
        return value;
    }
    wddm_aperture_desktop_fences desktopFenceCapacity() {
        open();wddm_aperture_desktop_fences value{};value.version=1;
        if(syscall(SYS_ioctl,control.get(),WDDM_APERTURE_DESKTOP_FENCES,&value))
            throw std::system_error(errno,std::generic_category(),"Broker desktop fence capacity");
        if(!wddm_aperture_valid_desktop_fences(value)) throw std::system_error(EPROTO,std::generic_category(),"Broker desktop fence receipt");
        return value;
    }
    wddm_aperture_coordinator_caps coordinatedCapacity() {
        open();wddm_aperture_coordinator_caps value{};value.version=1;
        if(syscall(SYS_ioctl,control.get(),WDDM_APERTURE_COORDINATOR_CAPS,&value))
            throw std::system_error(errno,std::generic_category(),"Broker coordinated kernel capacity");
        if(!wddm_aperture_valid_coordinator_caps(value)) throw std::system_error(EPROTO,std::generic_category(),"Broker coordinated kernel receipt");
        return value;
    }
    wddm_aperture_caps capacity() {
        open(); wddm_aperture_caps value{}; value.version=1;
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_CAPS,&value))
            throw std::system_error(errno,std::generic_category(),"Broker kernel capacity");
        if (!wddm_aperture_valid_caps(value)) throw std::system_error(EPROTO,std::generic_category(),"Broker kernel capacity mismatch");
        return value;
    }
    wddm_aperture_info info() {
        open(); wddm_aperture_info value{}; value.version=WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_INFO,&value))
            throw std::system_error(errno,std::generic_category(),"Broker kernel layout");
        if (value.version != WDDM_APERTURE_VERSION || !driver_bridge::validFenceSlots(value.fence_slots,true) ||
            !driver_bridge::validVendorCpuSlots(value.allocation_slots,true,true) || value.cpu_slot_bytes != 1048576 ||
            value.fence_bytes != value.fence_slots*4096ull || value.allocation_bytes != value.allocation_slots*1048576ull ||
            value.active_cpu_bytes > WDDM_APERTURE_MULTIAPP_CPU_LIMIT ||
            (value.active_cpu_bytes>WDDM_APERTURE_CPU_LIMIT && value.active_cpu_bytes>capacity().cpu_byte_limit))
            throw std::system_error(EPROTO,std::generic_category(),"Broker kernel layout mismatch");
        if(value.active_leases>wddm_aperture_lease_limit(value.fence_slots)) {
            const auto leases=coordinatedCapacity();
            if(!leases.enabled || (value.fence_slots!=256 && value.fence_slots!=512) || (value.allocation_slots!=512 && value.allocation_slots!=1024) || value.active_leases>leases.lease_limit)
                throw std::system_error(EPROTO,std::generic_category(),"Broker coordinated kernel layout mismatch");
            if(leases.lease_limit==WDDM_APERTURE_DESKTOP_LEASES && (value.fence_slots!=512 || value.allocation_slots!=1024))
                throw std::system_error(EPROTO,std::generic_category(),"Expanded lease records lack the desktop aperture layout");
        }
        if(value.fence_slots==512) {
            const auto fences=desktopFenceCapacity();const auto leases=coordinatedCapacity();
            if(!fences.enabled || fences.slots!=value.fence_slots || fences.bytes!=value.fence_bytes || !leases.enabled || (value.allocation_slots!=512 && value.allocation_slots!=1024))
                throw std::system_error(EPROTO,std::generic_category(),"Desktop fence kernel agreement missing");
        }
        if (value.allocation_slots>=512 && ((value.fence_slots!=256 && value.fence_slots!=512) || (capacity().flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED) && capacity().flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED|WDDM_APERTURE_MULTIAPP_CPU_ENABLED))))
            throw std::system_error(EPROTO,std::generic_category(),"Sparse kernel layout lacks exact capacity opt-in");
        if(value.allocation_slots==1024) {
            const auto slots=desktopSlotCapacity();
            if(!slots.enabled || slots.slots!=value.allocation_slots || slots.bytes!=value.allocation_bytes || value.fence_slots!=512)
                throw std::system_error(EPROTO,std::generic_category(),"Desktop slot kernel agreement missing");
        }
        if((capacity().flags&WDDM_APERTURE_MULTIAPP_CPU_ENABLED) &&
           (value.allocation_slots!=1024 || value.fence_slots!=512 ||
            coordinatedCapacity().lease_limit!=WDDM_APERTURE_DESKTOP_LEASES))
            throw std::system_error(EPROTO,std::generic_category(),"Multi-application CPU capacity lacks the full desktop layout");
        return value;
    }
    std::pair<Fd,std::uint64_t> acquire(ViewRequest view) {
        const auto layout=info();
        const auto limit=view.kind == WDDM_APERTURE_FENCE ? layout.fence_bytes : layout.allocation_bytes;
        if (!view.bytes || view.offset >= limit || view.bytes > limit-view.offset)
            throw std::system_error(ERANGE,std::generic_category(),"Broker view exceeds configured aperture");
        wddm_aperture_create request{}; request.version=WDDM_APERTURE_VERSION;
        request.kind=view.kind; request.offset=view.offset; request.bytes=view.bytes; request.host_generation=view.generation;
        request.flags=view.kind == WDDM_APERTURE_ALLOCATION ? WDDM_APERTURE_WRITE : 0;
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_CREATE,&request))
            throw std::system_error(errno,std::generic_category(),"Broker kernel view create");
        Fd fd(request.fd);
        if (fd.get() < 0 || !request.cookie) throw std::system_error(EPROTO,std::generic_category(),"Broker kernel view identity");
        return {std::move(fd),request.cookie};
    }
    void release(std::uint64_t cookie) {
        wddm_aperture_revoke request{WDDM_APERTURE_VERSION,0,cookie};
        if (syscall(SYS_ioctl,control.get(),WDDM_APERTURE_REVOKE,&request))
            throw std::system_error(errno,std::generic_category(),"Broker kernel view revoke");
    }
    Fd syncFile(SyncFileRequest value) {
        open(); wddm_sync_file_create request{}; request.version=WDDM_APERTURE_VERSION;
        request.cookie=value.cookie; request.offset=value.offset; request.value=value.value;
        if (syscall(SYS_ioctl,control.get(),WDDM_SYNC_FILE_CREATE,&request))
            throw std::system_error(errno,std::generic_category(),"Broker native sync-file export");
        if (request.fd<0) throw std::system_error(EPROTO,std::generic_category(),"Broker sync-file descriptor");
        return Fd(request.fd);
    }
    Fd nativeSyncFile(SyncFileRequest value,int nativeFd) {
        open(); wddm_sync_file_native_create request{};
        request.sync.version=WDDM_APERTURE_VERSION; request.sync.cookie=value.cookie;
        request.sync.offset=value.offset; request.sync.value=value.value; request.native_fd=nativeFd;
        if(syscall(SYS_ioctl,control.get(),WDDM_SYNC_FILE_NATIVE_CREATE,&request))
            throw std::system_error(errno,std::generic_category(),"Broker native sync-file lineage create");
        if(request.sync.fd<0) throw std::system_error(EPROTO,std::generic_category(),"Broker linked sync-file descriptor");
        return Fd(request.sync.fd);
    }
    wddm_sync_file_stats syncFileInfo() {
        open(); wddm_sync_file_stats value{}; value.version=WDDM_APERTURE_VERSION;
        if (syscall(SYS_ioctl,control.get(),WDDM_SYNC_FILE_STATS,&value))
            throw std::system_error(errno,std::generic_category(),"Broker native sync-file stats");
        return value;
    }
};
class Port {
    const std::string path;
    const std::size_t packetLimit;
    Fd descriptor;
    void transfer(void* pointer, std::size_t bytes, bool reading,
                  std::chrono::steady_clock::time_point deadline) {
        auto* data=static_cast<std::uint8_t*>(pointer);
        while (bytes) {
            const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if (ms <= 0) throw std::system_error(ETIMEDOUT,std::generic_category(),"Broker native transfer deadline");
            pollfd event{descriptor.get(),static_cast<short>(reading ? POLLIN : POLLOUT),0};
            const auto ready=poll(&event,1,static_cast<int>(ms));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || (event.revents & (POLLERR|POLLNVAL)))
                throw std::system_error(ready < 0 ? errno : EIO,std::generic_category(),"Broker native port unavailable");
            const auto count=reading ? read(descriptor.get(),data,bytes) : write(descriptor.get(),data,bytes);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (count <= 0) throw std::system_error(EPIPE,std::generic_category(),"Broker native port disconnected");
            data+=count; bytes-=static_cast<std::size_t>(count);
        }
    }
public:
    explicit Port(std::string value,std::size_t limit=native_gpu::MaxPacket):path(std::move(value)),packetLimit(limit) {
        if(limit<native_gpu::MaxPacket || limit>native_gpu::MaxPacket+40) throw std::runtime_error("Invalid native port limit");
    }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        if(packet.size()<sizeof(driver_bridge::Header) || packet.size()>packetLimit)
            throw std::system_error(EPROTO,std::generic_category(),"Broker native request bounds");
        if (descriptor.get() < 0) {
            descriptor=Fd(static_cast<int>(syscall(SYS_openat,AT_FDCWD,path.c_str(),O_RDWR|O_CLOEXEC|O_NONBLOCK,0)));
            if (descriptor.get() < 0) throw std::system_error(errno,std::generic_category(),"Broker native port open");
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        auto bytes=static_cast<std::uint32_t>(packet.size());
        transfer(&bytes,sizeof(bytes),false,deadline);
        transfer(const_cast<std::uint8_t*>(packet.data()),packet.size(),false,deadline);
        transfer(&bytes,sizeof(bytes),true,deadline);
        if (bytes < sizeof(driver_bridge::Header) || bytes > packetLimit)
            throw std::system_error(EPROTO,std::generic_category(),"Broker native reply bounds");
        std::vector<std::uint8_t> reply(bytes); transfer(reply.data(),reply.size(),true,deadline); return reply;
    }
};
// A client-supplied receipt never substitutes for the controller's own query.
template<class Authority> void verifyCpuCapacityProfile(const std::vector<std::uint8_t>& packet,Authority& kernel,
        const driver_bridge::coordinator::Info* agreement=nullptr) {
    using namespace driver_bridge;
    if (packet.size()<sizeof(Header)+sizeof(std::uint32_t)) return;
    Header h{}; std::memcpy(&h,packet.data(),sizeof h);
    std::uint32_t version=0; std::memcpy(&version,packet.data()+sizeof h,sizeof version);
    if (h.type!=static_cast<unsigned>(Op::BeginOpenGl) || version<7) return;
    if ((version!=7 && version!=8 && version!=9 && version!=10 && version!=11 && version!=12 && version!=13 && version!=14) || packet.size()!=sizeof h+(version>=10 ? sizeof(OpenGlDesktopPresentationCapacity) : version==9 ? sizeof(OpenGlDesktopLargeMapCapacity) : sizeof(OpenGlDesktop4kCapacity)))
        throw std::system_error(EPROTO,std::generic_category(),"Missing kernel capacity receipt");
    OpenGlDesktop4kCapacity wanted{}; std::memcpy(&wanted,packet.data()+sizeof h,sizeof wanted);
    OpenGlDesktopLargeMapCapacity large{};
    if (version==9) std::memcpy(&large,packet.data()+sizeof h,sizeof large);
    OpenGlDesktopPresentationCapacity presentation{};
    if(version>=10) std::memcpy(&presentation,packet.data()+sizeof h,sizeof presentation);
    if (version==14 ? !(validOpenGlMultiappCapacity(presentation) ||
            (agreement && coordinator::matchesApplicationMemoryCapacity(presentation,*agreement))) : version==13 ? !validOpenGlDesktopReferenceCapacity(presentation) : version==12 ? !validOpenGlDesktopSlotCapacity(presentation) : version==11 ? !validOpenGlDesktopFenceCapacity(presentation) : version==10 ? !validOpenGlDesktopPresentationCapacity(presentation) : version==7 ? !validOpenGlDesktop4kCapacity(wanted) : version==8 ? !validOpenGlDesktopSparseCapacity(wanted) : !validOpenGlDesktopLargeMapCapacity(large)) throw std::system_error(EPROTO,std::generic_category(),"Invalid CPU capacity contract");
    const auto caps=kernel.capacity(); const auto layout=kernel.info();
    if (!wddm_aperture_valid_caps(caps) || caps.flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED | (version>=8 ? WDDM_APERTURE_SPARSE_ENABLED : 0u) | (version==14 ? WDDM_APERTURE_MULTIAPP_CPU_ENABLED : 0u)) ||
        caps.cpu_byte_limit!=wanted.cpuByteLimit || layout.version!=1 ||
        layout.allocation_slots!=wanted.cpuSlots || layout.fence_slots!=wanted.fenceSlots)
        throw std::system_error(EPROTO,std::generic_category(),"Kernel/profile CPU capacity mismatch");
    if(version>=11) {
        const auto fences=kernel.desktopFenceCapacity();const auto leases=kernel.coordinatedCapacity();
        if(!wddm_aperture_valid_desktop_fences(fences) || !fences.enabled || fences.slots!=wanted.fenceSlots ||
           fences.bytes!=layout.fence_bytes || !wddm_aperture_valid_coordinator_caps(leases) || !leases.enabled ||
           (version==14 && leases.lease_limit!=WDDM_APERTURE_DESKTOP_LEASES))
            throw std::system_error(EPROTO,std::generic_category(),"Missing actual desktop fence receipt");
    }
    if(version>=12) {
        const auto slots=kernel.desktopSlotCapacity();
        if(!wddm_aperture_valid_desktop_slots(slots) || !slots.enabled || slots.slots!=wanted.cpuSlots || slots.bytes!=layout.allocation_bytes)
            throw std::system_error(EPROTO,std::generic_category(),"Missing actual desktop slot receipt");
    }
}
template<class Native,class Authority,class ObjectAuthority,class Objects> int serveWithObjects(Peer& peer, Native& native, Authority& kernel, ObjectAuthority& objectKernel,Objects& objects) {
    Views views;
    std::uint64_t calls=0,created=0,released=0,denied=0,recoverable=0;
    bool failed=false;
    try {
        while (peer.connected()) {
            // Descendants may retain an inherited socket after the original
            // producer exits. Its authenticated pidfd, rather than EOF, owns
            // the memory lifetime and starts capability retention cleanup.
            if (peer.processExited()) { peer.disconnect(); break; }
            auto received=peer.receive();
            if (received.state == ReceiveState::Disconnected) break;
            if (received.state == ReceiveState::Idle) { peer.waitReadable(100); continue; }
            const auto& message=received.message;
            bool expectedObjectError=false;
            try {
                const auto operation=message.frame.operation;
                if (operation!=Operation::DriverCall && operation!=Operation::ResolveObject &&
                    operation!=Operation::ExportNativeSyncFile && operation!=Operation::ResolveNativeSyncFile &&
                    operation!=Operation::ExportNativeBuffer && operation!=Operation::NativeBufferInfo && !message.fds.empty())
                    throw std::system_error(EPROTO,std::generic_category(),"Unsolicited broker descriptors");
                if (operation == Operation::Hello) {
                    if (!message.payload.empty()) throw std::system_error(EINVAL,std::generic_category(),"Broker hello payload");
                    peer.send(operation,message.frame.sequence,encode(kernel.info()));
                } else if (operation == Operation::DriverCall) {
                    views.approve(message.payload);
                    std::optional<driver_bridge::coordinator::Info> memoryAgreement;
                    if(message.payload.size()==sizeof(native_gpu::Header)+sizeof(driver_bridge::OpenGlDesktopPresentationCapacity)) {
                        native_gpu::Header header{};std::memcpy(&header,message.payload.data(),sizeof header);
                        driver_bridge::OpenGlDesktopPresentationCapacity capacity{};
                        if(header.type==static_cast<unsigned>(driver_bridge::Op::BeginOpenGl)) {
                            std::memcpy(&capacity,message.payload.data()+sizeof header,sizeof capacity);
                            if(driver_bridge::validApplicationMemoryCapacity(capacity))
                                memoryAgreement=decode<driver_bridge::coordinator::Info>(objects.coordinatedProfile());
                        }
                    }
                    // This agreement comes from the owner-side actor registry,
                    // never from an ordinary client's supplied capacity bytes.
                    const auto* agreement=memoryAgreement ? &*memoryAgreement : nullptr;
                    verifyCpuCapacityProfile(message.payload,kernel,agreement);
                    objects.authorize(message.payload,message.fds,objectKernel);
                    auto reply=native.exchange(message.payload); ++calls;
                    views.observe(message.payload,reply,agreement);
                    objects.observe(message.payload,reply);
                    peer.send(operation,message.frame.sequence,reply);
                } else if (operation == Operation::AcquireView) {
                    auto& grant=views.acquire(decode<ViewRequest>(message.payload));
                    auto result=kernel.acquire(grant.view); grant.cookie=result.second; ++created;
                    peer.send(operation,message.frame.sequence,encode(ViewReply{grant.cookie}),{result.first.get()});
                } else if (operation == Operation::ReleaseView) {
                    auto& grant=views.cookie(decode<ViewReply>(message.payload).cookie);
                    kernel.release(grant.cookie); grant.cookie=0; ++released;
                    peer.send(operation,message.frame.sequence,{});
                } else if (operation == Operation::ExportSyncFile) {
                    const auto value=decode<SyncFileRequest>(message.payload);
                    views.syncFile(value); expectedObjectError=true;
                    auto fd=kernel.syncFile(value);
                    peer.send(operation,message.frame.sequence,{}, {fd.get()});
                } else if (operation == Operation::SyncFileInfo) {
                    if (!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Sync-file info payload");
                    peer.send(operation,message.frame.sequence,encode(kernel.syncFileInfo()));
                } else if (operation == Operation::ExportNativeSyncFile) {
                    if(message.fds.size()!=1) throw std::system_error(EPROTO,std::generic_category(),"Native sync-file export requires one capability FD");
                    const auto value=decode<SyncFileRequest>(message.payload);
                    const auto source=views.syncFile(value,true); expectedObjectError=true;
                    objects.syncLineage(message.fds[0].get(),source,objectKernel);
                    auto fd=kernel.nativeSyncFile(value,message.fds[0].get());
                    peer.send(operation,message.frame.sequence,{}, {fd.get()});
                } else if (operation == Operation::ResolveNativeSyncFile) {
                    if(message.fds.size()!=1 || !message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Sync-file resolution requires exactly one FD");
                    expectedObjectError=true;
                    auto result=objectKernel.resolveSyncFile(message.fds[0].get());
                    objects.validate(result.first.get(),WDDM_OBJECT_SYNC,objectKernel);
                    peer.send(operation,message.frame.sequence,encode(result.second), {result.first.get()});
                } else if (operation == Operation::ExportNativeBuffer) {
                    if(message.fds.size()!=1) throw std::system_error(EPROTO,std::generic_category(),"Native buffer export requires one resource FD");
                    const auto bytes=decode<std::uint64_t>(message.payload); expectedObjectError=true;
                    if(!bytes || bytes>WDDM_NATIVE_BUFFER_MAX_BYTES) throw std::system_error(EINVAL,std::generic_category(),"Invalid native buffer logical extent");
                    objects.validate(message.fds[0].get(),WDDM_OBJECT_RESOURCE,objectKernel);
                    auto fd=objectKernel.nativeBuffer(message.fds[0].get(),bytes);
                    peer.send(operation,message.frame.sequence,{}, {fd.get()});
                } else if (operation == Operation::NativeBufferInfo) {
                    if(message.fds.size()!=1 || !message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Native buffer info requires exactly one FD");
                    expectedObjectError=true;
                    const auto bytes=objectKernel.nativeBufferInfo(message.fds[0].get());
                    objects.validate(message.fds[0].get(),WDDM_OBJECT_RESOURCE,objectKernel);
                    peer.send(operation,message.frame.sequence,encode(bytes));
                } else if (operation == Operation::NativeBufferStats) {
                    if(!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Native buffer stats payload");
                    peer.send(operation,message.frame.sequence,encode(objectKernel.nativeBufferStats()));
                } else if (operation == Operation::NativeBufferCaps) {
                    if(!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Native buffer caps payload");
                    peer.send(operation,message.frame.sequence,encode(objectKernel.nativeBufferCaps()));
                } else if (operation == Operation::ApertureCaps) {
                    if (!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Aperture caps payload");
                    peer.send(operation,message.frame.sequence,encode(kernel.capacity()));
                } else if (operation == Operation::CoordinatedProfile) {
                    if(!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Coordinated profile payload");
                    const auto value=objects.coordinatedProfile();
                    if(value.empty()) throw std::system_error(EOPNOTSUPP,std::generic_category(),"Coordinated profile unavailable");
                    peer.send(operation,message.frame.sequence,value);
                } else if (operation == Operation::ObjectInfo) {
                    if (!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Object info payload");
                    peer.send(operation,message.frame.sequence,encode(objectKernel.info()));
                } else if (operation == Operation::CreateObject) {
                    const auto request=decode<ObjectRequest>(message.payload);
                    expectedObjectError=true;
                    auto result=objects.create(request,objectKernel);
                    peer.send(operation,message.frame.sequence,encode(result.second),{result.first.get()});
                } else if (operation == Operation::ResolveObject) {
                    const auto request=decode<ObjectRequest>(message.payload);
                    if (message.fds.size()!=1 || request.ticket || request.reserved ||
                        (request.kind!=WDDM_OBJECT_SYNC && request.kind!=WDDM_OBJECT_RESOURCE))
                        throw std::system_error(EPROTO,std::generic_category(),"Invalid object resolution request");
                    expectedObjectError=true;
                    const auto result=objects.resolve(message.fds[0].get(),request.kind,objectKernel);
                    peer.send(operation,message.frame.sequence,encode(result));
                } else if (operation == Operation::ReapObject) {
                    if (!message.payload.empty()) throw std::system_error(EPROTO,std::generic_category(),"Object reap payload");
                    peer.send(operation,message.frame.sequence,encode(objects.reap(objectKernel)));
                } else throw std::system_error(EOPNOTSUPP,std::generic_category(),"Broker operation not implemented");
            } catch (const std::system_error& error) {
                // A valid request may finish after its producer closes the
                // socket. send() disconnects on these errors. Retain authority
                // until pidfd-confirmed exit, just as for ordinary EOF.
                if (!peer.connected() && (error.code().value()==EPIPE || error.code().value()==ECONNRESET || error.code().value()==ENOTCONN)) {
                    std::fprintf(stderr,"BROKER_ROUTE replyDisconnected=true processExitRequired=true\n");
                    break;
                }
                if (expectedObjectError && recoverableObjectError(message.frame.operation,error.code().value())) {
                    ++recoverable;
                    peer.send(message.frame.operation,message.frame.sequence,{}, {},error.code().value());
                    continue;
                }
                ++denied; failed=true;
                std::fprintf(stderr,"BROKER_ROUTE rejected=true errno=%d operation=%u reason=%s\n",error.code().value(),static_cast<unsigned>(message.frame.operation),error.what());
                peer.send(message.frame.operation,message.frame.sequence,{}, {},error.code().value());
                peer.disconnect();
            }
        }
    } catch (const std::exception& error) {
        failed=true; peer.disconnect(); std::fprintf(stderr,"BROKER_ROUTE failed=true reason=%s\n",error.what());
    }
    const auto retained=views.active();
    // This is deliberately outside the channel loop. Neither EOF nor any
    // rejection destroys the native transport or privileged kernel controller.
    for (;;) {
        try { if (peer.processExited()) break; }
        catch (const std::exception& error) {
            failed=true; std::fprintf(stderr,"BROKER_ROUTE failed=true processLifetimeUnknown=true reason=%s\n",error.what());
        }
        poll(nullptr,0,10);
    }
    // CPU mappings end with their producer. Shared native references use a
    // different controller and survive until the last recipient FD closes.
    kernel.close();
    try { objects.drain(native,objectKernel); }
    catch (const std::exception& error) {
        failed=true; std::fprintf(stderr,"BROKER_OBJECT failed=true producerExited=true reason=%s\n",error.what());
    }
    objects.report();
    if (recoverable) std::fprintf(stderr,"BROKER_OBJECT recoverableErrors=%llu\n",static_cast<unsigned long long>(recoverable));
    std::fprintf(stderr,"BROKER_ROUTE_EXIT processExited=true retainedViews=%zu nativeCalls=%llu viewsCreated=%llu viewsReleased=%llu rejected=%llu multiClient=%s\n",
                 retained,static_cast<unsigned long long>(calls),static_cast<unsigned long long>(created),
                 static_cast<unsigned long long>(released),static_cast<unsigned long long>(denied),objects.multiClient() ? "true" : "false");
    return failed ? 1 : 0;
}
template<class Native,class Authority,class ObjectAuthority> int serve(Peer& peer,Native& native,Authority& kernel,ObjectAuthority& objectKernel) {
    ObjectStore objects;return serveWithObjects(peer,native,kernel,objectKernel,objects);
}
template<class Native,class Authority> int serve(Peer& peer, Native& native, Authority& kernel) {
    KernelObjects objectKernel;
    return serve(peer,native,kernel,objectKernel);
}
} // namespace guest_broker
