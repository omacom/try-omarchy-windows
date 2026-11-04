// SPDX-License-Identifier: MIT
// One owned VM, one native Driver, independent ordinary-client Sessions.
// This channel is exclusively for the privileged guest controller. It must
// never be exposed as an ordinary client's broker protocol.
#pragma once
#include <chrono>
#include "driver_actor_router.h"
#include <memory>
#include <set>

namespace driver_bridge::coordinator {
constexpr std::uint32_t Magic=0x55504743; // CGPU
constexpr std::uint16_t ProtocolVersion=5;
enum class Operation : std::uint16_t { Initialize=1, Info, Register, Alias, ReapReference, Disconnect, Release, BeginLiveRetirement, RetireExitedActor, RetireStoppedGroup };
enum class Role : std::uint32_t { Application=1, Compositor=2 };
struct Frame {
    std::uint32_t magic=Magic;
    std::uint16_t version=ProtocolVersion;
    Operation operation=Operation::Info;
    std::uint64_t sequence=0;
    std::uint32_t bytes=0;
    std::int32_t status=0;
    std::uint64_t reserved=0;
};
struct Registration { Role role=Role::Application; std::uint32_t reserved=0; };
struct Alias {
    std::uint64_t donor=0,recipient=0;
    Kind kind{};
    std::uint32_t ticket=0,adapter=0,reserved=0;
};
struct Reference { std::uint64_t actor=0; Kind kind{}; std::uint32_t ticket=0; };
struct Initialization { OpenGlDesktopPresentationCapacity capacity{};CoordinatedObjectCapacity objects{}; };
struct ApplicationInitialization { Initialization base{};std::uint32_t actorSlots=8,reserved=0; };
struct ApplicationReferenceInitialization { ApplicationInitialization base{};std::uint32_t resourceReferences=ApplicationResourceReferences,reserved=0; };
struct ApplicationMemoryInitialization { ApplicationReferenceInitialization base{};ApplicationMemoryProfile memory{}; };
constexpr std::uint32_t ApplicationMemoryFlag=4;
constexpr std::uint32_t ApplicationObjectsFlag=8;
constexpr std::uint32_t ApplicationActorsFlag=1;
constexpr std::uint32_t ApplicationReferencesFlag=2;
constexpr std::uint32_t ApplicationMemoryFlags=ApplicationActorsFlag|ApplicationReferencesFlag|ApplicationMemoryFlag;
constexpr std::uint32_t ApplicationObjectsFlags=ApplicationMemoryFlags|ApplicationObjectsFlag;
struct Info {
    OpenGlDesktopPresentationCapacity capacity{};
    CoordinatedObjectCapacity objects{};
    OpenGlProfile gl{};
    SyncSharingProfile sync{};
    ResourceSharingProfile resource{};
    native_gpu::Capabilities caps{};
    std::uint32_t maxActors=static_cast<std::uint32_t>(actors::MaxActors),flags=0,reserved=0;
};
static_assert(sizeof(Frame)==32 && offsetof(Frame,sequence)==8 && offsetof(Frame,reserved)==24,"Coordinator framing");
static_assert(sizeof(Registration)==8 && sizeof(Alias)==32 && sizeof(Reference)==16 && sizeof(Initialization)==80 && sizeof(Info)==176,"Coordinator control payloads");
static_assert(sizeof(ApplicationInitialization)==88,"Explicit application process capacity payload");
static_assert(sizeof(ApplicationReferenceInitialization)==96,"Explicit application resource reference capacity payload");
static_assert(sizeof(ApplicationMemoryInitialization)==112,"Explicit application memory capacity payload");
constexpr std::size_t MaxPacket=actors::MaxEnvelopeBytes;
static_assert(MaxCoordinatedSharedReferences==actors::MaxActors*MaxSharedResourceHandles &&
              MaxCoordinatedSharedReferences==actors::MaxActors*MaxSharedSyncHandles,"Fixed v5 group and per-actor reference bounds");
static_assert(GuestCoordinatedActorObjects.hwQueues==actors::MaxActors*OpenGlFenceHwQueues &&
              GuestCoordinatedActorObjects.syncObjects==actors::MaxActors*OpenGlCompositorSyncObjects,
              "v8 object metadata covers the existing per-actor quotas");
inline bool validInfo(const Info& value) {
    const bool memory=(value.flags&ApplicationMemoryFlag)!=0;
    const bool objects=(value.flags&ApplicationObjectsFlag)!=0;
    if(objects != (value.objects.version==10) || (objects && (!memory || value.flags!=ApplicationObjectsFlags))) return false;
    return validCoordinatedAgreement(value.capacity,value.objects,memory) && validCoordinatedPresentationProfile(value.gl) &&
        value.gl.version==value.capacity.mapping.aperture.version && validSyncSharingProfile(value.sync) &&
        value.caps.version==Version && value.caps.vendor==0x10de && !value.reserved &&
        ((value.maxActors==actors::MaxActors && !value.flags && validResourceSharingProfile(value.resource)) ||
         (value.maxActors==actors::MaxApplicationActors && value.capacity.mapping.aperture.version==14 && (value.objects.version==9 || value.objects.version==10) &&
          ((value.flags==ApplicationActorsFlag && validResourceSharingProfile(value.resource)) ||
           ((value.flags==(ApplicationActorsFlag|ApplicationReferencesFlag) ||
             value.flags==(ApplicationActorsFlag|ApplicationReferencesFlag|ApplicationMemoryFlag) || value.flags==ApplicationObjectsFlags) &&
            validApplicationResourceSharingProfile(value.resource)))));
}
inline bool matchesApplicationMemoryCapacity(OpenGlDesktopPresentationCapacity capacity,const Info& info) {
    return validInfo(info) && (info.flags==(ApplicationActorsFlag|ApplicationReferencesFlag|ApplicationMemoryFlag) || info.flags==ApplicationObjectsFlags) &&
        validApplicationMemoryCapacity(capacity) && !std::memcmp(&capacity,&info.capacity,sizeof capacity);
}
class Runtime {
    Driver& driver;
    std::unique_ptr<CoordinatedProfiles> profiles;
    OpenGlDesktopPresentationCapacity capacity{};
    CoordinatedObjectCapacity objectCapacity{};
    std::map<std::uint64_t,std::unique_ptr<Session>> sessions;
    actors::Router router;
    std::uint64_t nextSequence=1;
    bool poisoned=false,primaryClaimed=false;
    bool liveRetirement=false,terminalRetirement=false,presentationStopped=false;
    std::uint64_t primaryActor=0,retiredActors=0,retiredObjects=0;
    std::uint64_t lastTerminalActor=0;
    std::uint32_t permittedActorLimit=static_cast<std::uint32_t>(actors::MaxActors);
    std::uint32_t actorLimit=static_cast<std::uint32_t>(actors::MaxActors);
    std::uint32_t permittedResourceReferences=MaxSharedResourceHandles;
    bool permittedApplicationMemory=false;
    bool permittedApplicationObjects=false;
    std::set<std::uint64_t> retired;
    static std::vector<std::uint8_t> response(Frame h,std::int32_t status,const std::vector<std::uint8_t>& payload={}) {
        h.status=status;h.bytes=static_cast<std::uint32_t>(payload.size());h.reserved=0;
        std::vector<std::uint8_t> out(sizeof h+payload.size());std::memcpy(out.data(),&h,sizeof h);
        if(!payload.empty()) std::memcpy(out.data()+sizeof h,payload.data(),payload.size());
        return out;
    }
    template<class T> static std::vector<std::uint8_t> encoded(const T& value) {
        std::vector<std::uint8_t> out(sizeof value);std::memcpy(out.data(),&value,sizeof value);return out;
    }
    template<class T> static bool payload(const std::vector<std::uint8_t>& packet,T& value) {
        if(packet.size()!=sizeof(Frame)+sizeof value) return false;
        std::memcpy(&value,packet.data()+sizeof(Frame),sizeof value);return true;
    }
    static bool referenceKind(Kind kind) { return kind==Kind::SharedResource || kind==Kind::SharedSync; }
public:
    explicit Runtime(Driver& value,std::uint32_t permittedActors=static_cast<std::uint32_t>(actors::MaxActors),
                     std::uint32_t resourceReferences=MaxSharedResourceHandles,bool applicationMemory=false,bool applicationObjects=false)
        : driver(value),permittedActorLimit(permittedActors),permittedResourceReferences(resourceReferences),permittedApplicationMemory(applicationMemory),permittedApplicationObjects(applicationObjects) {
        if(!actors::validActorLimit(permittedActors)) throw std::invalid_argument("Invalid owner actor capacity");
        if((resourceReferences!=MaxSharedResourceHandles && resourceReferences!=ApplicationResourceReferences) ||
           (resourceReferences==ApplicationResourceReferences && permittedActors!=actors::MaxApplicationActors))
            throw std::invalid_argument("Invalid owner resource reference capacity");
        if(applicationMemory && (permittedActors!=actors::MaxApplicationActors || resourceReferences!=ApplicationResourceReferences))
            throw std::invalid_argument("Application memory requires explicit application process and reference capacity");
        if(applicationObjects && !applicationMemory)
            throw std::invalid_argument("Application objects require explicit application memory permission");
    }
    Runtime(const Runtime&)=delete;
    Runtime& operator=(const Runtime&)=delete;
    // The owner must stop/reap its VM before this object's destruction. A
    // disconnected Session with live objects stays charged and owned here.
    bool failed() const { return poisoned || router.failed(); }
    std::size_t actorCount() const { return sessions.size(); }
    std::uint64_t liveRetiredActorCount() const { return retiredActors; }
    std::uint64_t liveRetiredObjectCount() const { return retiredObjects; }
    std::size_t objectCount() const {
        std::size_t total=0;for(const auto& entry:sessions) total+=entry.second->objectCount();return total;
    }
    bool teardownAfterVmExit(bool vmReaped) {
        if(!vmReaped) throw std::runtime_error("Coordinated teardown requires proven VM exit");
        for(unsigned phase=0;phase<4;++phase) for(auto& entry:sessions) entry.second->ownerTeardownPhase(phase);
        return !objectCount();
    }
    std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& packet) {
        if(failed()) return {};
        std::uint32_t magic=0;
        if(packet.size()<sizeof magic || packet.size()>MaxPacket) { poisoned=true;return {}; }
        std::memcpy(&magic,packet.data(),sizeof magic);
        if(magic==actors::Magic) {
            actors::Envelope h{};
            if(packet.size()<sizeof h) { poisoned=true;return {}; }
            std::memcpy(&h,packet.data(),sizeof h);
            if(!profiles || terminalRetirement || h.operation!=actors::Operation::Call) { poisoned=true;return {}; }
            return router.dispatch(packet);
        }
        if(magic!=Magic || packet.size()<sizeof(Frame)) { poisoned=true;return {}; }
        Frame h{};std::memcpy(&h,packet.data(),sizeof h);
        if(h.version!=ProtocolVersion || h.status || h.reserved || h.bytes!=packet.size()-sizeof h ||
            h.sequence!=nextSequence || nextSequence==UINT64_MAX ||
            h.operation<Operation::Initialize || h.operation>Operation::RetireStoppedGroup) {
            poisoned=true;return response(h,-71);
        }
        ++nextSequence;
        if(terminalRetirement && h.operation!=Operation::RetireStoppedGroup && h.operation!=Operation::Info)
            return response(h,-32);
        if(h.operation==Operation::Initialize) {
            Initialization wanted{};
            std::uint32_t requestedActors=static_cast<std::uint32_t>(actors::MaxActors);
            std::uint32_t requestedReferences=MaxSharedResourceHandles;
            bool requestedMemory=false;
            if(profiles) return response(h,-16);
            if(!payload(packet,wanted)) {
                ApplicationInitialization application{};
                if(!payload(packet,application)) {
                    ApplicationReferenceInitialization references{};
                    if(!payload(packet,references)) {
                        ApplicationMemoryInitialization memory{};
                        if(!payload(packet,memory) || !permittedApplicationMemory || !validApplicationMemoryProfile(memory.memory))
                            return response(h,-22);
                        references=memory.base;requestedMemory=true;
                    }
                    if(references.reserved ||
                       references.resourceReferences!=ApplicationResourceReferences ||
                       references.resourceReferences>permittedResourceReferences) return response(h,-22);
                    application=references.base;requestedReferences=references.resourceReferences;
                }
                if(application.reserved ||
                   application.actorSlots!=actors::MaxApplicationActors || application.actorSlots>permittedActorLimit ||
                   application.base.capacity.mapping.aperture.version!=14 || (application.base.objects.version!=9 && application.base.objects.version!=10))
                    return response(h,-22);
                wanted=application.base;requestedActors=application.actorSlots;
            }
            if(!validCoordinatedAgreement(wanted.capacity,wanted.objects,requestedMemory)) return response(h,-22);
            if(wanted.objects.version==10 && (!permittedApplicationObjects || !requestedMemory)) return response(h,-22);
            // A partial native initialization cannot be retried. The owner
            // catches the exception and stops the VM before releasing state.
            poisoned=true;
            auto group=std::make_unique<CoordinatedProfiles>(driver,wanted.capacity,wanted.objects,requestedReferences,requestedMemory);
            if(!router.configureActorLimit(requestedActors)) throw std::runtime_error("Actor capacity cannot be configured");
            profiles=std::move(group);capacity=wanted.capacity;objectCapacity=wanted.objects;
            actorLimit=requestedActors;poisoned=false;
            if(actorLimit>actors::MaxActors)
                std::cerr<<"{\"nativeApplicationActorCapacityNegotiated\":true,\"actorSlots\":"<<actorLimit
                    <<",\"hwQueues\":"<<objectCapacity.hwQueues<<",\"syncObjects\":"<<objectCapacity.syncObjects
                    <<",\"memoryBudgetsMultiplied\":false,\"trustedControllerOnly\":true}\n";
            if(requestedReferences>MaxSharedResourceHandles)
                std::cerr<<"{\"nativeApplicationReferenceCapacityNegotiated\":true,\"perActorResourceReferences\":"<<requestedReferences
                    <<",\"groupResourceReferences\":"<<MaxCoordinatedSharedReferences
                    <<",\"perActorSyncReferences\":"<<MaxSharedSyncHandles
                    <<",\"memoryBudgetsMultiplied\":false,\"trustedControllerOnly\":true}\n";
            if(requestedMemory)
                std::cerr<<"{\"nativeApplicationMemoryCapacityNegotiated\":true,\"reportedGpuBytes\":"<<capacity.reportedGpuByteLimit
                    <<",\"actorSlots\":"<<actorLimit<<",\"sharedAggregatePool\":true,\"trustedControllerOnly\":true}\n";
            if(objectCapacity.version==10)
                std::cerr<<"{\"nativeApplicationObjectsNegotiated\":true,\"version\":10,\"allocationObjects\":2048,\"perActorAllocationLimitUnchanged\":true,\"byteBudgetsUnchanged\":true,\"trustedControllerOnly\":true}\n";
            return response(h,0);
        }
        if(!profiles) return response(h,-71);
        if(h.operation==Operation::RetireStoppedGroup) {
            if(h.bytes) return response(h,-22);
            if(!liveRetirement || !primaryClaimed) return response(h,-95);
            // Disconnected means the trusted broker observed pidfd exit and
            // revoked CPU views. All shared capabilities must be drained before
            // sealing the group; deferred empty-process actors may remain.
            for(const auto& entry:sessions)
                if(!router.disconnected(entry.first) || entry.second->hasSharedReferences())
                    return response(h,-16);
            terminalRetirement=true;
            if(!presentationStopped) {
                const auto state=driver.pollCoordinatedPresentationStop();
                if(state==PresentationStopProgress::Pending) return response(h,-16);
                if(state!=PresentationStopProgress::Complete) {
                    poisoned=true;throw std::runtime_error("Terminal presentation drain failed");
                }
                presentationStopped=true;
            }
            StoppedGroupRetirementProgress result;result.step.phase=4;
            auto& progress=result.step;
            if(!sessions.empty()) {
                // Other actors may need to release queues/devices before this
                // actor's native parent can close. Retry fairly across actors.
                auto actor=sessions.upper_bound(lastTerminalActor);
                if(actor==sessions.end()) actor=sessions.begin();
                result.actor=lastTerminalActor=actor->first;
                try { progress=actor->first==primaryActor ? actor->second->retireStoppedPrimary() : actor->second->retireExitedActor(); }
                catch(...) { poisoned=true;throw; }
                if(!validLiveRetirementProgress(progress)) throw std::runtime_error("Malformed terminal retirement progress");
                if(actor->first!=primaryActor) retiredObjects+=progress.destroyed;
                if(!progress.remaining) {
                    if(!router.detachEmpty(actor->first)) throw std::runtime_error("Terminal actor still owns objects");
                    if(actor->first!=primaryActor) ++retiredActors;
                    retired.erase(actor->first);
                    sessions.erase(actor);
                }
            }
            result.actorsRemaining=static_cast<std::uint32_t>(sessions.size());
            if(!validStoppedGroupRetirementProgress(result,actorLimit)) throw std::runtime_error("Malformed terminal group progress");
            std::cerr<<"{\"nativeStoppedGroupRetirement\":true,\"primaryActor\":"<<primaryActor<<",\"phase\":"<<progress.phase
                <<",\"remaining\":"<<progress.remaining<<",\"attempted\":"<<progress.attempted<<",\"destroyed\":"<<progress.destroyed
                <<",\"sharedReferences\":"<<progress.sharedReferences<<",\"flags\":"<<progress.flags<<",\"ntstatus\":"<<progress.ntstatus
                <<",\"actor\":"<<result.actor<<",\"actorsRemaining\":"<<result.actorsRemaining
                <<",\"presentationDrained\":true,\"groupSealed\":true,\"ordinaryClientOpcode\":false}\n";
            return response(h,0,encoded(result));
        }
        if(h.operation==Operation::BeginLiveRetirement) {
            LiveRetirementProfile wanted{};
            if(liveRetirement || !sessions.empty()) return response(h,-16);
            if(!payload(packet,wanted) || !validLiveRetirementProfile(wanted)) return response(h,-22);
            if((capacity.mapping.aperture.version!=13 && capacity.mapping.aperture.version!=14) || objectCapacity.version<5 || !driver.supportsLiveRetirement()) return response(h,-95);
            liveRetirement=true;
            std::cerr<<"{\"nativeLiveRetirementEnabled\":true,\"version\":1,\"batch\":16,\"phases\":4,\"ordinaryClientOpcode\":false}\n";
            return response(h,0,encoded(wanted));
        }
        if(h.operation==Operation::Info) {
            if(h.bytes) return response(h,-22);
            const Info value{capacity,objectCapacity,profiles->openGl(),profiles->syncSharing(),profiles->resourceSharing(),profiles->capabilities(),
                             actorLimit,(actorLimit==actors::MaxApplicationActors ? ApplicationActorsFlag : 0u)|
                             (profiles->resourceSharing().version==2 ? ApplicationReferencesFlag : 0u)|
                             (profiles->applicationMemory() ? ApplicationMemoryFlag : 0u)|
                             (objectCapacity.version==10 ? ApplicationObjectsFlag : 0u),0};
            return response(h,0,encoded(value));
        }
        if(h.operation==Operation::Register) {
            Registration request{};
            if(!payload(packet,request) || request.reserved ||
                (request.role!=Role::Application && request.role!=Role::Compositor)) return response(h,-22);
            if(sessions.size()>=actorLimit || (request.role==Role::Compositor && primaryClaimed)) return response(h,-24);
            auto session=std::make_unique<Session>(driver,*profiles,request.role==Role::Compositor);
            const auto id=router.attach(*session);if(!id) return response(h,-24);
            try { sessions.emplace(id,std::move(session)); }
            catch(...) { (void)router.detachEmpty(id);throw; }
            if(request.role==Role::Compositor) { primaryClaimed=true;primaryActor=id; }
            return response(h,0,encoded(id));
        }
        if(h.operation==Operation::Alias) {
            Alias request{};
            if(!payload(packet,request) || request.reserved || !referenceKind(request.kind) || !request.ticket || !request.adapter)
                return response(h,-22);
            const auto donor=sessions.find(request.donor),recipient=sessions.find(request.recipient);
            if(donor==sessions.end() || recipient==sessions.end() || router.disconnected(request.recipient)) return response(h,-9);
            const auto result=recipient->second->aliasSharedReference(*donor->second,request.kind,request.ticket,request.adapter);
            return response(h,0,encoded(result));
        }
        if(h.operation==Operation::ReapReference) {
            Reference request{};
            if(!payload(packet,request) || !request.ticket || !referenceKind(request.kind)) return response(h,-22);
            const auto actor=sessions.find(request.actor);if(actor==sessions.end()) return response(h,-9);
            const Header closing{static_cast<std::uint32_t>(request.kind==Kind::SharedSync ? Op::CloseSharedSync : Op::CloseSharedResource),request.ticket,0,0};
            // Only a kernel-confirmed capability reap may use this control.
            // It deliberately cannot submit work or destroy devices/queues.
            return response(h,0,actor->second->dispatch(encoded(closing)));
        }
        std::uint64_t id=0;
        if(!payload(packet,id) || !id) return response(h,-22);
        const auto actor=sessions.find(id);if(actor==sessions.end()) return response(h,-9);
        if(h.operation==Operation::Disconnect) { (void)router.disconnect(id);return response(h,0); }
        if(!router.disconnected(id)) return response(h,-16);
        if(h.operation==Operation::RetireExitedActor) {
            if(!liveRetirement) return response(h,-95);
            if(id==primaryActor) return response(h,-13);
            LiveRetirementProgress progress;
            const auto started=std::chrono::steady_clock::now();
            try { progress=actor->second->retireExitedActor(); }
            catch(...) { poisoned=true;throw; }
            if(!validLiveRetirementProgress(progress)) throw std::runtime_error("Malformed live retirement progress");
            retiredObjects+=progress.destroyed;
            if(!progress.remaining) retired.insert(id);
            std::cerr<<"{\"nativeLiveRetirementStep\":true,\"actor\":"<<id<<",\"phase\":"<<progress.phase
                <<",\"remaining\":"<<progress.remaining<<",\"attempted\":"<<progress.attempted<<",\"destroyed\":"<<progress.destroyed
                <<",\"sharedReferences\":"<<progress.sharedReferences<<",\"flags\":"<<progress.flags<<",\"ntstatus\":"<<progress.ntstatus
                <<",\"gpuQuiescenceVerified\":"<<(progress.attempted ? "true" : "false")
                <<",\"nativeElapsedUs\":"<<std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count()<<"}\n";
            return response(h,0,encoded(progress));
        }
        if(!router.detachEmpty(id)) return response(h,-16);
        if(retired.erase(id)) ++retiredActors;
        const auto activity=actor->second->acceptedSubmissionActivity();
        std::cerr<<"{\"nativeActorSubmissionSummary\":true,\"actor\":"<<id
                 <<",\"acceptedCommands\":"<<activity.acceptedCommands
                 <<",\"asynchronousCommands\":"<<activity.asynchronousCommands
                 <<",\"acceptedCommandBytes\":"<<activity.acceptedCommandBytes
                 <<",\"countsExact\":"<<(activity.exact ? "true" : "false")
                 <<",\"validatedNativeRepliesOnly\":true,\"queueAcceptanceOnly\":true"
                 <<",\"wireObjectsRemaining\":0,\"beforeGlobalTeardown\":true}\n";
        sessions.erase(actor);return response(h,0);
    }
};
} // namespace driver_bridge::coordinator
