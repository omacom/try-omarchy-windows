// SPDX-License-Identifier: MIT
// Actual mapping classes + authenticated Unix IPC and memfd mappings.
// Aperture metadata/native fences are mocks; no GPU or kernel module is used.
#include "guest_paging_fence.h"
#include "guest_allocation_memory.h"
#include <grp.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
using namespace guest_broker;
#define CHECK(x) do { if(!(x)) throw std::runtime_error("direct mapping control line "+std::to_string(__LINE__)); } while(0)
enum class Case { Fence, LostFence, FenceMapMmio, FenceRetirementMmio,
                  Allocation, AllocationReadMmio, AllocationWriteMmio };
template<class Action> static void rejects(Action action) {
    bool rejected=false;try { action(); } catch(const std::exception&) { rejected=true; }
    CHECK(rejected);
}
static void childControl(Case test) {
    const bool allocation=test>=Case::Allocation;
    if(!allocation) {
        guest_paging::Fences fences;
        if(test==Case::FenceMapMmio) {
            rejects([&] { fences.map(1,0); });CHECK(!fences.active());
        } else {
            const auto* mapped=static_cast<const std::uint64_t*>(fences.map(1,0));
            CHECK(mapped && fences.active()==1 && fences.contains(1));
            CHECK(fences.directLoads()==guest_direct_read::ProbeLoads);
            rejects([&] { fences.verifyRetired(9,7,"unknown-queue"); });
            if(test==Case::Fence) {
                fences.verifyRetired(1,7,"ready");
                CHECK(fences.directLoads()==2*guest_direct_read::ProbeLoads);
                rejects([&] { fences.verifyRetired(1,8,"future-target"); });
                rejects([&] { fences.map(1,0); });
            } else rejects([&] { fences.verifyRetired(1,7,"lost-or-emulated"); });
            fences.unmap(1);CHECK(!fences.active());
        }
    } else {
        guest_allocation::Memory memory;
        const driver_bridge::VendorCpuReply reply{4096,0,1};
        if(test==Case::Allocation) {
            const auto* mapped=static_cast<const std::uint64_t*>(memory.map(1,0,reply));
            CHECK(mapped && mapped[0]==11 && mapped[511]==29 && memory.active()==1);
            CHECK(memory.directLoads()==guest_direct_read::ProbeLoads);
            rejects([&] { memory.map(1,0,reply); });
            CHECK(memory.release(1) && !memory.active());
        } else {
            rejects([&] { memory.map(1,0,reply); });CHECK(!memory.active());
        }
    }
    client().disconnect();
}
static void scenario(Case test) {
    char directory[]="/tmp/direct-read-mapping-XXXXXX";
    CHECK(mkdtemp(directory) && !chmod(directory,0755));
    const std::string path=std::string(directory)+"/broker.sock";
    sockaddr_un address{};address.sun_family=AF_UNIX;
    CHECK(path.size()<sizeof address.sun_path);std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    Fd listener(socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0));const int enabled=1;
    CHECK(listener.get()>=0 && !setsockopt(listener.get(),SOL_SOCKET,SO_PASSCRED,&enabled,sizeof enabled));
    CHECK(!bind(listener.get(),reinterpret_cast<sockaddr*>(&address),sizeof address));
    CHECK(!chown(path.c_str(),65534,65534) && !chmod(path.c_str(),0600) && !listen(listener.get(),1));
    Fd backing(memfd_create("direct-read-controlled-memory",MFD_CLOEXEC));
    CHECK(backing.get()>=0 && !ftruncate(backing.get(),4096));
    const std::uint64_t first=test>=Case::Allocation ? 11 : test==Case::LostFence ? UINT64_MAX : 7;
    const std::uint64_t last=29;
    CHECK(pwrite(backing.get(),&first,sizeof first,0)==sizeof first);
    CHECK(pwrite(backing.get(),&last,sizeof last,4088)==sizeof last);
    const auto child=fork();CHECK(child>=0);
    if(!child) {
        try {
            close(listener.get());close(backing.get());
            CHECK(!setgroups(0,nullptr) && !setgid(65534) && !setuid(65534));
            CHECK(!setenv("WDDM_BRIDGE_BROKER_SOCKET",path.c_str(),1));
            CHECK(!setenv("WDDM_BRIDGE_KERNEL_APERTURE","1",1));
            CHECK(!unsetenv("WDDM_BRIDGE_ACTOR_PROFILE"));
            CHECK(!unsetenv("WDDM_BRIDGE_CPU_STORE_TEST"));
            childControl(test);_exit(0);
        } catch(const std::exception& error) {
            dprintf(2,"DIRECT_READ_MAPPING_CHILD_FAILURE case=%u %s\n",static_cast<unsigned>(test),error.what());_exit(1);
        }
    }
    Fd accepted(accept4(listener.get(),nullptr,nullptr,SOCK_CLOEXEC|SOCK_NONBLOCK));CHECK(accepted.get()>=0);
    Peer peer(std::move(accepted),65534,65534);
    unsigned acquired=0,released=0,afterAcquireInfo=0;
    for(;;) {
        auto result=peer.receive();
        if(result.state==ReceiveState::Disconnected) break;
        if(result.state==ReceiveState::Idle) { peer.waitReadable(5000);continue; }
        const auto& message=result.message;CHECK(message.fds.empty());
        std::vector<std::uint8_t> reply;std::vector<int> fds;
        if(message.frame.operation==Operation::Hello) {
            CHECK(message.payload.empty());
            if(acquired) ++afterAcquireInfo;
            wddm_aperture_info value{};value.version=WDDM_APERTURE_VERSION;
            value.fence_slots=64;value.allocation_slots=16;value.fence_bytes=64*4096;
            value.allocation_bytes=16ull*1048576;value.cpu_slot_bytes=1048576;
            if(acquired && test==Case::FenceMapMmio) value.fence_mmio_reads=1;
            if(acquired && test==Case::FenceRetirementMmio && afterAcquireInfo>=3) value.fence_mmio_reads=1;
            if(acquired && test==Case::AllocationReadMmio) value.allocation_mmio_reads=1;
            if(acquired && test==Case::AllocationWriteMmio) value.allocation_mmio_writes=1;
            reply=encode(value);
        } else if(message.frame.operation==Operation::ApertureCaps) {
            CHECK(message.payload.empty());
            reply=encode(wddm_aperture_caps{1,0,WDDM_APERTURE_CPU_LIMIT,{0,0}});
        } else if(message.frame.operation==Operation::AcquireView) {
            const auto view=decode<ViewRequest>(message.payload);
            CHECK(!acquired && !view.reserved && !view.offset && view.bytes==4096);
            CHECK(view.kind==(test>=Case::Allocation ? WDDM_APERTURE_ALLOCATION : WDDM_APERTURE_FENCE));
            CHECK(view.generation==(test>=Case::Allocation ? 1u : 0u));
            ++acquired;reply=encode(ViewReply{1});fds.push_back(backing.get());
        } else if(message.frame.operation==Operation::ReleaseView) {
            CHECK(decode<ViewReply>(message.payload).cookie==1 && acquired==1 && !released);++released;
        } else throw std::runtime_error("Unexpected controlled broker operation");
        peer.send(message.frame.operation,message.frame.sequence,reply,fds);
    }
    int status=0;CHECK(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    CHECK(acquired==1 && released==1 && peer.processExited());
    CHECK(!unlink(path.c_str()) && !rmdir(directory));
    std::cout<<"DIRECT_READ_MAPPING case="<<static_cast<unsigned>(test)<<" loads="<<guest_direct_read::ProbeLoads
             <<" leaseReleased=true uid=65534 gpu=false kernel=mock"<<std::endl;
}
int main() {
    if(geteuid()!=0) { std::cerr<<"Requires root only for real UID65534 IPC controls\n";return 77; }
    try {
        for(const auto test:{Case::Fence,Case::LostFence,Case::FenceMapMmio,Case::FenceRetirementMmio,
                            Case::Allocation,Case::AllocationReadMmio,Case::AllocationWriteMmio}) scenario(test);
    } catch(const std::exception& error) { std::cerr<<"DIRECT_READ_MAPPING_FAILURE "<<error.what()<<std::endl;return 1; }
}
