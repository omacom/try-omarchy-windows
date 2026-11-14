// SPDX-License-Identifier: MIT
#include "guest_bridge_log.h"
#include <cerrno>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void require(bool value) {if(!value) throw std::runtime_error("diagnostic logging control failed");}
static std::string read(std::FILE* file) {
    require(!std::fflush(file));require(!std::fseek(file,0,SEEK_SET));std::string result;
    char buffer[256];std::size_t bytes=0;
    while((bytes=std::fread(buffer,1,sizeof buffer,file))) result.append(buffer,bytes);
    require(!std::ferror(file));return result;
}
int main() {
    std::FILE* file=std::tmpfile();require(file!=nullptr);
    unsigned evaluated=0;errno=ERANGE;
    const int count=guest_bridge_log::diagnostic(file,"LINUX_BRIDGE allocationItem index=%u flags=0\n",++evaluated);
    require(evaluated==1);
    if constexpr(WDDM_BRIDGE_VERBOSE_IOCTL_TRACE) require(count>0);
    else {require(count==0);require(errno==ERANGE);}
    const char* important[]={"LINUX_BRIDGE ioctlError errno=%d\n","LINUX_BRIDGE unsupported nr=%d\n",
        "LINUX_BRIDGE allocationRejected result=%d\n","LINUX_BRIDGE nativeVendorResident status=%d\n",
        "LINUX_BRIDGE workerReaped=false errno=%d\n","LINUX_BRIDGE summary count=%d\n",
        "LINUX_BRIDGE nvidiaUmdPresentDuringPrivateQuery=%d\n","LINUX_BRIDGE processExitViewsRetained=%d\n",
        "LINUX_BRIDGE futureUnknownDiagnostic=%d\n","LINUX_BRIDGE allocationItemFuture=%d\n","application error %d\n"};
    for(const char* format:important) require(guest_bridge_log::diagnostic(file,format,-22)>0);
    guest_bridge_log::report(file);
    const auto text=read(file);
    require((text.find("allocationItem index=1")!=std::string::npos)==bool(WDDM_BRIDGE_VERBOSE_IOCTL_TRACE));
    require(text.find("ioctlError errno=-22")!=std::string::npos && text.find("nativeVendorResident status=-22")!=std::string::npos);
    require(text.find("application error -22")!=std::string::npos);
    require(text.find("futureUnknownDiagnostic=-22")!=std::string::npos && text.find("allocationItemFuture=-22")!=std::string::npos);
    require((text.find("suppressedRoutineRecords=1")!=std::string::npos)==!bool(WDDM_BRIDGE_VERBOSE_IOCTL_TRACE));
    require(!std::fclose(file));
    if constexpr(!WDDM_BRIDGE_VERBOSE_IOCTL_TRACE) {
        const auto before=guest_bridge_log::suppressed.load();
        std::vector<std::thread> workers;
        for(unsigned n=0;n<4;++n) workers.emplace_back([] {
            for(unsigned i=0;i<2500;++i) guest_bridge_log::diagnostic(nullptr,"LINUX_BRIDGE pagingFenceRetired=true target=%u\n",i);
        });
        for(auto& worker:workers) worker.join();
        require(guest_bridge_log::suppressed.load()==before+10000);
    }
    std::printf("LOG_CONTROL_PASS verbose=%u argumentEvaluated=true errorsAndStatusRetained=true concurrentCount=true gpu=false\n",WDDM_BRIDGE_VERBOSE_IOCTL_TRACE);
}
