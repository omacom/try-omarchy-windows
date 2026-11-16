// SPDX-License-Identifier: MIT
// Actual EGL callbacks plus independent native GPU ordering; not a desktop test.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
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
#include <cstring>
#include <cstdint>
#include <cerrno>
using Microsoft::WRL::ComPtr;
namespace {
struct Fd {
    int value=-1;
    explicit Fd(int fd=-1):value(fd) {}
    Fd(const Fd&)=delete;
    ~Fd() { reset(); }
    void reset() { if(value>=0) close(value); value=-1; }
};
struct Sync {
    EGLDisplay display; EGLSyncKHR value=EGL_NO_SYNC_KHR; PFNEGLDESTROYSYNCKHRPROC destroy;
    Sync(EGLDisplay d,PFNEGLDESTROYSYNCKHRPROC f):display(d),destroy(f) {}
    Sync(const Sync&)=delete;
    ~Sync() { if(value!=EGL_NO_SYNC_KHR) destroy(display,value); }
    bool reset() { if(value==EGL_NO_SYNC_KHR) return true; const auto result=destroy(display,value); value=EGL_NO_SYNC_KHR; return result==EGL_TRUE; }
};
bool extension(const char* list,const char* name) {
    if(!list) return false;
    const auto bytes=std::strlen(name);
    for(const char* p=list; (p=std::strstr(p,name)); p+=bytes)
        if((p==list || p[-1]==' ') && (!p[bytes] || p[bytes]==' ')) return true;
    return false;
}
#ifndef WDDM_BRIDGE_EGL_NATIVE_SYNC_DISABLED
int status(int fd) {
    sync_file_info info{};
    if(syscall(SYS_ioctl,fd,SYNC_IOC_FILE_INFO,&info) || info.num_fences!=1) return -10000;
    return info.status;
}
bool finish(int fd,int expected) {
    pollfd event{fd,POLLIN,0};
    return syscall(SYS_poll,&event,1,6500)==1 && (event.revents&POLLIN) && status(fd)==expected;
}
#endif
}
extern "C" int wddm_native_sync_file_test(EGLDisplay display,EGLContext context) {
    const auto failure=[](const char* stage){std::printf("EGL_NATIVE_SYNC_FAILURE stage=%s egl=%04x errno=%d\n",stage,eglGetError(),errno);return 1;};
    const bool advertised=extension(eglQueryString(display,EGL_EXTENSIONS),"EGL_ANDROID_native_fence_sync");
#ifdef WDDM_BRIDGE_EGL_NATIVE_SYNC_DISABLED
    (void)context;
    if(advertised) return failure("DisabledAdvertisement");
    std::puts("EGL_NATIVE_SYNC_DISABLED extensionAbsent=true desktop=false");
    return 0;
#else
    std::puts("EGL_NATIVE_SYNC_BEGIN callbacks=true nativeGpu=true kernel=true desktop=false");
    if(!advertised) return failure("Extension");
    auto create=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    auto destroy=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    auto dup=reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    auto wait=reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
    auto clientWait=reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    auto attribute=reinterpret_cast<PFNEGLGETSYNCATTRIBKHRPROC>(eglGetProcAddress("eglGetSyncAttribKHR"));
    auto exportFile=reinterpret_cast<int(*)(int,std::uint64_t)>(dlsym(RTLD_DEFAULT,"wddm_bridge_export_native_sync_file"));
    auto stats=reinterpret_cast<int(*)(wddm_sync_file_stats*)>(dlsym(RTLD_DEFAULT,"wddm_bridge_sync_file_info"));
    if(!create || !destroy || !dup || !wait || !clientWait || !attribute || !exportFile || !stats) return failure("Functions");
    d3d12_interop_device_info native{}; mesa_glinterop_device_info interop{};
    interop.version=2; interop.driver_data_size=sizeof native; interop.driver_data=&native;
    if(MesaGLInteropEGLQueryDeviceInfo(display,context,&interop)!=MESA_GLINTEROP_SUCCESS ||
        interop.driver_data_size!=sizeof native || !native.device || !native.queue) return failure("MesaDevice");
    auto import=[&](Fd& fd,Sync& sync) {
        const EGLint attributes[]={EGL_SYNC_NATIVE_FENCE_FD_ANDROID,fd.value,EGL_NONE};
        sync.value=create(display,EGL_SYNC_NATIVE_FENCE_ANDROID,attributes);
        if(sync.value==EGL_NO_SYNC_KHR) return false;
        fd.value=-1; // EGL owns the original; Mesa owns its independent duplicate.
        return true;
    };
    wddm_sync_file_stats baseline{};
    if(stats(&baseline) || baseline.errors) return failure("BaselineStats");
    std::printf("EGL_NATIVE_SYNC_BASELINE created=%llu signaled=%llu released=%llu held=%u pending=%u\n",
        (unsigned long long)baseline.created,(unsigned long long)baseline.signaled,(unsigned long long)baseline.released,baseline.held,baseline.pending);
    Sync exported(display,destroy),imported(display,destroy),zero(display,destroy),canceled(display,destroy),dependent(display,destroy);
    exported.value=create(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);
    if(exported.value==EGL_NO_SYNC_KHR) return failure("CreateExport");
    Fd first(dup(display,exported.value)),second(dup(display,exported.value));
    if(first.value<0 || second.value<0 || first.value==second.value ||
        !(fcntl(first.value,F_GETFD)&FD_CLOEXEC) || !(fcntl(second.value,F_GETFD)&FD_CLOEXEC) ||
        !finish(first.value,1) || !import(first,imported) || !exported.reset()) return failure("ExportDuplicateImport");
    if(clientWait(display,imported.value,0,0)!=EGL_CONDITION_SATISFIED_KHR || !finish(second.value,1) || !imported.reset())
        return failure("CompletedImport");
    second.reset();
    std::puts("EGL_NATIVE_SYNC_EXPORT create=true duplicateFds=true cloexec=true inputOwnership=EGL sourceSyncDestroyed=true importedComplete=true");
    ComPtr<ID3D12Fence> source,acknowledgement;
    HANDLE shared=nullptr;
    if(FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&source))) ||
        FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&shared))) return failure("Source");
    Fd cap(static_cast<int>(reinterpret_cast<std::intptr_t>(shared)));
    Fd zeroFd(exportFile(cap.value,0));
    if(zeroFd.value<0 || !finish(zeroFd.value,1) || !import(zeroFd,zero) ||
        clientWait(display,zero.value,0,0)!=EGL_CONDITION_SATISFIED_KHR || !zero.reset()) return failure("ZeroTarget");
    constexpr std::uint64_t target=(UINT64_C(1)<<32)+91;
    Fd future(exportFile(cap.value,target)); cap.reset();
    if(future.value<0 || status(future.value)!=0 || !import(future,imported)) return failure("ImportFuture");
    Fd retained(dup(display,imported.value));
    EGLint syncStatus=0;
    if(retained.value<0 || !(fcntl(retained.value,F_GETFD)&FD_CLOEXEC) || status(retained.value)!=0 ||
        clientWait(display,imported.value,0,0)!=EGL_TIMEOUT_EXPIRED_KHR ||
        attribute(display,imported.value,EGL_SYNC_STATUS_KHR,&syncStatus)!=EGL_TRUE || syncStatus!=EGL_UNSIGNALED_KHR)
        return failure("PendingStatus");
    ComPtr<ID3D12CommandQueue> release;
    D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type=D3D12_COMMAND_LIST_TYPE_COPY;
    if(FAILED(native.device->CreateCommandQueue(&desc,IID_PPV_ARGS(&release))) ||
        FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&acknowledgement)))) return failure("ReleaseQueue");
    const auto waited=wait(display,imported.value,0);
    dependent.value=create(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);
    Fd dependentFd(dependent.value==EGL_NO_SYNC_KHR?-1:dup(display,dependent.value));
    const bool dependentPending=dependentFd.value>=0 && status(dependentFd.value)==0 &&
        clientWait(display,dependent.value,0,0)==EGL_TIMEOUT_EXPIRED_KHR;
    const auto signaled=native.queue->Signal(acknowledgement.Get(),1);
    usleep(50000);
    const auto before=source->GetCompletedValue(),gated=acknowledgement->GetCompletedValue();
    const auto released=release->Signal(source.Get(),target); // Always release before diagnosing the observation.
    const auto completed=clientWait(display,imported.value,0,UINT64_C(5000000000));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(acknowledgement->GetCompletedValue()==0 && std::chrono::steady_clock::now()<deadline) usleep(1000);
    if(waited!=EGL_TRUE || FAILED(signaled) || FAILED(released) || before!=0 || gated!=0 ||
        completed!=EGL_CONDITION_SATISFIED_KHR || source->GetCompletedValue()!=target ||
        acknowledgement->GetCompletedValue()!=1 || !finish(retained.value,1) ||
        !dependentPending || !finish(dependentFd.value,1) ||
        clientWait(display,dependent.value,0,0)!=EGL_CONDITION_SATISFIED_KHR ||
        attribute(display,imported.value,EGL_SYNC_STATUS_KHR,&syncStatus)!=EGL_TRUE || syncStatus!=EGL_SIGNALED_KHR)
        return failure("GpuOrdering");
    dependentFd.reset(); if(!dependent.reset()) return failure("DependentDestroy");
    std::printf("EGL_NATIVE_SYNC_GPU target=%llu pendingClientWait=timeout pendingStatus=unsignaled gpuWait=true dependentEglExportBefore=pending dependentEglExportAfter=signaled acknowledgementBefore=0 acknowledgementAfter=1 independentGpuRelease=true completedClientWait=satisfied completedStatus=signaled cpuFenceWrites=false\n",
        static_cast<unsigned long long>(target));
    source.Reset();
    if(!imported.reset() || !import(retained,imported) ||
        clientWait(display,imported.value,0,0)!=EGL_CONDITION_SATISFIED_KHR || !imported.reset()) return failure("SourceLifetime");
    acknowledgement.Reset(); release.Reset();
    std::puts("EGL_NATIVE_SYNC_LIFETIME originalNativeFdClosed=true originalSourceReleased=true firstEglSyncDestroyed=true completedEglReimport=true zeroTargetImport=true");
    // Exercise an actual native residency-reference cycle after all GPU
    // dependencies retire. This does not claim forced physical page-out.
    ComPtr<ID3D12Resource> residentBuffer;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=65536; buffer.Height=1; buffer.DepthOrArraySize=1; buffer.MipLevels=1;
    buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if(FAILED(native.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COMMON,
        nullptr,IID_PPV_ARGS(&residentBuffer)))) return failure("ResidencyBuffer");
    ID3D12Pageable* pageable=residentBuffer.Get();
    const auto evicted=native.device->Evict(1,&pageable),resident=native.device->MakeResident(1,&pageable);
    if(FAILED(evicted) || FAILED(resident)) return failure("NativeResidencyCycle");
    residentBuffer.Reset();
    std::puts("EGL_NATIVE_SYNC_RESIDENCY evict=true makeResident=true nativeResource=true forcedPhysicalPageOut=false");
    Fd foreign(open("/dev/null",O_RDONLY|O_CLOEXEC));
    if(import(foreign,imported) || eglGetError()!=EGL_BAD_ATTRIBUTE || fcntl(foreign.value,F_GETFD)<0) return failure("ForeignOwnership");
    if(FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&source))) ||
        FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&shared))) return failure("CanceledSource");
    cap.value=static_cast<int>(reinterpret_cast<std::intptr_t>(shared));
    Fd cancelFd(exportFile(cap.value,1)); cap.reset();
    if(cancelFd.value<0 || status(cancelFd.value)!=0 || !import(cancelFd,canceled)) return failure("CanceledImport");
    Fd cancelCopy(dup(display,canceled.value)); source.Reset();
    if(cancelCopy.value<0 || !finish(cancelCopy.value,-ENODEV) ||
        clientWait(display,canceled.value,0,0)!=EGL_TIMEOUT_EXPIRED_KHR ||
        attribute(display,canceled.value,EGL_SYNC_STATUS_KHR,&syncStatus)!=EGL_TRUE || syncStatus!=EGL_UNSIGNALED_KHR ||
        import(cancelCopy,imported) || eglGetError()!=EGL_BAD_ATTRIBUTE || fcntl(cancelCopy.value,F_GETFD)<0 || !canceled.reset())
        return failure("CanceledStatusOwnership");
    cancelCopy.reset();
    std::puts("EGL_NATIVE_SYNC_REJECT foreignError=badAttribute failedInputFdRetained=true canceledStatus=-19 canceledClientWait=timeout canceledEglStatus=unsignaled canceledReimport=badAttribute");
    wddm_sync_file_stats info{};
    const auto cleanupDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
        if(stats(&info)) return failure("Stats");
        if(!info.pending && info.created>=baseline.created+5 && info.released>=baseline.released+5 && info.signaled+info.errors==info.created) break;
        usleep(1000);
    } while(std::chrono::steady_clock::now()<cleanupDeadline);
    if(info.pending || info.created<baseline.created+5 || info.created>baseline.created+16 || info.errors!=1 ||
        info.released<baseline.released+5 || info.signaled+info.errors!=info.created || info.released+info.held!=info.created || info.held>16)
        return failure("CleanupCounts");
    if(eglGetError()!=EGL_SUCCESS) return failure("FinalEglError");
    std::printf("EGL_NATIVE_SYNC_COUNTS created=%llu signaled=%llu errors=%llu released=%llu held=%u pending=%u mesaBatchFencesIncluded=true\n",
        (unsigned long long)info.created,(unsigned long long)info.signaled,(unsigned long long)info.errors,(unsigned long long)info.released,info.held,info.pending);
    std::puts("EGL_NATIVE_SYNC_COMPLETE externalCases=5 callbacksVerified=true generalInterop=false desktop=false");
    return 0;
#endif
}
