// SPDX-License-Identifier: MIT
// Real authenticated Unix IPC; aperture and native agreement replies are mocks.
#include "guest_kernel_aperture.h"
#include <atomic>
#include <grp.h>
#include <iostream>
#include <set>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
using namespace guest_broker;
using namespace driver_bridge;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("aperture info control line "+std::to_string(__LINE__)); } while(0)
static coordinator::Info agreement(bool memory=false,bool objects=false) {
    coordinator::Info value{};
    value.capacity=GuestOpenGlMultiappCapacity;value.objects=GuestCoordinatedMultiappObjects;
    value.gl=GuestOpenGlMultiappProfile;value.sync=GuestSyncSharingProfile;value.resource=GuestResourceSharingProfile;
    if(memory) {
        value.capacity=GuestApplicationMemoryCapacity;value.maxActors=8;value.flags=7;
        value.resource=GuestApplicationResourceSharingProfile;
    }
    if(objects) {
        CHECK(memory);value.objects=GuestCoordinatedApplicationObjects;value.flags=coordinator::ApplicationObjectsFlags;
    }
    value.caps={driver_bridge::Version,0,0x10de,0};CHECK(coordinator::validInfo(value));return value;
}
static void scenario(unsigned broken,bool memory=false,bool objects=false) {
    char directory[]="/tmp/aperture-info-control-XXXXXX";CHECK(mkdtemp(directory) && !chmod(directory,0755));
    const std::string path=std::string(directory)+"/broker.sock";
    sockaddr_un address{};address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));const int enabled=1;
    CHECK(listener.get()>=0 && !setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled));
    CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof address));
    CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),1));
    const auto child=fork();CHECK(child>=0);
    if(!child) {
        try {
            close(listener.get());CHECK(!setgroups(0,nullptr) && !setgid(65534) && !setuid(65534));
            CHECK(!setenv("WDDM_BRIDGE_BROKER_SOCKET",path.c_str(),1) && !setenv("WDDM_BRIDGE_KERNEL_APERTURE","1",1));
            CHECK(!setenv("WDDM_BRIDGE_ACTOR_PROFILE","14",1));
            guest_kernel::Aperture aperture;
            if(broken) {
                bool rejected=false;try { (void)aperture.info(); } catch(const std::exception&) { rejected=true; }
                CHECK(rejected);
            } else {
                const auto first=aperture.info(),second=aperture.info();
                CHECK(second.fence_mmio_reads==first.fence_mmio_reads+1 && second.active_cpu_bytes==first.active_cpu_bytes+4096);
                std::mutex mutex;std::set<std::uint64_t> observations;std::atomic<bool> failed{false};
                std::thread threads[4];
                for(auto& thread:threads) thread=std::thread([&] {
                    try { const auto value=aperture.info();std::lock_guard<std::mutex> lock(mutex);CHECK(observations.insert(value.fence_mmio_reads).second); }
                    catch(...) { failed=true; }
                });
                for(auto& thread:threads) thread.join();
                CHECK(!failed && observations.size()==4);
                client().disconnect();bool rejected=false;
                try { (void)aperture.info(); } catch(const std::system_error& error) { rejected=error.code().value()==ESHUTDOWN; }
                CHECK(rejected);
            }
            _exit(0);
        } catch(const std::exception& error) { dprintf(2,"APERTURE_INFO_CLIENT_FAILURE %s\n",error.what());_exit(1); }
    }
    Fd accepted(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK));CHECK(accepted.get()>=0);
    Peer peer(std::move(accepted),65534,65534);unsigned layouts=0,capacities=0,profiles=0;
    for(;;) {
        auto result=peer.receive();
        if(result.state==ReceiveState::Disconnected) break;
        if(result.state==ReceiveState::Idle) { peer.waitReadable(5000);continue; }
        const auto& message=result.message;CHECK(message.fds.empty() && message.payload.empty());
        std::vector<std::uint8_t> reply;
        if(message.frame.operation==Operation::Hello) {
            ++layouts;wddm_aperture_info value{};value.version=WDDM_APERTURE_VERSION;
            value.fence_slots=512;value.allocation_slots=1024;value.active_leases=800;
            value.fence_bytes=512ull*4096;value.allocation_bytes=1024ull*1048576;value.cpu_slot_bytes=1048576;
            value.active_cpu_bytes=208ull*1048576+layouts*4096;value.fence_mmio_reads=layouts;
            if(broken==1) value.active_cpu_bytes=WDDM_APERTURE_MULTIAPP_CPU_LIMIT+4096;
            if(broken==2) value.fence_bytes+=4096;
            reply=encode(value);
        } else if(message.frame.operation==Operation::ApertureCaps) {
            ++capacities;wddm_aperture_caps value{1,7,WDDM_APERTURE_MULTIAPP_CPU_LIMIT,{0,0}};
            if(broken==3) value.reserved[0]=1;
            reply=encode(value);
        } else if(message.frame.operation==Operation::CoordinatedProfile) {
            ++profiles;auto value=agreement(memory,objects);
            if(broken==4) value.maxActors=3;
            if(broken==5) value.flags^=coordinator::ApplicationMemoryFlag;
            if(broken==6) ++value.capacity.reportedGpuByteLimit;
            if(broken==7) value.flags^=coordinator::ApplicationObjectsFlag;
            if(broken==8) value.objects.version=9;
            if(broken==9) ++value.capacity.mapping.aperture.cpuByteLimit;
            reply=encode(value);
        } else throw std::runtime_error("Unexpected aperture metadata operation");
        peer.send(message.frame.operation,message.frame.sequence,reply);
    }
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(peer.processExited());CHECK(!unlink(path.c_str()) && !rmdir(directory));
    std::cout<<"APERTURE_INFO_REQUESTS broken="<<broken<<" memory="<<memory<<" objects="<<objects<<" layouts="<<layouts<<" capacities="<<capacities<<" profiles="<<profiles<<std::endl;
    if(!broken) CHECK(layouts==7 && capacities==6 && profiles==6);
}
int main() {
    if(geteuid()!=0) { std::cerr<<"Requires root only to run real UID65534 IPC controls\n";return 77; }
    try {
        for(const bool memory:{false,true}) for(unsigned broken=0;broken<7;++broken) scenario(broken,memory);
        for(unsigned broken=0;broken<10;++broken) scenario(broken,true,true);
        std::cout<<"APERTURE_INFO_COALESCING perCallOnly=true freshCounters=true concurrentCalls=true closedChannelRejected=true malformedLayoutsAndAgreementsRejected=true uid=65534 gpu=false kernel=mock\n";
    } catch(const std::exception& error) { std::cerr<<"APERTURE_INFO_CONTROL_FAILURE "<<error.what()<<"\n";return 1; }
}
