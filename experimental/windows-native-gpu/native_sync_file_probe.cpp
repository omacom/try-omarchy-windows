// SPDX-License-Identifier: MIT
// Genuine GPU timeline -> Linux sync_file. EGL/Hyprland integration is separate.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxg/d3dkmthk.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
struct HGLRC__; using HGLRC=HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include "guest-kernel/wddm_aperture.h"
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <poll.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <initializer_list>
using Microsoft::WRL::ComPtr;
template<class T> static bool call(int fd,unsigned nr,T& value) {
    if (!ioctl(fd,_IOC(_IOC_READ|_IOC_WRITE,'G',nr,sizeof value),&value)) return true;
    std::printf("NATIVE_SYNC_FILE_FAILURE nr=%u errno=%d\n",nr,errno); return false;
}
struct Descriptor {
    int fd=-1;
    explicit Descriptor(int n=-1):fd(n) {}
    ~Descriptor() { if(fd>=0) close(fd); }
    void reset() { if(fd>=0) close(fd); fd=-1; }
};
static int status(int fd) {
    sync_fence_info fence{}; sync_file_info info{};
    info.num_fences=1; info.sync_fence_info=reinterpret_cast<std::uintptr_t>(&fence);
    if (syscall(SYS_ioctl,fd,SYNC_IOC_FILE_INFO,&info) || info.num_fences!=1 || fence.status!=info.status) return -10000;
    const bool native=!std::strcmp(fence.driver_name,"wddm-aperture") && !std::strcmp(fence.obj_name,"native-monitored-fence");
    // Linux 6.18 dma_fence_{driver,timeline}_name replaces names after signal.
    // Pending fences must still identify this exporter; completion/error
    // requires the matching standard status, never a name as a substitute.
    const bool detached=info.status!=0 && !std::strcmp(fence.driver_name,"detached-driver") && !std::strcmp(fence.obj_name,"signaled-timeline");
    if(!native && !detached) return -10000;
    return info.status;
}
static int ready(int fd,int timeout) {
    pollfd event{fd,POLLIN,0};
    const auto n=syscall(SYS_poll,&event,1,timeout);
    if(n<0 || (n>0 && !(event.revents&POLLIN))) return -1;
    return static_cast<int>(n);
}
static bool finish(int fd,int expected) { return ready(fd,6500)==1 && status(fd)==expected; }
static int transfer(int socket,int fd,bool send) {
    char byte='F'; iovec vector{&byte,1};
    union { cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } buffer{};
    msghdr message{}; message.msg_iov=&vector; message.msg_iovlen=1;
    message.msg_control=buffer.bytes; message.msg_controllen=sizeof buffer;
    if(send) {
        auto h=CMSG_FIRSTHDR(&message); h->cmsg_level=SOL_SOCKET; h->cmsg_type=SCM_RIGHTS; h->cmsg_len=CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(h),&fd,sizeof fd);
        return syscall(SYS_sendmsg,socket,&message,MSG_NOSIGNAL)==1 ? fd:-1;
    }
    if(syscall(SYS_recvmsg,socket,&message,MSG_CMSG_CLOEXEC)!=1 || (message.msg_flags&(MSG_CTRUNC|MSG_TRUNC))) return -1;
    auto h=CMSG_FIRSTHDR(&message);
    if(!h || h->cmsg_level!=SOL_SOCKET || h->cmsg_type!=SCM_RIGHTS || h->cmsg_len!=CMSG_LEN(sizeof(int))) return -1;
    std::memcpy(&fd,CMSG_DATA(h),sizeof fd); return fd;
}
extern "C" int wddm_native_sync_file_test(EGLDisplay display,EGLContext context) {
    const auto failure=[](const char* name){std::printf("NATIVE_SYNC_FILE_FAILURE stage=%s errno=%d\n",name,errno);return 1;};
    std::puts("NATIVE_SYNC_FILE_BEGIN gpu=true kernel=true eglNativeFence=false desktop=false");
    auto exportFile=reinterpret_cast<int(*)(const void*,std::uint64_t)>(dlsym(RTLD_DEFAULT,"wddm_bridge_export_sync_file"));
    auto stats=reinterpret_cast<int(*)(wddm_sync_file_stats*)>(dlsym(RTLD_DEFAULT,"wddm_bridge_sync_file_info"));
    auto nativeWord=reinterpret_cast<const void*(*)(int)>(dlsym(RTLD_DEFAULT,"wddm_bridge_native_fence_word"));
    if(!exportFile || !stats || !nativeWord) return failure("BridgeHooks");
    d3d12_interop_device_info native{}; mesa_glinterop_device_info interop{};
    interop.version=2; interop.driver_data_size=sizeof native; interop.driver_data=&native;
    if(MesaGLInteropEGLQueryDeviceInfo(display,context,&interop)!=MESA_GLINTEROP_SUCCESS ||
        interop.driver_data_size!=sizeof native || !native.device || !native.queue) return failure("MesaDevice");
    ComPtr<ID3D12Fence> source,control;
    if(FAILED(native.device->CreateFence(57,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&source)))) return failure("Source");
    HANDLE handle=nullptr;
    if(FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&handle))) return failure("ExportNative");
    Descriptor shared(static_cast<int>(reinterpret_cast<std::intptr_t>(handle)));
    const auto word=nativeWord(shared.fd);
    if(!word || *static_cast<const volatile std::uint64_t*>(word)!=57) return failure("InitialValue");
    for(const auto target:{std::uint64_t(0),UINT64_MAX}) {
        errno=0; if(exportFile(word,target)!=-1 || errno!=EINVAL) return failure("InvalidTarget");
    }
    errno=0; if(exportFile(static_cast<const char*>(word)+8,58)!=-1 || errno!=EBADF) return failure("WrongWord");
    Descriptor done(exportFile(word,57));
    const auto completeReady=done.fd<0 ? -1:ready(done.fd,6500);
    sync_fence_info detail{}; sync_file_info fileInfo{}; fileInfo.num_fences=1;
    fileInfo.sync_fence_info=reinterpret_cast<std::uintptr_t>(&detail);
    const auto infoResult=done.fd<0 ? -1:syscall(SYS_ioctl,done.fd,SYNC_IOC_FILE_INFO,&fileInfo);
    std::printf("NATIVE_SYNC_FILE_INFO fd=%d flags=%d ready=%d ioctl=%ld fences=%u status=%d fenceStatus=%d driver=%s timeline=%s\n",
        done.fd,done.fd<0?-1:fcntl(done.fd,F_GETFD),completeReady,infoResult,fileInfo.num_fences,fileInfo.status,detail.status,detail.driver_name,detail.obj_name);
    if(done.fd<0 || !(fcntl(done.fd,F_GETFD)&FD_CLOEXEC) || completeReady!=1 || status(done.fd)!=1) return failure("AlreadyComplete");
    done.reset();
    if(FAILED(native.device->OpenSharedHandle(handle,IID_PPV_ARGS(&control)))) return failure("ImportNative");
    shared.reset();
    constexpr std::uint64_t target=(UINT64_C(1)<<32)+58;
    Descriptor pending(exportFile(word,target));
    if(pending.fd<0 || status(pending.fd)!=0 || ready(pending.fd,0)!=0) return failure("PendingBeforeGpuRelease");
    int sockets[2]; if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,sockets)) return failure("RecipientSocket");
    const auto child=fork(); if(child<0) return failure("RecipientFork");
    if(!child) {
        syscall(SYS_close,sockets[0]); syscall(SYS_close,pending.fd);
        const auto received=transfer(sockets[1],-1,false);
        if(received<0 || !(syscall(SYS_fcntl,received,F_GETFD)&FD_CLOEXEC) ||
            status(received)!=0 || ready(received,0)!=0 || syscall(SYS_write,sockets[1],"P",1)!=1) _exit(2);
        if(!finish(received,1)) _exit(3);
        const char receipt=static_cast<unsigned>(syscall(SYS_getuid))==65534 ? 'S':'R';
        if(syscall(SYS_write,sockets[1],&receipt,1)!=1) _exit(4);
        syscall(SYS_close,received); syscall(SYS_close,sockets[1]); _exit(0);
    }
    Descriptor channel(sockets[0]); close(sockets[1]);
    if(transfer(channel.fd,pending.fd,true)<0) return failure("Transfer");
    pending.reset(); char receipt=0;
    if(read(channel.fd,&receipt,1)!=1 || receipt!='P') return failure("RecipientPending");
    if(FAILED(native.queue->Signal(control.Get(),target))) return failure("GpuSignal");
    int outcome=0;
    if(read(channel.fd,&receipt,1)!=1 || (receipt!='S' && receipt!='R') || waitpid(child,&outcome,0)!=child ||
        !WIFEXITED(outcome) || WEXITSTATUS(outcome)) return failure("RecipientCompletion");
    std::printf("NATIVE_SYNC_FILE_GPU target=%llu observed=%llu pendingBeforeRelease=true recipientUid=%u parentFdClosed=true childExit=0\n",
        static_cast<unsigned long long>(target),static_cast<unsigned long long>(control->GetCompletedValue()),receipt=='S'?65534:0);
    if(control->GetCompletedValue()!=target) return failure("NativeCompletion");
    Descriptor canceled(exportFile(word,target+1)),abandoned(exportFile(word,target+1));
    if(canceled.fd<0 || abandoned.fd<0 || status(canceled.fd)!=0 || status(abandoned.fd)!=0) return failure("CancellationPending");
    abandoned.reset();
    source.Reset();
    if(!finish(canceled.fd,-ENODEV)) return failure("Revocation");
    canceled.reset(); control.Reset();
    std::puts("NATIVE_SYNC_FILE_REVOKE status=-19 sourceRevoked=true abandonedFdClosed=true");
    if(FAILED(native.device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&source)))) return failure("TimeoutSource");
    if(FAILED(native.device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&handle))) return failure("TimeoutExport");
    shared.fd=static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
    const auto timeoutWord=nativeWord(shared.fd); shared.reset();
    if(!timeoutWord) return failure("TimeoutWord");
    Descriptor timeout(exportFile(timeoutWord,1));
    if(timeout.fd<0 || !finish(timeout.fd,-ETIMEDOUT)) return failure("Timeout");
    timeout.reset(); source.Reset();
    wddm_sync_file_stats info{};
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    do {
        if(stats(&info)) return failure("Stats");
        if(!info.held && !info.pending) break;
        usleep(1000);
    } while(std::chrono::steady_clock::now()<deadline);
    if(info.held || info.pending || info.created!=5 || info.signaled!=2 || info.errors!=3 || info.released!=5)
        return failure("Counts");
    std::puts("NATIVE_SYNC_FILE_COMPLETE created=5 signaled=2 errors=3 released=5 held=0 pending=0 timeoutStatus=-110 eglNativeFence=false desktop=false");
    return 0;
}
