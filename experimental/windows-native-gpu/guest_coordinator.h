// SPDX-License-Identifier: MIT
// Privileged controller transport and cross-client capability registry.
#pragma once
#include "guest_broker_server.h"
#include "driver_coordinator.h"
#include "guest_broker_receipts.h"
#include "broker_residency_observation.h"
#include "fair_rpc_mutex.h"
#include "stopped_group_retirement_poll.h"
#include <deque>
#include <set>
#include <atomic>
#include <sys/stat.h>
#if defined(WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
#if !defined(WDDM_BRIDGE_4K_LAB) || !WDDM_BRIDGE_4K_LAB || !defined(WDDM_BRIDGE_MULTIAPP_CPU) || !WDDM_BRIDGE_MULTIAPP_CPU
#error Diagnostic alias refusal requires the explicit CPU384 4K test build
#endif
#if WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME < 1 || WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME > 1700
#error Diagnostic alias refusal must be armed in frames 1..1700
#endif
#include "diagnostic_alias_refusal.h"
#endif
#if defined(WDDM_BRIDGE_MULTIAPP_CPU) && WDDM_BRIDGE_MULTIAPP_CPU
#if !defined(WDDM_BRIDGE_FOUR_ACTOR_OBJECTS) || !WDDM_BRIDGE_FOUR_ACTOR_OBJECTS
#error Multi-application CPU mappings require four-actor object negotiation
#endif
#endif
#if defined(WDDM_BRIDGE_FOUR_ACTOR_OBJECTS) && WDDM_BRIDGE_FOUR_ACTOR_OBJECTS
#if !defined(WDDM_BRIDGE_WIDE_KERNEL_LEASES) || !WDDM_BRIDGE_WIDE_KERNEL_LEASES
#error Four-actor object metadata requires expanded kernel lease negotiation
#endif
#endif

#if defined(WDDM_BRIDGE_APPLICATION_MEMORY) && WDDM_BRIDGE_APPLICATION_MEMORY
#if !defined(WDDM_BRIDGE_APPLICATION_ACTORS) || !WDDM_BRIDGE_APPLICATION_ACTORS || !defined(WDDM_BRIDGE_APPLICATION_REFERENCES) || !WDDM_BRIDGE_APPLICATION_REFERENCES || !defined(WDDM_BRIDGE_MULTIAPP_CPU) || !WDDM_BRIDGE_MULTIAPP_CPU
#error Application memory requires the explicit application process, reference and kernel CPU agreements
#endif
#endif
namespace guest_broker {
namespace cg=driver_bridge::coordinator;
namespace ar=driver_bridge::actors;
inline void publishRetirementWitness(bool enabled,pid_t process,std::uint64_t actor,
                                     const char* path="/run/wddm-retired") {
    if(!enabled) return;
    // Legacy finite diagnostics require this root-owned PID witness. Normal
    // sessions may have rolling actors for one PID and must not depend on it.
    char name[32];const auto count=snprintf(name,sizeof name,"%d",process);
    if(count<=0 || static_cast<std::size_t>(count)>=sizeof name) throw std::runtime_error("Retirement marker PID");
    Fd directory(open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
    struct stat status{};
    if(directory.get()<0 || fstat(directory.get(),&status) || status.st_uid || (status.st_mode&0022))
        throw std::runtime_error("Retirement marker directory ownership");
    Fd marker(openat(directory.get(),name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0444));
    char value[64];const auto bytes=snprintf(value,sizeof value,"%llu\n",static_cast<unsigned long long>(actor));
    if(marker.get()<0 || bytes<=0 || static_cast<std::size_t>(bytes)>=sizeof value || fchmod(marker.get(),0444) || write(marker.get(),value,bytes)!=bytes)
        throw std::runtime_error("Retirement marker write");
}
inline int nativeAliasFailureErrno(std::int32_t status) {
    // ntstatus.h: STATUS_NO_MEMORY / STATUS_INSUFFICIENT_RESOURCES. An
    // unexpected native failure must not masquerade as recoverable pressure.
    const auto value=static_cast<std::uint32_t>(status);
    return value==0xc0000017u || value==0xc000009au ? ENOMEM : EIO;
}
class CoordinatorPort {
    Port port;
    driver_bridge::FairRpcMutex lock;
    std::uint64_t sequence=1,actorSequence=1;
    std::atomic<bool> failed{false};
    cg::Info agreement{};
    bool liveRetirement=false;
    bool retirementWitnessFiles=true;
    template<class T> static T at(const std::vector<std::uint8_t>& packet,std::size_t offset=0) {
        if(offset>packet.size() || sizeof(T)>packet.size()-offset) throw std::runtime_error("Truncated coordinator response");
        T value{};std::memcpy(&value,packet.data()+offset,sizeof value);return value;
    }
    [[noreturn]] void reject(const char* message) { failed=true;throw std::runtime_error(message); }
public:
    explicit CoordinatorPort(std::string path,bool witnesses=true):port(std::move(path),cg::MaxPacket),retirementWitnessFiles(witnesses) {}
    std::vector<std::uint8_t> control(cg::Operation operation,const std::vector<std::uint8_t>& payload={}) {
        std::lock_guard<driver_bridge::FairRpcMutex> guard(lock);
        if(failed || sequence==UINT64_MAX) reject("Coordinator control unavailable");
        cg::Frame h;h.operation=operation;h.sequence=sequence++;h.bytes=static_cast<std::uint32_t>(payload.size());
        auto packet=encode(h);packet.insert(packet.end(),payload.begin(),payload.end());
        std::vector<std::uint8_t> reply;
        try { reply=port.exchange(packet); } catch(...) { failed=true;throw; }
        const auto r=at<cg::Frame>(reply);
        if(r.magic!=cg::Magic || r.version!=cg::ProtocolVersion || r.operation!=operation || r.sequence!=h.sequence || r.reserved ||
            r.bytes!=reply.size()-sizeof r || r.status>0 || r.status< -4095 || (r.status && r.bytes)) reject("Coordinator reply identity mismatch");
        if(operation==cg::Operation::RetireStoppedGroup && r.status==-EBUSY)
            throw driver_bridge::StoppedGroupPending{};
        if(r.status) throw std::system_error(-r.status,std::generic_category(),"Coordinator control refused");
        return {reply.begin()+sizeof r,reply.end()};
    }
    void initialize(const wddm_aperture_info& layout,const wddm_aperture_caps& caps,const wddm_aperture_coordinator_caps& leases,
                    const wddm_aperture_desktop_fences& fences={},const wddm_aperture_desktop_slots& slots={},bool references=false) {
        const bool wide=layout.fence_slots==512;
        const bool expanded=layout.allocation_slots==1024;
#if defined(WDDM_BRIDGE_WIDE_KERNEL_LEASES) && WDDM_BRIDGE_WIDE_KERNEL_LEASES
#if !defined(WDDM_BRIDGE_MANAGED_GPUVA) || !WDDM_BRIDGE_MANAGED_GPUVA
#error Expanded kernel lease negotiation requires managed GPU address accounting
#endif
        constexpr auto expectedLeases=driver_bridge::GuestCoordinatedLeaseObjects.leaseLimit;
        if(!wide || !expanded || !references) reject("Expanded kernel leases require the full reference desktop layout");
#else
        constexpr auto expectedLeases=driver_bridge::CoordinatedObjectCapacity{}.leaseLimit;
#endif
#if defined(WDDM_BRIDGE_MULTIAPP_CPU) && WDDM_BRIDGE_MULTIAPP_CPU
        constexpr bool multiapp=true;
#else
        constexpr bool multiapp=false;
#endif
#if defined(WDDM_BRIDGE_APPLICATION_MEMORY) && WDDM_BRIDGE_APPLICATION_MEMORY
        constexpr bool requestedMemory=true;
#else
        constexpr bool requestedMemory=false;
#endif
        const driver_bridge::OpenGlDesktopPresentationCapacity wanted{{{expanded ? references ? multiapp ? 14u : 13u : 12u : wide ? 11u : 10u,caps.version,caps.cpu_byte_limit,layout.allocation_slots,layout.fence_slots,0},
            driver_bridge::OpenGlDesktopLargeMapBytes},requestedMemory ? driver_bridge::GuestApplicationMemoryProfile.reportedBytes : driver_bridge::OpenGlDesktopPresentationReportedBytes};
        if((references && !expanded) || !wddm_aperture_valid_coordinator_caps(leases) || !leases.enabled || leases.lease_limit!=expectedLeases ||
            !wddm_aperture_valid_caps(caps) || caps.flags!=(WDDM_APERTURE_DESKTOP_CPU_ENABLED|WDDM_APERTURE_SPARSE_ENABLED|(multiapp ? WDDM_APERTURE_MULTIAPP_CPU_ENABLED : 0u)) ||
            (requestedMemory ? !driver_bridge::validApplicationMemoryCapacity(wanted) : !driver_bridge::validCoordinatedPresentationCapacity(wanted)) ||
            (wide && (!wddm_aperture_valid_desktop_fences(fences) || !fences.enabled || fences.slots!=layout.fence_slots || fences.bytes!=layout.fence_bytes)) ||
            (expanded && (!wddm_aperture_valid_desktop_slots(slots) || !slots.enabled || slots.slots!=layout.allocation_slots || slots.bytes!=layout.allocation_bytes))) reject("Coordinator kernel capacity mismatch");
#if defined(WDDM_BRIDGE_APPLICATION_OBJECTS) && WDDM_BRIDGE_APPLICATION_OBJECTS
#if !defined(WDDM_BRIDGE_APPLICATION_MEMORY) || !WDDM_BRIDGE_APPLICATION_MEMORY
#error Application allocation metadata requires explicit application memory agreement
#endif
        const auto referenceObjects=driver_bridge::GuestCoordinatedApplicationObjects;
#elif defined(WDDM_BRIDGE_MULTIAPP_CPU) && WDDM_BRIDGE_MULTIAPP_CPU
        const auto referenceObjects=driver_bridge::GuestCoordinatedMultiappObjects;
#elif defined(WDDM_BRIDGE_FOUR_ACTOR_OBJECTS) && WDDM_BRIDGE_FOUR_ACTOR_OBJECTS
        const auto referenceObjects=driver_bridge::GuestCoordinatedActorObjects;
#elif defined(WDDM_BRIDGE_WIDE_KERNEL_LEASES) && WDDM_BRIDGE_WIDE_KERNEL_LEASES
        const auto referenceObjects=driver_bridge::GuestCoordinatedLeaseObjects;
#elif defined(WDDM_BRIDGE_MANAGED_GPUVA) && WDDM_BRIDGE_MANAGED_GPUVA
        const auto referenceObjects=driver_bridge::GuestCoordinatedManagedObjects;
#else
        const auto referenceObjects=driver_bridge::GuestCoordinatedReferenceObjects;
#endif
        const auto objects=references ? referenceObjects : wide ? driver_bridge::GuestCoordinatedDesktopObjects : driver_bridge::CoordinatedObjectCapacity{};
        constexpr std::uint32_t requestedActors=
#if defined(WDDM_BRIDGE_APPLICATION_ACTORS) && WDDM_BRIDGE_APPLICATION_ACTORS
            driver_bridge::actors::MaxApplicationActors;
#else
            driver_bridge::actors::MaxActors;
#endif
        if(requestedActors>driver_bridge::actors::MaxActors && (!multiapp || !references))
            reject("Application processes require the shared aggregate desktop agreement");
        constexpr std::uint32_t requestedReferences=
#if defined(WDDM_BRIDGE_APPLICATION_REFERENCES) && WDDM_BRIDGE_APPLICATION_REFERENCES
            driver_bridge::ApplicationResourceReferences;
#else
            driver_bridge::MaxSharedResourceHandles;
#endif
        if(requestedReferences>driver_bridge::MaxSharedResourceHandles && requestedActors!=driver_bridge::actors::MaxApplicationActors)
            reject("Application resource references require application process negotiation");
        if(requestedMemory && (requestedActors!=driver_bridge::actors::MaxApplicationActors || requestedReferences!=driver_bridge::ApplicationResourceReferences))
            reject("Application memory requires explicit process and reference capacity");
        const auto initialization=requestedMemory ?
            encode(cg::ApplicationMemoryInitialization{{{{wanted,objects},requestedActors,0},requestedReferences,0},driver_bridge::GuestApplicationMemoryProfile}) :
            requestedReferences>driver_bridge::MaxSharedResourceHandles ?
            encode(cg::ApplicationReferenceInitialization{{{wanted,objects},requestedActors,0},requestedReferences,0}) :
            requestedActors==driver_bridge::actors::MaxActors ? encode(cg::Initialization{wanted,objects}) :
            encode(cg::ApplicationInitialization{{wanted,objects},requestedActors,0});
        if(!control(cg::Operation::Initialize,initialization).empty()) reject("Coordinator initialization payload");
        agreement=decode<cg::Info>(control(cg::Operation::Info));
        if(!cg::validInfo(agreement) || std::memcmp(&wanted,&agreement.capacity,sizeof wanted) ||
            std::memcmp(&objects,&agreement.objects,sizeof objects) || agreement.maxActors!=requestedActors ||
            agreement.resource.handleLimit!=requestedReferences || bool(agreement.flags&cg::ApplicationMemoryFlag)!=requestedMemory)
            reject("Coordinator agreement mismatch");
        if(requestedActors>driver_bridge::actors::MaxActors)
            receipt("BROKER_COORDINATOR_APPLICATION_ACTOR_CAPACITY actors=%u hwQueues=%u syncObjects=%u cpuBytes=%llu reportedGpuBytes=%llu memoryBudgetsMultiplied=false\n",
                requestedActors,objects.hwQueues,objects.syncObjects,static_cast<unsigned long long>(caps.cpu_byte_limit),static_cast<unsigned long long>(wanted.reportedGpuByteLimit));
        receipt("BROKER_COORDINATOR_KERNEL_CAPACITY verified=true leaseLimit=%u cpuBytes=%llu flags=%u actualIoctl=true\n",
            leases.lease_limit,static_cast<unsigned long long>(caps.cpu_byte_limit),caps.flags);
        if(wide) receipt("BROKER_COORDINATOR_FENCE_CAPACITY profile=%u slots=%u bytes=%llu actualIoctl=true\n",wanted.mapping.aperture.version,fences.slots,static_cast<unsigned long long>(fences.bytes));
        if(expanded) receipt("BROKER_COORDINATOR_SLOT_CAPACITY profile=%u slots=%u bytes=%llu cpuByteLimit=%llu actualIoctl=true\n",wanted.mapping.aperture.version,slots.slots,static_cast<unsigned long long>(slots.bytes),static_cast<unsigned long long>(caps.cpu_byte_limit));
        if(references) receipt("BROKER_COORDINATOR_REFERENCE_CAPACITY profile=%u objectsVersion=%u resources=64 syncs=64 perActor=%u perActorSync=16 byteBudgetsMultiplied=false\n",wanted.mapping.aperture.version,objects.version,requestedReferences);
        if(objects.version>=6) receipt("BROKER_COORDINATOR_MANAGED_MAPPING_CAPACITY version=%u reservedAddressBytes=68719476736 standaloneMappingBytes=1073741824 residentUsageBytes=%llu cpuBytes=%llu\n",objects.version,static_cast<unsigned long long>(wanted.reportedGpuByteLimit),static_cast<unsigned long long>(caps.cpu_byte_limit));
        if(objects.version>=7) receipt("BROKER_COORDINATOR_DESKTOP_LEASE_CAPACITY version=%u leaseLimit=1536 fenceSlots=512 allocationSlots=1024 physicalMemoryGuardsChanged=%s\n",objects.version,multiapp?"true":"false");
        if(objects.version>=8) receipt("BROKER_COORDINATOR_FOUR_ACTOR_OBJECT_CAPACITY version=%u actors=4 hwQueues=64 syncObjects=768 perActorHwQueues=16 perActorSyncObjects=192 physicalMemoryGuardsChanged=%s\n",objects.version,multiapp?"true":"false");
        if(multiapp) receipt("BROKER_COORDINATOR_MULTIAPP_CPU_CAPACITY profile=14 objectsVersion=%u flags=%u cpuBytes=402653184 reportedGpuBytes=%llu cpuMappingGuardRaised=true gpuUsageGuardRaised=%s\n",
            objects.version,agreement.flags,static_cast<unsigned long long>(wanted.reportedGpuByteLimit),requestedMemory ? "true" : "false");
        if(requestedMemory) receipt("BROKER_COORDINATOR_APPLICATION_MEMORY_CAPACITY version=1 infoFlags=%u reportedGpuBytes=%llu sharedPool=true kernelChanged=false physicalPartition=false\n",
            agreement.flags,static_cast<unsigned long long>(wanted.reportedGpuByteLimit));
    }
    cg::Info info() const { return agreement; }
    void beginLiveRetirement() {
        const auto profile=decode<driver_bridge::LiveRetirementProfile>(control(cg::Operation::BeginLiveRetirement,encode(driver_bridge::LiveRetirementProfile{})));
        if(!driver_bridge::validLiveRetirementProfile(profile)) reject("Live retirement profile mismatch");
        liveRetirement=true;
        receipt("BROKER_COORDINATOR_LIVE_RETIREMENT version=1 batch=16 phases=4 trustedControllerOnly=true\n");
    }
    bool liveRetirementEnabled() const { return liveRetirement; }
    bool retirementWitnessesEnabled() const { return retirementWitnessFiles; }
    void retireStoppedGroup() {
        if(!liveRetirement) reject("Terminal group retirement not negotiated");
        try {
            const auto summary=driver_bridge::pollStoppedGroupRetirement([&] {
                return decode<driver_bridge::StoppedGroupRetirementProgress>(control(cg::Operation::RetireStoppedGroup));
            },[] { return std::chrono::steady_clock::now(); },[] { poll(nullptr,0,1); },agreement.maxActors);
            receipt("BROKER_COORDINATOR_PRIMARY_RETIRED destroyedWireObjects=%u steps=%u groupSealed=true\n",summary.destroyed,summary.attempts);
        } catch(...) { failed=true;throw; }
    }
    driver_bridge::LiveRetirementProgress retireExited(std::uint64_t actor) {
        if(!liveRetirement) reject("Live retirement not negotiated");
        const auto progress=decode<driver_bridge::LiveRetirementProgress>(control(cg::Operation::RetireExitedActor,encode(actor)));
        if(!driver_bridge::validLiveRetirementProgress(progress)) reject("Live retirement progress mismatch");
        return progress;
    }
    std::uint64_t attach(bool primary) {
        const auto id=decode<std::uint64_t>(control(cg::Operation::Register,encode(cg::Registration{primary ? cg::Role::Compositor : cg::Role::Application,0})));
        if(!id) reject("Coordinator actor identity absent");
        return id;
    }
    std::vector<std::uint8_t> call(std::uint64_t actor,const std::vector<std::uint8_t>& payload) {
        std::lock_guard<driver_bridge::FairRpcMutex> guard(lock);
        if(failed || actorSequence==UINT64_MAX || payload.size()<sizeof(driver_bridge::Header) || payload.size()>native_gpu::MaxPacket)
            reject("Coordinator actor call unavailable");
        ar::Envelope h;h.actor=actor;h.sequence=actorSequence++;h.bytes=static_cast<std::uint32_t>(payload.size());
        auto packet=encode(h);packet.insert(packet.end(),payload.begin(),payload.end());
        std::vector<std::uint8_t> reply;
        try { reply=port.exchange(packet); } catch(...) { failed=true;throw; }
        const auto r=at<ar::Envelope>(reply);
        if(r.magic!=ar::Magic || r.version!=ar::ProtocolVersion || r.operation!=h.operation || r.actor!=actor || r.sequence!=h.sequence ||
            r.reserved || r.padding || r.status || r.bytes!=reply.size()-sizeof r || r.bytes<sizeof(driver_bridge::Header) || r.bytes>native_gpu::MaxPacket)
            reject("Coordinator actor reply mismatch");
        return {reply.begin()+sizeof r,reply.end()};
    }
    driver_bridge::SharedReferenceAlias alias(cg::Alias value) {
        const auto result=decode<driver_bridge::SharedReferenceAlias>(control(cg::Operation::Alias,encode(value)));
        if(result.ntstatus>0 || (result.ntstatus ? result.ticket!=0 : !result.ticket)) reject("Coordinator alias reply mismatch");
        if(result.ntstatus) {
            const auto error=nativeAliasFailureErrno(result.ntstatus);
            receipt("BROKER_COORDINATOR_ALIAS_REJECTED kind=%u ntstatus=%d errno=%d nativeReplyPreserved=true\n",static_cast<unsigned>(value.kind),result.ntstatus,error);
            throw std::system_error(error,std::generic_category(),"Native recipient alias failed");
        }
        return result;
    }
    void disconnect(std::uint64_t actor) { if(!control(cg::Operation::Disconnect,encode(actor)).empty()) reject("Coordinator disconnect payload"); }
    bool release(std::uint64_t actor) {
        try { if(!control(cg::Operation::Release,encode(actor)).empty()) reject("Coordinator release payload");return true; }
        catch(const std::system_error& error) { if(error.code().value()==EBUSY) return false;throw; }
    }
};
class ActorPort {
    CoordinatorPort& owner;
    std::uint64_t id=0;
    const bool primary;
    const pid_t process;
    std::set<std::uint32_t> adapters;
    bool dead=false;
#if defined(WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
    DiagnosticAliasRefusal diagnosticRefusal;
#endif
public:
    ActorPort(CoordinatorPort& controller,bool presentationOwner,pid_t pid):owner(controller),primary(presentationOwner),process(pid)
#if defined(WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
        ,diagnosticRefusal(presentationOwner,WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
#endif
    {}
    std::uint64_t actor() const { return id; }
    cg::Info info() const { return owner.info(); }
    std::vector<std::uint8_t> exchange(const std::vector<std::uint8_t>& packet) {
        using namespace driver_bridge;
        if(packet.size()<sizeof(Header)) throw std::runtime_error("Actor native request truncated");
        if(!id && !dead) {
            id=owner.attach(primary);
            receipt("BROKER_COORDINATOR_ACTOR actor=%llu pid=%d primary=%s pidfdAuthenticated=true nativeDriverCount=1\n",
                static_cast<unsigned long long>(id),process,primary ? "true" : "false");
        }
        Header h{};std::memcpy(&h,packet.data(),sizeof h);std::vector<std::uint8_t> reply;
        if(dead) {
            if(packet.size()!=sizeof h || (h.type!=static_cast<std::uint32_t>(Op::CloseSharedResource) && h.type!=static_cast<std::uint32_t>(Op::CloseSharedSync)))
                throw std::runtime_error("Disconnected actor cannot submit native calls");
            reply=owner.control(cg::Operation::ReapReference,encode(cg::Reference{id,h.type==static_cast<std::uint32_t>(Op::CloseSharedSync) ? Kind::SharedSync : Kind::SharedResource,h.handle}));
        } else reply=owner.call(id,packet);
        if(const auto failure=residencyFailureObservation(packet,reply)) {
            receipt("BROKER_RESIDENCY_NATIVE_FAILURE actor=%llu pid=%d queue=%u requested=%u flags=%u returned=%u ntstatus=%d pagingFence=%llu bytesToTrim=%llu nativeReplyReceived=true clientDeliveryVerified=false\n",
                static_cast<unsigned long long>(id),process,failure->queue,failure->requested,failure->flags,
                failure->returned,failure->ntstatus,static_cast<unsigned long long>(failure->pagingFence),
                static_cast<unsigned long long>(failure->bytesToTrim));
        }
        if(reply.size()>=sizeof(Header)+sizeof(Reply)) {
            Header r{};Reply value{};std::memcpy(&r,reply.data(),sizeof r);std::memcpy(&value,reply.data()+sizeof r,sizeof value);
            if(!r.status && !value.ntstatus) {
                if(h.type==static_cast<std::uint32_t>(Op::OpenAdapter)) adapters.insert(r.handle);
                if(h.type==static_cast<std::uint32_t>(Op::CloseAdapter)) adapters.erase(h.handle);
#if defined(WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
                if(primary && h.type==static_cast<std::uint32_t>(Op::QueueCompositorFrame)) {
                    if(r.type!=h.type || reply.size()!=sizeof(Header)+sizeof(Reply))
                        throw std::runtime_error("Diagnostic compositor acknowledgement extent mismatch");
                    if(diagnosticRefusal.acceptedNativeFrame(value.value))
                        receipt("BROKER_DIAGNOSTIC_ALIAS_REFUSAL_ARMED actor=%llu pid=%d completedFrames=%llu threshold=%u actualNativeReplyAccepted=true testOnly=true gpuProof=false\n",
                            static_cast<unsigned long long>(id),process,static_cast<unsigned long long>(value.value),WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME);
                }
#endif
            }
        }
        return reply;
    }
    std::uint32_t alias(std::uint64_t donor,std::uint32_t ticket,std::uint32_t kind) {
        if(dead || adapters.empty()) throw std::system_error(EBADF,std::generic_category(),"Recipient has no adapter");
#if defined(WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
        if(diagnosticRefusal.refuseResourceAlias(kind==WDDM_OBJECT_RESOURCE)) {
            receipt("BROKER_DIAGNOSTIC_ALIAS_REFUSAL_INJECTED actor=%llu pid=%d donor=%llu sourceTicket=%u kind=%u completedFrames=%llu errno=%d origin=guest-before-native nativeAliasCalled=false testOnly=true resourcesReleased=false gpuProof=false\n",
                static_cast<unsigned long long>(id),process,static_cast<unsigned long long>(donor),ticket,kind,
                static_cast<unsigned long long>(diagnosticRefusal.completedFrames()),ENOMEM);
            throw DiagnosticAliasRefusalError();
        }
#endif
        return owner.alias(cg::Alias{donor,id,kind==WDDM_OBJECT_SYNC ? driver_bridge::Kind::SharedSync : driver_bridge::Kind::SharedResource,ticket,*adapters.begin(),0}).ticket;
    }
    void processExited() { if(id) owner.disconnect(id);dead=true; }
    void finish() {
        if(!id) { receipt("BROKER_COORDINATOR_METADATA_EXIT pid=%d nativeActorAllocated=false\n",process);return; }
        if(owner.liveRetirementEnabled() && !primary) {
            const auto started=std::chrono::steady_clock::now();
            const auto deadline=started+std::chrono::seconds(8);
            unsigned destroyed=0,steps=0;
            driver_bridge::LiveRetirementProgress progress;
            do {
                progress=owner.retireExited(id);++steps;destroyed+=progress.destroyed;
                if(!progress.remaining) break;
                poll(nullptr,0,1);
            } while(steps<512 && std::chrono::steady_clock::now()<deadline);
            receipt("BROKER_COORDINATOR_LIVE_RETIREMENT_TIMING actor=%llu elapsedMs=%lld steps=%u\n",
                static_cast<unsigned long long>(id),static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()),steps);
            if(!progress.remaining && owner.release(id)) {
                publishRetirementWitness(owner.retirementWitnessesEnabled(),process,id);
                receipt("BROKER_COORDINATOR_LIVE_RETIRED actor=%llu pid=%d destroyedWireObjects=%u steps=%u processExited=true cpuViewsRevoked=true sharedFdsDrained=true\n",
                    static_cast<unsigned long long>(id),process,destroyed,steps);
                receipt("BROKER_COORDINATOR_ACTOR_EXIT actor=%llu empty=true retainedUntilVmExit=false\n",static_cast<unsigned long long>(id));
                return;
            }
            receipt("BROKER_COORDINATOR_LIVE_DEFERRED actor=%llu pid=%d remaining=%u phase=%u flags=%u ntstatus=%d steps=%u\n",
                static_cast<unsigned long long>(id),process,progress.remaining,progress.phase,progress.flags,progress.ntstatus,steps);
        }
        const auto empty=owner.release(id);
        receipt("BROKER_COORDINATOR_ACTOR_EXIT actor=%llu empty=%s retainedUntilVmExit=%s\n",static_cast<unsigned long long>(id),empty ? "true" : "false",empty ? "false" : "true");
    }
};
// One kernel object controller gives every capability a globally unique
// ticket/cookie. Per-client views use separate controllers and pidfd lifetimes.
template<class Native,class Authority> class ActorObjects;
template<class Authority=KernelObjects> class ObjectRegistry {
    template<class Native,class A> friend class ActorObjects;
    struct Entry {
        ObjectRef global;
        std::uint64_t producer=0;
        std::uint32_t source=0;
        std::map<std::uint64_t,std::uint32_t> aliases;
        bool reaped=false;
    };
    Authority kernel;
    std::mutex mutex;
    std::map<std::uint32_t,Entry> entries;
    std::map<std::uint64_t,std::deque<ObjectRef>> pending;
    std::uint32_t nextTicket=1;
    void collect() {
        for(;;) {
            const auto ref=kernel.reap();if(!ref.ticket) break;
            const auto found=entries.find(ref.ticket);
            if(found==entries.end() || found->second.reaped || found->second.global.kind!=ref.kind || found->second.global.cookie!=ref.cookie)
                throw std::runtime_error("Coordinator global capability reap mismatch");
            auto& entry=found->second;entry.reaped=true;
            for(const auto& alias:entry.aliases) pending[alias.first].push_back(ObjectRef{ref.kind,alias.second,ref.cookie});
        }
    }
public:
    explicit ObjectRegistry(Authority value={}) : kernel(std::move(value)) {}
    bool empty() {
        std::lock_guard<std::mutex> guard(mutex);
        return entries.empty() && pending.empty();
    }
};
template<class Native,class Authority> class ActorObjects {
    ObjectRegistry<Authority>& registry;
    Native& native;
    ObjectStore local;
    static void require(bool ok,const char* message) { if(!ok) throw std::system_error(EPROTO,std::generic_category(),message); }
    static driver_bridge::ResourceSharingProfile resourceProfile(const Native& port) {
        const auto info=port.info();
        require(cg::validInfo(info),"Invalid authenticated actor capability agreement");
        return info.resource;
    }
public:
    ActorObjects(ObjectRegistry<Authority>& objects,Native& port):registry(objects),native(port),local(resourceProfile(port)) {}
    std::vector<std::uint8_t> coordinatedProfile() const { return encode(native.info()); }
    bool multiClient() const { return true; }
    // Authority methods translate kernel-global identities into this actor's
    // independent wire namespace. Callers never see or choose actor IDs.
    std::pair<Fd,ObjectRef> create(std::uint32_t ticket,std::uint32_t kind) {
        std::lock_guard<std::mutex> guard(registry.mutex);
        require(native.actor() && registry.nextTicket!=UINT32_MAX,"Global capability actor or ID absent");
        const auto global=registry.nextTicket++;auto result=registry.kernel.create(global,kind);
        typename ObjectRegistry<Authority>::Entry entry{result.second,native.actor(),ticket,{{native.actor(),ticket}},false};
        require(registry.entries.emplace(global,std::move(entry)).second,"Global capability collision");
        result.second.ticket=ticket;return result;
    }
    ObjectRef resolve(int fd,std::uint32_t kind) {
        std::lock_guard<std::mutex> guard(registry.mutex);
        const auto ref=registry.kernel.resolve(fd,kind);const auto found=registry.entries.find(ref.ticket);
        require(found!=registry.entries.end() && !found->second.reaped && ref.cookie && found->second.global.cookie==ref.cookie &&
                ref.kind==kind && found->second.global.kind==kind && native.actor(),"Foreign kernel capability identity mismatch");
        auto& entry=found->second;auto alias=entry.aliases.find(native.actor());
        if(alias==entry.aliases.end()) {
            std::uint32_t ticket=0;
            try { local.admitAlias(kind);ticket=native.alias(entry.producer,entry.source,kind); }
            catch(const std::system_error& error) {
#if defined(WDDM_BRIDGE_MULTIAPP_CPU) && WDDM_BRIDGE_MULTIAPP_CPU
                // Observe FD lifetime after alias refusal. Never
                // close a live capability or infer GPU completion from this view.
                try {
                registry.collect();
                unsigned live=0,reaped=0,pending=0;
                for(const auto& item:registry.entries) {
                    if(item.second.global.kind!=kind || !item.second.aliases.count(native.actor())) continue;
                    if(item.second.reaped) ++reaped;else ++live;
                    receipt("BROKER_ALIAS_REFUSAL_REFERENCE actor=%llu kind=%u cookie=%llu sourceActor=%llu sourceTicket=%u recipientTicket=%u kernelFdReaped=%s registryLocked=true gpuProof=false\n",
                        static_cast<unsigned long long>(native.actor()),kind,static_cast<unsigned long long>(item.second.global.cookie),
                        static_cast<unsigned long long>(item.second.producer),item.second.source,item.second.aliases.at(native.actor()),item.second.reaped?"true":"false");
                }
                const auto queued=registry.pending.find(native.actor());
                if(queued!=registry.pending.end()) for(const auto& item:queued->second) if(item.kind==kind) ++pending;
#if defined(WDDM_BRIDGE_DIAGNOSTIC_ALIAS_REFUSAL_FRAME)
                const bool injected=dynamic_cast<const DiagnosticAliasRefusalError*>(&error)!=nullptr;
                receipt("BROKER_ALIAS_REFUSAL_SNAPSHOT actor=%llu kind=%u errno=%d liveFdReferences=%u reapedFdReferences=%u pendingReaps=%u afterAliasAdmissionRefused=true diagnosticInjected=%s registryLocked=true resourcesReleased=false gpuProof=false\n",
                    static_cast<unsigned long long>(native.actor()),kind,error.code().value(),live,reaped,pending,injected?"true":"false");
#else
                receipt("BROKER_ALIAS_REFUSAL_SNAPSHOT actor=%llu kind=%u errno=%d liveFdReferences=%u reapedFdReferences=%u pendingReaps=%u afterNativeAdmissionRefused=true registryLocked=true resourcesReleased=false gpuProof=false\n",
                    static_cast<unsigned long long>(native.actor()),kind,error.code().value(),live,reaped,pending);
#endif
                } catch(...) {
                    receipt("BROKER_ALIAS_REFUSAL_SNAPSHOT_UNAVAILABLE actor=%llu originalErrno=%d resourcesReleased=false originalErrorPreserved=true\n",
                        static_cast<unsigned long long>(native.actor()),error.code().value());
                }
#endif
                (void)error;
                throw;
            }
            local.retainAlias(ObjectRef{kind,ticket,ref.cookie});
            alias=entry.aliases.emplace(native.actor(),ticket).first;
            receipt("BROKER_COORDINATOR_ALIAS donor=%llu recipient=%llu kind=%u sourceTicket=%u recipientTicket=%u cookie=%llu realFd=true\n",
                static_cast<unsigned long long>(entry.producer),static_cast<unsigned long long>(native.actor()),kind,entry.source,ticket,static_cast<unsigned long long>(ref.cookie));
        }
        return {kind,alias->second,ref.cookie};
    }
    ObjectRef reap() {
        std::lock_guard<std::mutex> guard(registry.mutex);registry.collect();
        auto& pending=registry.pending[native.actor()];if(pending.empty()) return {};
        const auto result=pending.front();pending.pop_front();return result;
    }
    wddm_object_stats info() { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.info(); }
    std::pair<Fd,std::uint64_t> resolveSyncFile(int fd) { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.resolveSyncFile(fd); }
    Fd nativeBuffer(int fd,std::uint64_t bytes) { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBuffer(fd,bytes); }
    std::uint64_t nativeBufferInfo(int fd) { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBufferInfo(fd); }
    wddm_native_buffer_stats nativeBufferStats() { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBufferStats(); }
    wddm_native_buffer_caps nativeBufferCaps() { std::lock_guard<std::mutex> guard(registry.mutex);return registry.kernel.nativeBufferCaps(); }
    template<class A> void authorize(const std::vector<std::uint8_t>& packet,const std::vector<Fd>& fds,A&) { local.authorize(packet,fds,*this); }
    void observe(const std::vector<std::uint8_t>& packet,const std::vector<std::uint8_t>& reply) {
        local.observe(packet,reply);
        using namespace driver_bridge;
        Header h{},r{};std::memcpy(&h,packet.data(),sizeof h);std::memcpy(&r,reply.data(),sizeof r);
        if(r.status || reply.size()!=sizeof(Header)+sizeof(Reply) ||
            (h.type!=static_cast<std::uint32_t>(Op::CloseSharedSync) && h.type!=static_cast<std::uint32_t>(Op::CloseSharedResource))) return;
        Reply status{};std::memcpy(&status,reply.data()+sizeof r,sizeof status);if(status.ntstatus) return;
        std::lock_guard<std::mutex> guard(registry.mutex);
        for(auto i=registry.entries.begin();i!=registry.entries.end();++i) {
            const auto alias=i->second.aliases.find(native.actor());
            if(alias==i->second.aliases.end() || alias->second!=h.handle) continue;
            require(i->second.reaped,"Native alias closed before global FD reap");i->second.aliases.erase(alias);
            if(i->second.aliases.empty()) registry.entries.erase(i);
            break;
        }
    }
    template<class A> std::pair<Fd,ObjectRef> create(ObjectRequest request,A&) { return local.create(request,*this); }
    template<class A> ObjectRef resolve(int fd,std::uint32_t kind,A&) { return local.resolve(fd,kind,*this); }
    template<class A> void validate(int fd,std::uint32_t kind,A&) {
        std::lock_guard<std::mutex> guard(registry.mutex);
        const auto ref=registry.kernel.resolve(fd,kind);const auto found=registry.entries.find(ref.ticket);
        require(found!=registry.entries.end() && !found->second.reaped && ref.cookie && found->second.global.cookie==ref.cookie &&
                ref.kind==kind && found->second.global.kind==kind,"Kernel capability validation mismatch");
    }
    template<class A> ObjectRef reap(A&) { return local.reap(*this); }
    template<class A> void syncLineage(int fd,std::uint32_t source,A&) { local.syncLineage(fd,source,*this); }
    template<class N,class A> void drain(N&,A&) {
        native.processExited();local.drain(native,*this);
        {
            std::lock_guard<std::mutex> guard(registry.mutex);
            for(auto i=registry.entries.begin();i!=registry.entries.end();) {
                if(i->second.aliases.erase(native.actor())) require(i->second.reaped,"Actor drained before FD lifetime ended");
                if(i->second.aliases.empty()) i=registry.entries.erase(i);else ++i;
            }
            registry.pending.erase(native.actor());
        }
        native.finish();
    }
    void report() const { local.report(); }
};
} // namespace guest_broker
