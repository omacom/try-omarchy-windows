// SPDX-License-Identifier: MIT
// Guest-only supervisor/peer lifetime. No GPU admission or input policy changes.
#pragma once
#include "guest_broker_peer.h"
#include "guest_broker_receipts.h"
#include "guest_primary_identity.h"
#include "driver_actor_router.h"
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <memory>
#include <optional>
#include <thread>

namespace guest_broker {
class SessionPolicy {
public:
    using Clock=std::chrono::steady_clock;
private:
    bool session;
    Fd supervisor;
    Clock::time_point startupDeadline;
    std::optional<Clock::time_point> drainDeadline;
public:
    static bool requested(const char* value) {
        if(!value) return false;
        if(std::strcmp(value,"1")) throw std::runtime_error("Invalid root broker session opt-in");
        return true;
    }
    static bool retirementWitnesses(const char* value,bool enabled) {
        if(!value || !std::strcmp(value,"1")) return true;
        if(std::strcmp(value,"0") || !enabled)
            throw std::runtime_error("Disabling retirement witnesses requires the root broker session");
        return false;
    }
    SessionPolicy(bool enabled,int rootChannel,Clock::time_point now=Clock::now())
        :session(enabled),startupDeadline(now+std::chrono::seconds(30)) {
        if(!session) return;
        if(getuid() || geteuid() || wddm_primary_channel(rootChannel))
            throw std::runtime_error("Broker session requires its root supervisor channel");
        ucred owner{};socklen_t bytes=sizeof owner;
        if(getsockopt(rootChannel,SOL_SOCKET,SO_PEERCRED,&owner,&bytes) || bytes!=sizeof owner ||
           owner.uid || owner.gid || owner.pid<=1 || owner.pid==getpid() || owner.pid!=getppid())
            throw std::runtime_error("Broker session channel is not from its actual root parent");
        int pinned=-1;bytes=sizeof pinned;
        if(getsockopt(rootChannel,SOL_SOCKET,SO_PEERPIDFD,&pinned,&bytes))
            throw std::system_error(errno,std::generic_category(),"Broker session supervisor pidfd");
        supervisor=Fd(pinned);
        if(bytes!=sizeof pinned || pinned<0 || fcntl(pinned,F_SETFD,FD_CLOEXEC) ||
           wddm_primary_pidfd_pid(pinned,static_cast<std::uint64_t>(owner.pid)) || owner.pid!=getppid())
            throw std::runtime_error("Broker session supervisor identity is not live and pinned");
    }
    bool enabled() const noexcept { return session; }
    bool mayAccept(std::uint64_t connections) const noexcept { return session || connections<64; }
    const char* failure(bool primaryAccepted,bool primaryFinished,unsigned active,Clock::time_point now=Clock::now()) {
        if(!session) return nullptr;
        if(!wddm_primary_live(supervisor.get())) return "root-supervisor-exited";
        if(!primaryAccepted && now>=startupDeadline) return "primary-connection-deadline";
        if(primaryFinished && active && !drainDeadline) drainDeadline=now+std::chrono::seconds(10);
        if(drainDeadline && now>=*drainDeadline) return "secondary-drain-deadline";
        return nullptr;
    }
    [[noreturn]] static void contain(const char* reason,unsigned active) noexcept {
        // Reopen only our diagnostic endpoint with independent nonblocking
        // status flags. A full journal/tty must not block fault containment or
        // change the parent's inherited file-description flags.
        const int diagnostic=open("/proc/self/fd/2",O_WRONLY|O_NONBLOCK|O_CLOEXEC|O_NOCTTY);
        if(diagnostic>=0) {
            char record[512];const int bytes=snprintf(record,sizeof record,
                "\nBROKER_SESSION_CONTAINMENT reason=%.240s active=%u controllerClosing=true liveGpuRecoveryVerified=false\n",reason,active);
            if(bytes>0 && static_cast<std::size_t>(bytes)<sizeof record) {
                const auto written=write(diagnostic,record,static_cast<std::size_t>(bytes));
                (void)written; // Best effort only: never retry/block containment for diagnostics.
            }
            close(diagnostic);
        }
        // Joining idle/live peers here could strand the controller forever.
        // Close it by process exit. The Windows owner must reap this VM before
        // releasing retained native GPU mappings, as on other transport faults.
        _exit(125);
    }
};
struct PeerGroupOutcome {
    std::uint64_t connections=0,failures=0;
    unsigned peakWorkers=0;
    bool primaryAccepted=false,primaryFinished=false,connectionCountExact=true;
};
// Reused by the real broker and real Unix/pidfd CPU controls. GPU work remains
// the caller's callback; peer credentials, primary role and slots stay here.
template<class Work> PeerGroupOutcome runPeerGroup(Fd& listener,unsigned uid,unsigned gid,
        std::uint64_t primary,int primaryProcess,unsigned actorLimit,SessionPolicy& policy,Work work) {
    if(!driver_bridge::actors::validActorLimit(actorLimit)) throw std::runtime_error("Invalid negotiated actor limit");
    PeerGroupOutcome outcome;
    std::atomic<unsigned> active{0};std::atomic<std::uint64_t> failures{0};std::atomic<bool> primaryFinished{false};
    struct Workers {
        struct Task { std::thread thread;std::atomic<bool> done{false}; };
        std::vector<std::unique_ptr<Task>> tasks;
        explicit Workers(std::size_t limit) { tasks.reserve(limit); }
        ~Workers() { for(auto& task:tasks) if(task->thread.joinable()) task->thread.join(); }
    } workers(actorLimit);
    auto check=[&] { if(const auto reason=policy.failure(outcome.primaryAccepted,primaryFinished.load(),active.load())) SessionPolicy::contain(reason,active.load()); };
    try {
        while(!primaryFinished || active) {
            check();
            for(auto i=workers.tasks.begin();i!=workers.tasks.end();) {
                if((*i)->done) { (*i)->thread.join();i=workers.tasks.erase(i); } else ++i;
            }
            if(primaryFinished || workers.tasks.size()>=actorLimit) { poll(nullptr,0,10);continue; }
            pollfd incoming{listener.get(),POLLIN,0};const auto readyCount=poll(&incoming,1,100);
            if(readyCount<0 && errno==EINTR) continue;
            if(readyCount<0 || (incoming.revents&(POLLERR|POLLNVAL))) throw std::runtime_error("Coordinator listener poll failed");
            if(!readyCount) continue;
            Fd channel(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK));
            if(channel.get()<0) { if(errno==EINTR || errno==EAGAIN) continue;throw std::runtime_error("Coordinator accept failed"); }
            if(!policy.mayAccept(outcome.connections)) { receipt("BROKER_COORDINATOR_PEER denied=true lifetimeConnectionLimit=64\n");continue; }
            std::unique_ptr<Peer> peer;
            try { peer=std::make_unique<Peer>(std::move(channel),uid,gid); }
            catch(const std::exception& error) { receipt("BROKER_COORDINATOR_PEER denied=true reason=%s\n",error.what());continue; }
            const auto isPrimary=static_cast<std::uint64_t>(peer->pid())==primary &&
                wddm_primary_live(primaryProcess) && !peer->processExited();
            if((!outcome.primaryAccepted && !isPrimary) || (isPrimary && outcome.primaryAccepted)) {
                receipt("BROKER_COORDINATOR_PEER denied=true primaryPidMismatch=true\n");continue;
            }
            outcome.primaryAccepted|=isPrimary;
            if(outcome.connections!=UINT64_MAX) ++outcome.connections;else outcome.connectionCountExact=false;
            char path[64],exe[4096];snprintf(path,sizeof path,"/proc/%d/exe",peer->pid());
            const auto bytes=readlink(path,exe,sizeof exe);std::string hex;
            if(bytes>0) { constexpr char digits[]="0123456789abcdef";for(ssize_t i=0;i<bytes;++i) { const auto c=static_cast<unsigned char>(exe[i]);hex+=digits[c>>4];hex+=digits[c&15]; } }
            receipt("BROKER_COORDINATOR_PEER pid=%d primary=%s exePathHex=%s pidfdAuthenticated=true\n",peer->pid(),isPrimary ? "true" : "false",hex.c_str());
            ++active;
            auto task=std::make_unique<typename Workers::Task>();auto* completed=task.get();
            task->thread=std::thread([&,peer=std::move(peer),isPrimary,completed]() mutable {
                int result=1;
                try { result=work(*peer,isPrimary); }
                catch(const std::exception& error) { std::fprintf(stderr,"BROKER_COORDINATOR_ACTOR failed=true pid=%d reason=%s\n",peer->pid(),error.what()); }
                if(result) ++failures;
                if(isPrimary) primaryFinished=true;
                completed->done=true;--active;
            });
            workers.tasks.push_back(std::move(task));
            outcome.peakWorkers=(std::max)(outcome.peakWorkers,static_cast<unsigned>(workers.tasks.size()));
        }
        check();
    } catch(const std::exception& error) {
        if(policy.enabled()) SessionPolicy::contain(error.what(),active.load());
        throw;
    } catch(...) {
        if(policy.enabled()) SessionPolicy::contain("unknown-controller-failure",active.load());
        throw;
    }
    listener=Fd();
    for(auto& task:workers.tasks) task->thread.join();
    outcome.failures=failures.load();outcome.primaryFinished=primaryFinished.load();
    return outcome;
}
}
