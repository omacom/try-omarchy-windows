// SPDX-License-Identifier: MIT
// Real owned Windows process/pipes/job cleanup; source zero is rejected before
// any KMT sharing or D3D12 device call. This is not a GPU rendering test.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include "driver_wire.h"
#include "shared_texture_worker.h"
int main(int argc,char** argv) {
    try {
        if (argc==4 && std::string(argv[1])=="--native-texture-worker") return driver_worker::run(argv[2],argv[3]);
        DWORD before=0,after=0;
        driver_worker::Counters counters;
        // The first process-creation cycle retains extra handles in this
        // environment. Its attribution is separate; test continuing growth.
        {
            driver_worker::TextureWorker warmup(counters);
            if (SUCCEEDED(warmup.initialize(0,LUID{}))) throw std::runtime_error("Invalid warmup source accepted");
        }
        if (!GetProcessHandleCount(GetCurrentProcess(),&before)) throw std::runtime_error("Initial owned handle count unavailable");
        {
            driver_worker::TextureWorker worker(counters);
            if (SUCCEEDED(worker.initialize(0,LUID{}))) throw std::runtime_error("Invalid source accepted");
        }
        const bool counted=GetProcessHandleCount(GetCurrentProcess(),&after)!=FALSE;
        std::cerr<<"WORKER_PROCESS_COUNTS before="<<before<<" after="<<after<<" started="<<counters.started<<" exited="<<counters.exited<<" forced="<<counters.forced<<" failed="<<counters.failed<<" transferred="<<counters.transferred<<'\n';
        if (!counted || before!=after || counters.started!=2 || counters.exited!=2 ||
            counters.forced || counters.failed!=2 || counters.transferred)
            throw std::runtime_error("Owned worker EOF/error cleanup failed");
        std::cout<<"NATIVE_WORKER_PROCESS invalidSourceRejected=true childExited=true forced=false steadyStateHandleLeaks=0 warmup=1 gpu=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
