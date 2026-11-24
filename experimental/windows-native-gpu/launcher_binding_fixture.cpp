// SPDX-License-Identifier: MIT
// A real owned hidden child, with synthetic QMP state; no QEMU or GPU.
#define main owned_qemu_fixture_main
#include "owned_qemu_process_test.cpp"
#undef main
#include "launcher_lifetime.h"
#include <filesystem>

int main(int argc,char** argv) {
    if(argc==4 && std::string(argv[1])=="--owned-child") return owned_qemu_fixture_main(argc,argv);
    try {
        if(argc!=5 || std::string(argv[3])!="--driver-launcher-cancel-handle") throw std::runtime_error("Invalid binding fixture command");
        const std::string mode=argv[1];
        if(mode!="ready" && mode!="exit-before-ready" && mode!="cancel-before-ready" && mode!="bad-ready" && mode!="duplicate-bound")
            throw std::runtime_error("Unknown binding fixture mode");
        Handle creator,token;
        driver_launcher::captureActualCreator(creator);
        driver_launcher::takeInheritedCancellation(driver_launcher::parseHandle(argv[4]),token);
        Child child;OwnedProcess owner;
        driver_launcher::Lifetime lifetime(creator.value,token.value);
        owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),child.claim(),self());
        child.originalHandleTransferred();
        std::cout<<driver_launcher::bound(owner.binding(),lifetime).dump()<<'\n'<<std::flush;
        lifetime.requireLive();child.resume();
        lifetime.start([&](driver_launcher::End){owner.stop(0);});
        const auto deadline=GetTickCount64()+10000;
        while(!std::filesystem::is_regular_file(argv[2])) {
            if(lifetime.wait(10)) return 1;
            if(GetTickCount64()>=deadline) throw std::runtime_error("Binding fixture marker deadline");
        }
        if(mode=="exit-before-ready") {
            std::cerr<<"cannot set up guest memory: synthetic helper control, qemu=false gpu=false\n";
            owner.stop(0);return 1;
        }
        if(mode=="cancel-before-ready") throw std::runtime_error("Cancelled fixture unexpectedly received a ready marker");
        auto message=mode=="duplicate-bound" ? driver_launcher::bound(owner.binding(),lifetime) : driver_launcher::ready(owner.binding(),lifetime);
        if(mode=="bad-ready") message["qemuCreationTime100ns"]=std::to_string(owner.binding().creationTime+1);
        std::cout<<message.dump()<<'\n'<<std::flush;
        if(!lifetime.wait(10000)) throw std::runtime_error("Binding fixture cancellation deadline");
        return 1;
    } catch(const std::exception& error) { std::cerr<<"BINDING_FIXTURE_FAIL "<<error.what()<<'\n';return 1; }
}
