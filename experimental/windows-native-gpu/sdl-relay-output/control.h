// SPDX-License-Identifier: MIT
#pragma once
#include <functional>
#include <memory>
#include <string>
#include "../qmp_protocol.h"

namespace driver_sdl {
struct Control {
    std::string session,adapterLuid;
    // A dedicated, owned graphics QMP connection. Never share the input socket.
    std::function<driver_qmp::Json(const std::string&)> command;
    // A fatal GPU exit must first reap the owned VM while driver pages remain
    // alive. This callback never frees guest objects or touches input code.
    std::function<void()> beforeFatalExit;
    bool bootConsole=false;
};
inline bool canonicalHex(const std::string& value,std::size_t size) {
    return value.size()==size && value.find_first_not_of("0123456789abcdef")==std::string::npos;
}
inline std::string luidText(const void* luid) {
    const auto bytes=static_cast<const unsigned char*>(luid);
    const char hex[]="0123456789abcdef";std::string result;
    for(unsigned i=0;i<8;++i) { result+=hex[bytes[i]>>4];result+=hex[bytes[i]&15]; }
    return result;
}
inline void validateState(const Control& control,const driver_qmp::Json& row,
                          std::uint64_t frame,unsigned audits,bool attached) {
    const bool pendingBoot=control.bootConsole && frame==0;
    if(control.bootConsole) {
        if(!row.at("consoleIndex").is_number_integer() || row.at("consoleIndex")!=0 ||
           row.at("bootConsoleBorrowed")!=true || row.at("nativeConsoleActive")!=(attached && !pendingBoot) ||
           (pendingBoot && audits))
            throw std::runtime_error("SDL boot-console ownership or transition phase differs");
    } else if(row.contains("bootConsoleBorrowed") && row.at("bootConsoleBorrowed")!=false) {
        throw std::runtime_error("Unexpected borrowed SDL boot console");
    }
    for(const auto* field:{"generation","liveImportedObjects","sourceWidth","sourceHeight",
                           "lastQueuedFrame","convertedFrames","auditFrames"})
        if(!row.at(field).is_number_integer()) throw std::runtime_error("SDL GPU state counter is not an integer");
    if(!row.is_object() || row.at("attached")!=attached || row.at("poisoned")!=false ||
       row.at("everAttached")!=true || row.at("rootAdapterVerified")!=!pendingBoot || row.at("generation")!=1 ||
       row.at("importContextLive")!=attached || row.at("localScanoutLive")!=attached ||
       row.at("liveImportedObjects")!=(attached?4:0) || row.at("channelProgramLive")!=attached ||
       row.at("adapterLuid")!=control.adapterLuid || row.at("vendor")!="NVIDIA Corporation" ||
       row.at("renderer").get<std::string>().find("NVIDIA")==std::string::npos ||
       row.at("sourceWidth")!=3840 || row.at("sourceHeight")!=2160 || row.at("sourceFormat")!="bgra8" ||
       row.at("lastQueuedFrame")!=frame || row.at("convertedFrames")!=frame || row.at("auditFrames")!=audits ||
       row.at("independentFences")!=true || row.at("cpuTextureUpload")!=false ||
       row.at("physicalInputVerified")!=false || row.at("physicalScanoutVerified")!=false ||
       row.at("guestGpuScanoutVerified")!=false || row.at("nearNativePerformanceVerified")!=false)
        throw std::runtime_error("SDL relay GPU state, lifetime or bounded scope differs");
}
}
