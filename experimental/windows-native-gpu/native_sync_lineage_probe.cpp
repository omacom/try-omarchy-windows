// SPDX-License-Identifier: MIT
// Actual D3D12 GPU ordering through Linux sync_file lineage, not EGL integration.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
struct HGLRC__; using HGLRC=HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include "guest-kernel/wddm_aperture.h"
#include <linux/sync_file.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cerrno>
using Microsoft::WRL::ComPtr;
namespace {
struct Fd {
    int value=-1;
    explicit Fd(int n=-1):value(n) {}
    Fd(const Fd&)=delete;
    ~Fd() { reset(); }
    void reset() { if(value>=0) close(value); value=-1; }
};
int status(int fd) {
    sync_fence_info detail{}; sync_file_info info{}; info.num_fences=1;
    info.sync_fence_info=reinterpret_cast<std::uintptr_t>(&detail);
    if(syscall(SYS_ioctl,fd,SYNC_IOC_FILE_INFO,&info) || info.num_fences!=1 || detail.status!=info.status) return -10000;
    return info.status;
}
bool finish(int fd,int expected) {
    pollfd event{fd,POLLIN,0};
    return syscall(SYS_poll,&event,1,6500)==1 && (event.revents&POLLIN) && status(fd)==expected;
}
bool completed(ID3D12Fence* fence,std::uint64_t target) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do {
        const auto value=fence->GetCompletedValue();
        if(value==UINT64_MAX) return false;
        if(value>=target) return true;
        usleep(1000);
    } while(std::chrono::steady_clock::now()<deadline);
    return false;
}
HANDLE handle(int fd) { return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd)); }
}
extern "C" int wddm_native_sync_file_test(EGLDisplay display,EGLContext context) {
    const auto failure=[](const char* name){std::printf("SYNC_LINEAGE_FAILURE stage=%s errno=%d\n",name,errno);return 1;};
    std::puts("SYNC_LINEAGE_BEGIN gpu=true kernel=true eglNativeFence=false desktop=false");
    auto exportFile=reinterpret_cast<int(*)(int,std::uint64_t)>(dlsym(RTLD_DEFAULT,"wddm_bridge_export_native_sync_file"));
    auto resolve=reinterpret_cast<int(*)(int,std::uint64_t*)>(dlsym(RTLD_DEFAULT,"wddm_bridge_resolve_sync_file"));
    auto stats=reinterpret_cast<int(*)(wddm_sync_file_stats*)>(dlsym(RTLD_DEFAULT,"wddm_bridge_sync_file_info"));
    if(!exportFile || !resolve || !stats) return failure("BridgeHooks");
    d3d12_interop_device_info native{}; mesa_glinterop_device_info interop{};
    interop.version=2; interop.driver_data_size=sizeof native; interop.driver_data=&native;
    if(MesaGLInteropEGLQueryDeviceInfo(display,context,&interop)!=MESA_GLINTEROP_SUCCESS ||
        interop.driver_data_size!=sizeof native || !native.device || !native.queue) return failure("MesaDevice");
    std::uint64_t target=0;
    Fd foreign(open("/dev/null",O_RDONLY|O_CLOEXEC));
    errno=0; if(resolve(foreign.value,&target)!=-1 || errno!=EINVAL) return failure("ForeignSyncFile");
    errno=0; if(resolve(foreign.value,nullptr)!=-1 || errno!=EINVAL) return failure("MissingOutput");
    errno=0; if(exportFile(foreign.value,1)!=-1 || errno!=EINVAL) return failure("ForeignNativeCapability");
    ComPtr<ID3D12Fence> source,imported,acknowledgement;
    if(FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&source)))) return failure("Source");
    HANDLE shared=nullptr;
    if(FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&shared))) return failure("ExportNative");
    Fd cap(static_cast<int>(reinterpret_cast<std::intptr_t>(shared)));
    errno=0; if(exportFile(cap.value,UINT64_MAX)!=-1 || errno!=EINVAL) return failure("InvalidTarget");
    // Target zero represents a valid no-work dependency. It must be a real
    // kernel sync_file, not an invented signaled marker/foreign descriptor.
    Fd zero(exportFile(cap.value,0));
    if(zero.value<0 || !(fcntl(zero.value,F_GETFD)&FD_CLOEXEC) || !finish(zero.value,1)) return failure("ZeroTarget");
    Fd zeroNative(resolve(zero.value,&target));
    if(zeroNative.value<0 || target!=0 || !(fcntl(zeroNative.value,F_GETFD)&FD_CLOEXEC)) return failure("ResolveZero");
    zeroNative.reset(); zero.reset();
    constexpr std::uint64_t pendingTarget=(UINT64_C(1)<<32)+71;
    Fd pending(exportFile(cap.value,pendingTarget));
    if(pending.value<0 || status(pending.value)!=0) return failure("Pending");
    Fd resolved(resolve(pending.value,&target));
    if(resolved.value<0 || target!=pendingTarget || !(fcntl(resolved.value,F_GETFD)&FD_CLOEXEC)) return failure("ResolvePending");
    if(FAILED(native.device->OpenSharedHandle(handle(resolved.value),IID_PPV_ARGS(&imported)))) return failure("ImportResolved");
    resolved.reset(); cap.reset();
    ComPtr<ID3D12CommandQueue> release;
    D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_COPY;
    if(FAILED(native.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&release))) ||
        FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&acknowledgement)))) return failure("ReleaseQueue");
    if(FAILED(native.queue->Wait(imported.Get(),target))) return failure("GpuWait");
    const auto signal=native.queue->Signal(acknowledgement.Get(),1);
    usleep(50000);
    const auto before=imported->GetCompletedValue(),gated=acknowledgement->GetCompletedValue();
    // Release even on observation failure to avoid intentionally stranding
    // the original Mesa GPU queue during diagnostic cleanup.
    const auto released=release->Signal(source.Get(),target);
    const bool retired=SUCCEEDED(released) && completed(imported.Get(),target) && completed(acknowledgement.Get(),1);
    if(FAILED(signal) || before!=0 || gated!=0 || !retired || !finish(pending.value,1)) return failure("GpuOrdering");
    std::printf("SYNC_LINEAGE_GPU target=%llu before=0 acknowledgementBefore=0 acknowledgementAfter=1 nativeWait=true independentGpuRelease=true cpuFenceWrites=false\n",
        static_cast<unsigned long long>(target));
    imported.Reset(); source.Reset();
    // The completed Linux FD retains the genuine native object after original
    // source, exported FD and first imported D3D12 alias have all been released.
    Fd retained(resolve(pending.value,&target));
    if(retained.value<0 || target!=pendingTarget ||
        FAILED(native.device->OpenSharedHandle(handle(retained.value),IID_PPV_ARGS(&imported))) ||
        imported->GetCompletedValue()!=pendingTarget) return failure("CompletedSourceLifetime");
    retained.reset(); pending.reset(); imported.Reset(); acknowledgement.Reset(); release.Reset();
    std::puts("SYNC_LINEAGE_LIFETIME originalSourceReleased=true originalNativeFdClosed=true firstImportedAliasReleased=true completedSyncFileRetainsNative=true");
    if(FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&source))) ||
        FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&shared))) return failure("CancelSource");
    cap.value=static_cast<int>(reinterpret_cast<std::intptr_t>(shared));
    Fd canceled(exportFile(cap.value,1)); cap.reset();
    if(canceled.value<0 || status(canceled.value)!=0) return failure("CancelPending");
    source.Reset();
    if(!finish(canceled.value,-ENODEV)) return failure("CancelStatus");
    errno=0; if(resolve(canceled.value,&target)!=-1 || errno!=ENODEV) return failure("CancelImport");
    canceled.reset();
    std::puts("SYNC_LINEAGE_REJECT canceledStatus=-19 canceledImportErrno=19 foreignRejected=true nullOutputRejected=true");
    wddm_sync_file_stats info{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
        if(stats(&info)) return failure("Stats");
        if(!info.held && !info.pending) break;
        usleep(1000);
    } while(std::chrono::steady_clock::now()<deadline);
    if(info.held || info.pending || info.created!=3 || info.signaled!=2 || info.errors!=1 || info.released!=3) return failure("Counts");
    std::puts("SYNC_LINEAGE_COMPLETE created=3 signaled=2 errors=1 released=3 held=0 pending=0 gpuQueueWaits=1 gpuReleaseSignals=1 dependentGpuSignals=1 eglNativeFence=false desktop=false");
    return 0;
}
