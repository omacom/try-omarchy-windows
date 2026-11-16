// SPDX-License-Identifier: MIT
// Coordinator building block. No production transport or multi-client opt-in.
// The VM owner retains Session and Driver lifetimes; this router never destroys
// live GPU resources on EOF. A disconnected actor remains charged until empty.
#pragma once
#include "driver_wire.h"
#include <functional>
#include <limits>

namespace driver_bridge::actors {
constexpr std::uint32_t Magic=0x54434157; // WACT, little endian
constexpr std::uint16_t ProtocolVersion=1;
constexpr std::size_t MaxActors=4;
constexpr std::size_t MaxApplicationActors=8;
constexpr bool validActorLimit(std::size_t value) { return value==MaxActors || value==MaxApplicationActors; }
enum class Operation : std::uint16_t { Call=1, Disconnect=2, Close=3 };
struct Envelope {
    std::uint32_t magic=Magic;
    std::uint16_t version=ProtocolVersion;
    Operation operation=Operation::Call;
    std::uint64_t actor=0,sequence=0;
    std::uint32_t bytes=0,reserved=0;
    std::int32_t status=0;
    std::uint32_t padding=0;
};
static_assert(sizeof(Envelope)==40 && offsetof(Envelope,actor)==8 && offsetof(Envelope,status)==32,"Fixed actor envelope");
constexpr std::size_t MaxEnvelopeBytes=sizeof(Envelope)+native_gpu::MaxPacket;
class Router {
    struct Actor { std::reference_wrapper<Session> session; bool disconnected=false; std::uint64_t calls=0; };
    std::map<std::uint64_t,Actor> actors;
    std::uint64_t nextActor=1,nextSequence=1;
    bool poisoned=false;
    std::size_t actorLimit=MaxActors;
    static std::vector<std::uint8_t> response(Envelope h,std::int32_t status,const std::vector<std::uint8_t>& payload={}) {
        h.status=status;h.bytes=static_cast<std::uint32_t>(payload.size());h.reserved=h.padding=0;
        std::vector<std::uint8_t> out(sizeof h+payload.size());std::memcpy(out.data(),&h,sizeof h);
        if(!payload.empty()) std::memcpy(out.data()+sizeof h,payload.data(),payload.size());
        return out;
    }
public:
    Router()=default;
    Router(const Router&)=delete;
    Router& operator=(const Router&)=delete;
    Router(Router&&)=delete;
    Router& operator=(Router&&)=delete;
    // The trusted coordinator selects this once before registering processes.
    // Additional process namespaces do not multiply Driver memory budgets.
    bool configureActorLimit(std::size_t value) {
        if(poisoned || !actors.empty() || nextActor!=1 || !validActorLimit(value)) return false;
        actorLimit=value;return true;
    }
    // Trusted coordinator-only registration. A guest packet cannot create or
    // select a native Session. IDs never repeat, including after empty close.
    std::uint64_t attach(Session& session) {
        if(poisoned || actors.size()>=actorLimit || nextActor==UINT64_MAX) return 0;
        for(const auto& item:actors) if(&item.second.session.get()==&session) return 0;
        const auto id=nextActor++;
        actors.emplace(id,Actor{std::ref(session),false,0});return id;
    }
    std::size_t size() const { return actors.size(); }
    bool failed() const { return poisoned; }
    bool disconnected(std::uint64_t id) const {
        const auto found=actors.find(id);return found!=actors.end() && found->second.disconnected;
    }
    // Owner lifecycle operations do not consume the client call sequence.
    bool disconnect(std::uint64_t id) {
        const auto found=actors.find(id);if(found==actors.end()) return false;
        found->second.disconnected=true;return true;
    }
    bool detachEmpty(std::uint64_t id) {
        const auto found=actors.find(id);
        if(found==actors.end() || found->second.session.get().objectCount()) return false;
        actors.erase(found);return true;
    }
    std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& packet) {
        if(poisoned) return {};
        if(packet.size()<sizeof(Envelope)) { poisoned=true;return {}; }
        Envelope h{};std::memcpy(&h,packet.data(),sizeof h);
        if(packet.size()>MaxEnvelopeBytes || h.magic!=Magic || h.version!=ProtocolVersion || !h.actor || h.status ||
            h.reserved || h.padding || h.bytes!=packet.size()-sizeof h || h.sequence!=nextSequence || nextSequence==UINT64_MAX ||
            (h.operation!=Operation::Call && h.operation!=Operation::Disconnect && h.operation!=Operation::Close) ||
            (h.operation==Operation::Call ? h.bytes<sizeof(Header) : h.bytes!=0)) {
            poisoned=true;return response(h,-71);
        }
        ++nextSequence;
        const auto found=actors.find(h.actor);if(found==actors.end()) return response(h,-9);
        auto& actor=found->second;auto& session=actor.session.get();
        if(h.operation==Operation::Disconnect) { actor.disconnected=true;return response(h,0); }
        if(h.operation==Operation::Close) {
            if(session.objectCount()) return response(h,-16);
            actors.erase(found);return response(h,0);
        }
        if(actor.disconnected) return response(h,-32);
        if(!lifetimeBudgetAvailable(actor.calls,session.requestLimit())) return response(h,-24);
        // Existing Session validation rewrites all nested handles inside this
        // actor's namespace. Native Driver byte/object/pending budgets remain
        // shared; routing must not reset them or renegotiate a global profile.
        std::vector<std::uint8_t> payload(packet.begin()+sizeof h,packet.end());
        ++actor.calls;const auto result=session.dispatch(payload);
        if(result.size()<sizeof(Header) || result.size()>native_gpu::MaxPacket) { poisoned=true;return response(h,-5); }
        return response(h,0,result);
    }
};
} // namespace driver_bridge::actors
