// SPDX-License-Identifier: MIT
#include "guest_broker_server.h"
#include "guest_coordinator.h"
#include "guest_primary_identity.h"
#include "guest_broker_session.h"
#include <sys/stat.h>
#include <sys/un.h>
#include <limits>
#include <thread>

static int coordinated(guest_broker::Fd& listener,const char* path,unsigned uid,unsigned gid,guest_broker::Fd notification,guest_broker::Fd primaryPipe,bool sessionMode,bool witnesses) {
    using namespace guest_broker;
    CoordinatorPort controller(path,witnesses);Kernel layout;
    const auto info=layout.info();const auto caps=layout.capacity();
    const auto references=std::getenv("WDDM_BRIDGE_COORDINATED_REFERENCES");
    if(references && std::strcmp(references,"1")) throw std::runtime_error("Invalid coordinated reference opt-in");
    controller.initialize(info,caps,layout.coordinatedCapacity(),info.fence_slots==512 ? layout.desktopFenceCapacity() : wddm_aperture_desktop_fences{},
        info.allocation_slots==1024 ? layout.desktopSlotCapacity() : wddm_aperture_desktop_slots{},references!=nullptr);layout.close();
    if(const auto retirement=std::getenv("WDDM_BRIDGE_COORDINATED_RETIREMENT")) {
        if(std::strcmp(retirement,"1") || !references) throw std::runtime_error("Invalid coordinated live retirement opt-in");
        if(witnesses) {
            if(mkdir("/run/wddm-retired",0755) && errno!=EEXIST) throw std::runtime_error("Retirement witness directory");
            struct stat status{};
            if(lstat("/run/wddm-retired",&status) || !S_ISDIR(status.st_mode) || status.st_uid || (status.st_mode&0022))
                throw std::runtime_error("Untrusted retirement witness directory");
            Fd directory(open("/run/wddm-retired",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
            if(directory.get()<0 || fchmod(directory.get(),0755)) throw std::runtime_error("Retirement witness directory mode");
        }
        controller.beginLiveRetirement();
    }
    receipt("BROKER_RETIREMENT_WITNESSES enabled=%s nativeRetirementRequired=true\n",witnesses ? "true" : "false");
    const char ready='R';if(write(notification.get(),&ready,1)!=1) throw std::runtime_error("Coordinator readiness failed");
    notification=Fd();
    std::uint64_t primary=0;int descriptor=-1;
    if(wddm_primary_receive(primaryPipe.get(),30000,&primary,&descriptor))
        throw std::runtime_error("Coordinator trusted primary pidfd missing or invalid");
    Fd primaryProcess(descriptor);
    if(sessionMode && !controller.liveRetirementEnabled()) throw std::runtime_error("Broker session requires live coordinated retirement");
    SessionPolicy policy(sessionMode,primaryPipe.get());
    primaryPipe=Fd();
    receipt("BROKER_COORDINATOR_PRIMARY pid=%llu rootChannel=true pinnedPidfd=true gpuProof=false\n",static_cast<unsigned long long>(primary));
    ObjectRegistry<> objects;
    const auto actorLimit=controller.info().maxActors;
    if(!driver_bridge::actors::validActorLimit(actorLimit)) throw std::runtime_error("Invalid negotiated actor limit");
    if(sessionMode) {
        alarm(0);
        receipt("BROKER_SESSION_LIFETIME rootSupervisorPidfd=true lifetimeConnectionLimit=none activeActorLimit=%u primaryConnectDeadlineSeconds=30 drainDeadlineSeconds=10 guestSessionVerified=false\n",actorLimit);
    } else alarm(info.fence_slots==512 ? 240 : 120);
    const auto outcome=runPeerGroup(listener,uid,gid,primary,primaryProcess.get(),actorLimit,policy,[&](Peer& peer,bool isPrimary) {
        ActorPort native(controller,isPrimary,peer.pid());ActorObjects store(objects,native);Kernel kernel;
        return serveWithObjects(peer,native,kernel,store,store);
    });
    const auto failures=outcome.failures;
    if(controller.liveRetirementEnabled() && !failures) {
        pollfd exited{primaryProcess.get(),POLLIN,0};
        if(!outcome.primaryAccepted || !outcome.primaryFinished || poll(&exited,1,0)!=1 ||
            !(exited.revents&POLLIN) || (exited.revents&(POLLERR|POLLNVAL)) || !objects.empty())
            throw std::runtime_error("Terminal group lacks complete process/capability retirement");
        receipt("BROKER_COORDINATOR_GROUP_STOPPED primaryPid=%llu workersJoined=true listenerClosed=true primaryPidfdExited=true registryEmpty=true\n",
            static_cast<unsigned long long>(primary));
        controller.retireStoppedGroup();
    }
    alarm(0);
    if(sessionMode) receipt("BROKER_SESSION_SUMMARY connections=%llu countsExact=%s peakWorkers=%u activeActorLimit=%u workersJoined=true gpuProof=false\n",
        static_cast<unsigned long long>(outcome.connections),outcome.connectionCountExact ? "true" : "false",outcome.peakWorkers,actorLimit);
    std::fprintf(stderr,"BROKER_COORDINATOR_EXIT connections=%llu failed=%llu primaryPidFromRoot=true gpuProof=false\n",
        static_cast<unsigned long long>(outcome.connections),static_cast<unsigned long long>(failures));
    return failures ? 1 : 0;
}

int main(int argc,char** argv) {
    using namespace guest_broker;
    try {
        const bool group=argc==8 && std::string(argv[6])=="--coordinator";
        const bool sessionMode=SessionPolicy::requested(std::getenv("WDDM_BRIDGE_BROKER_SESSION"));
        if(sessionMode && !group) throw std::runtime_error("Broker session requires the root coordinated runtime");
        if ((argc != 6 && !group) || getuid() || geteuid()) throw std::runtime_error("Broker requires root: SOCKET PORT UID GID READY_FD [--coordinator PRIMARY_IDENTITY_FD]");
        const bool witnesses=SessionPolicy::retirementWitnesses(std::getenv("WDDM_BRIDGE_RETIREMENT_WITNESSES"),sessionMode);
        const auto number=[](const char* value) {
            if (!value || !*value || std::strspn(value,"0123456789") != std::strlen(value)) throw std::runtime_error("Invalid broker numeric argument");
            const auto n=std::stoul(value); if (n >= std::numeric_limits<unsigned>::max()) throw std::runtime_error("Broker numeric argument overflow"); return static_cast<unsigned>(n);
        };
        const auto uid=number(argv[3]),gid=number(argv[4]),ready=number(argv[5]);
        if (!uid || !gid || ready < 3 || ready > static_cast<unsigned>(std::numeric_limits<int>::max())) throw std::runtime_error("Broker requires an ordinary client and readiness pipe");
        sockaddr_un address{}; address.sun_family=AF_UNIX;
        if (argv[1][0] != '/' || std::strlen(argv[1]) >= sizeof(address.sun_path) || argv[2][0] != '/')
            throw std::runtime_error("Broker paths must be bounded and absolute");
        std::memcpy(address.sun_path,argv[1],std::strlen(argv[1])+1);
        Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));
        const int credentials=1; umask(0077);
        if (listener.get() < 0 || setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&credentials,sizeof(credentials)) ||
            bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof(address)) ||
            chown(argv[1],uid,gid) || chmod(argv[1],0600) || listen(listener.get(),group ? 4 : 1))
            throw std::system_error(errno,std::generic_category(),"Broker listener setup");
        Fd notification(static_cast<int>(ready));
        if(group) {
            const auto descriptor=number(argv[7]);
            if(descriptor<3 || descriptor==ready || descriptor>INT32_MAX || wddm_primary_channel(static_cast<int>(descriptor)))
                throw std::runtime_error("Coordinator requires a distinct inherited root seqpacket channel for the primary pidfd");
            return coordinated(listener,argv[2],uid,gid,std::move(notification),Fd(static_cast<int>(descriptor)),sessionMode,witnesses);
        }
        const char marker='R';
        if (write(notification.get(),&marker,1) != 1) throw std::runtime_error("Broker readiness signal failed");
        notification=Fd();
        Fd accepted;
        for (;;) {
            const auto fd=accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK);
            if (fd >= 0) { accepted=Fd(fd); break; }
            if (errno != EINTR) throw std::system_error(errno,std::generic_category(),"Broker accept failed");
        }
        listener=Fd(); // One process per native GPU worker until coordinator integration.
        Peer peer(std::move(accepted),uid,gid);
        std::fprintf(stderr,"BROKER_ROUTE_START protocol=%u clientPid=%d clientUid=%u nativeWorkerClients=1 kernelControllersClient=false\n",Version,peer.pid(),uid);
        Port native(argv[2]); Kernel kernel;
        return serve(peer,native,kernel);
    } catch (const std::exception& error) {
        std::fprintf(stderr,"BROKER_ROUTE failed=true startup=true reason=%s\n",error.what()); return 1;
    }
}
