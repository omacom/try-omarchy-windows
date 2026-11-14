// SPDX-License-Identifier: MIT
// Trace genuine native texture export/import from the current Mesa device.
// Native resource FDs alone do not establish dma-buf or compositor support.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
struct HGLRC__;
using HGLRC = HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef WDDM_BRIDGE_RESOURCE_DMABUF_TEST
#include "guest-kernel/wddm_aperture.h"
#include <linux/dma-buf.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#endif
#ifdef WDDM_BRIDGE_RESOURCE_FD_RECIPIENT
#include <poll.h>
#include <sys/syscall.h>
#endif

using Microsoft::WRL::ComPtr;

extern "C" int wddm_mesa_resource_fd_test(EGLDisplay display, EGLContext context) {
#ifdef WDDM_BRIDGE_RESOURCE_DMABUF_TEST
    std::puts("MESA_RESOURCE_FD_BEGIN width=3840 height=2160 format=28 native=true dmaBuf=true desktop=false");
#else
    std::puts("MESA_RESOURCE_FD_BEGIN width=3840 height=2160 format=28 native=true dmaBuf=false desktop=false");
#endif
    const auto failed = [](const char* stage, HRESULT result) {
        std::printf("MESA_RESOURCE_FD_FAILURE stage=%s status=%08x errno=%d\n", stage,
                    static_cast<unsigned>(result), errno);
        return 1;
    };
    d3d12_interop_device_info native{};
    mesa_glinterop_device_info info{};
    info.version = 2; info.driver_data_size = sizeof native; info.driver_data = &native;
    if (MesaGLInteropEGLQueryDeviceInfo(display, context, &info) != MESA_GLINTEROP_SUCCESS ||
        info.driver_data_size != sizeof native || !native.device || !native.queue)
        return failed("MesaDeviceInterop", E_FAIL);
    using Prepare = int (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    const auto prepare = reinterpret_cast<Prepare>(dlsym(RTLD_DEFAULT, "wddm_bridge_prepare_native_texture"));
    if (!prepare || prepare(3840, 2160, 28, 1)) return failed("NativeTextureProfile", E_FAIL);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = 3840; desc.Height = 2160;
    desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> source;
    auto result = native.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&source));
    if (FAILED(result)) return failed("CreateSharedTexture", result);
    std::puts("MESA_RESOURCE_FD_TEXTURE_CREATED width=3840 height=2160 shared=true");
    HANDLE handle = nullptr;
    result = native.device->CreateSharedHandle(source.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(result)) return failed("ExportTexture", result);
    const auto value = reinterpret_cast<intptr_t>(handle);
    if (value < 0 || value > INT32_MAX) return failed("ExportDescriptor", E_FAIL);
    auto fd = static_cast<int>(value);
    if (fcntl(fd, F_GETFD) < 0) return failed("ExportDescriptor", E_FAIL);
#ifdef WDDM_BRIDGE_RESOURCE_DMABUF_TEST
    const auto wrap=reinterpret_cast<int(*)(int,std::uint64_t)>(dlsym(RTLD_DEFAULT,"wddm_bridge_export_native_buffer"));
    const auto bufferInfo=reinterpret_cast<int(*)(int,std::uint64_t*)>(dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_info"));
    const auto bufferStats=reinterpret_cast<int(*)(wddm_native_buffer_stats*)>(dlsym(RTLD_DEFAULT,"wddm_bridge_native_buffer_stats"));
    wddm_native_buffer_stats baseline{};
    constexpr std::uint64_t logicalBytes=UINT64_C(3840)*2160*4;
    if(!wrap || !bufferInfo || !bufferStats || bufferStats(&baseline) || baseline.held || baseline.bytes || baseline.created!=baseline.released) {
        close(fd); return failed("BufferHooksStats",E_FAIL);
    }
    std::uint64_t logical=0; errno=0;
#ifdef WDDM_BRIDGE_RESOURCE_DMABUF_DISABLED
    if(bufferInfo(fd,&logical)!=-1 || errno!=EOPNOTSUPP || fcntl(fd,F_GETFD)<0)
        return failed("DisabledBufferInfo",E_FAIL);
    errno=0;
    if(wrap(fd,logicalBytes)!=-1 || errno!=EOPNOTSUPP || fcntl(fd,F_GETFD)<0)
        return failed("DisabledBufferExport",E_FAIL);
    if(close(fd) || bufferStats(&baseline) || baseline.held || baseline.bytes || baseline.created || baseline.released)
        return failed("DisabledBufferCleanup",E_FAIL);
    std::puts("NATIVE_BUFFER_GPU_DISABLED exportUnsupported=true infoUnsupported=true inputFdRetained=true dmaBufsCreated=0 gpuImport=false desktop=false");
    return 0;
#endif
    if(bufferInfo(fd,&logical)!=-1 || errno!=EINVAL || fcntl(fd,F_GETFD)<0) {
        close(fd); return failed("RawCapabilityRejected",E_FAIL);
    }
    const auto bufferFd=wrap(fd,logicalBytes);
    const auto originalClosed=close(fd); fd=bufferFd;
    if(fd<0 || originalClosed) return failed("NativeBufferExport",E_FAIL);
    if(!(fcntl(fd,F_GETFD)&FD_CLOEXEC) || lseek(fd,0,SEEK_END)!=static_cast<off_t>(logicalBytes) ||
       lseek(fd,0,SEEK_SET)!=0 || bufferInfo(fd,&logical) || logical!=logicalBytes) {
        close(fd); return failed("NativeBufferIdentity",E_FAIL);
    }
    struct dma_buf_sync cpu{}; cpu.flags=DMA_BUF_SYNC_START|DMA_BUF_SYNC_READ; errno=0;
    if(syscall(SYS_ioctl,fd,DMA_BUF_IOCTL_SYNC,&cpu)!=-1 || errno!=EOPNOTSUPP) {
        close(fd); return failed("NativeBufferCpuAccess",E_FAIL);
    }
    const auto duplicated=fcntl(fd,F_DUPFD_CLOEXEC,0);
    const auto bufferClosed=close(fd); fd=duplicated;
    if(fd<0 || bufferClosed || bufferInfo(fd,&logical) || logical!=logicalBytes) return failed("NativeBufferDupLifetime",E_FAIL);
    handle=reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(fd));
    std::printf("NATIVE_BUFFER_GPU_EXPORT dmaBuf=true nativeFdClosed=true duplicateLifetime=true cloexec=true logicalBytes=%llu cpuAccess=false eglImage=false gbm=false\n",
        (unsigned long long)logicalBytes);
#endif
    std::puts("MESA_RESOURCE_FD_EXPORTED native=true rawHostHandle=false");
    ComPtr<ID3D12Resource> imported;
    result = native.device->OpenSharedHandle(handle, IID_PPV_ARGS(&imported));
    const auto closeResult = close(fd);
    if (FAILED(result) || !imported || closeResult) return failed("ImportTexture", result);
    const auto importedDesc = imported->GetDesc();
    if (importedDesc.Dimension != desc.Dimension || importedDesc.Width != desc.Width || importedDesc.Height != desc.Height ||
        importedDesc.Format != desc.Format || importedDesc.Flags != desc.Flags)
        return failed("ImportedDescription", E_FAIL);
    std::puts("MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true");
#ifdef WDDM_BRIDGE_RESOURCE_DMABUF_TEST
    wddm_native_buffer_stats finished{};
    if(bufferStats(&finished) || finished.held || finished.bytes || finished.created!=baseline.created+1 ||
       finished.released!=baseline.released+1) return failed("NativeBufferFdCleanup",E_FAIL);
    std::printf("NATIVE_BUFFER_GPU_LIFETIME originalNativeFdClosed=true dmaBufFdClosed=true importedGpuReference=true baselineCreated=%llu created=%llu released=%llu held=0 bytes=0\n",
        (unsigned long long)baseline.created,(unsigned long long)finished.created,(unsigned long long)finished.released);
#endif
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowBytes = 0, totalBytes = 0;
    native.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    if (rows != 2160 || rowBytes != 3840 * 4 || footprint.Offset || footprint.Footprint.RowPitch < rowBytes ||
        footprint.Footprint.RowPitch % 256 || totalBytes > 32u * 1024 * 1024 || !totalBytes)
        return failed("ReadbackFootprint", E_FAIL);
    // Readback is an acceptance diagnostic, not the display path. The normal
    // probe uses a bounded band; the separate full-buffer build reproduces
    // the aperture capacity failure without changing the rendered workload.
#ifdef WDDM_BRIDGE_RESOURCE_FD_FULL_READBACK
    constexpr unsigned BandRows = 2160;
#else
    constexpr unsigned BandRows = 512;
#endif
    const auto readbackBytes = static_cast<UINT64>(footprint.Footprint.RowPitch) * BandRows;
#ifdef WDDM_BRIDGE_RESOURCE_FD_FULL_READBACK
    std::printf("MESA_RESOURCE_FD_READBACK_PROFILE fullBuffer=true bandsPerRound=1 bytes=%llu\n", static_cast<unsigned long long>(readbackBytes));
#endif
    D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = readbackBytes;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    result = native.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(result)) return failed("CreateReadback", result);
    ComPtr<ID3D12CommandAllocator> allocator;
    result = native.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    if (FAILED(result)) return failed("CreateAllocator", result);
    ComPtr<ID3D12GraphicsCommandList> commands;
    result = native.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
    if (FAILED(result)) return failed("CreateCommandList", result);
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{}; rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtvDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv;
    result = native.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtv));
    if (FAILED(result)) return failed("CreateRtv", result);
    ComPtr<ID3D12Fence> retired;
    result = native.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retired));
    if (FAILED(result)) return failed("CreateRetirementFence", result);
    const auto transition = [&](ID3D12Resource* texture, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture; barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before; barrier.Transition.StateAfter = after;
        commands->ResourceBarrier(1, &barrier);
    };
    UINT64 retirementTarget = 0;
#ifdef WDDM_BRIDGE_RESOURCE_FD_BAD_COLOR
    std::puts("MESA_RESOURCE_FD_NEGATIVE_CONTROL kind=sourceColor expectedFailure=SharedPixels");
#endif
    for (unsigned round = 0; round < 2; ++round) {
        if (round) source.Reset();
        auto writer = round ? imported.Get() : source.Get();
        FLOAT color[4] = {round ? 0.0f : 1.0f, round ? 1.0f : 0.0f, 0.0f, 1.0f};
#ifdef WDDM_BRIDGE_RESOURCE_FD_BAD_COLOR
        if (!round) { color[0] = 0.0f; color[2] = 1.0f; }
#endif
        const std::uint8_t expected[4] = {round ? std::uint8_t{0} : std::uint8_t{255}, round ? std::uint8_t{255} : std::uint8_t{0}, 0, 255};
        std::uint64_t digest = UINT64_C(14695981039346656037);
        unsigned bands = 0;
        for (unsigned firstRow = 0; firstRow < 2160; firstRow += BandRows) {
            if (retirementTarget) {
                result = allocator->Reset(); if (FAILED(result)) return failed("ResetAllocator", result);
                result = commands->Reset(allocator.Get(), nullptr); if (FAILED(result)) return failed("ResetCommands", result);
            }
            const unsigned remaining = 2160 - firstRow;
            const unsigned bandRows = remaining < BandRows ? remaining : BandRows;
            if (!firstRow) {
                const auto descriptor = rtv->GetCPUDescriptorHandleForHeapStart();
                native.device->CreateRenderTargetView(writer, nullptr, descriptor);
                transition(writer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                commands->ClearRenderTargetView(descriptor, color, 0, nullptr);
                transition(writer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
            }
            transition(imported.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
            destination.PlacedFootprint.Footprint.Height = bandRows;
            D3D12_TEXTURE_COPY_LOCATION origin{}; origin.pResource = imported.Get(); origin.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, firstRow, 0, 3840, firstRow + bandRows, 1};
            commands->CopyTextureRegion(&destination, 0, 0, 0, &origin, &box);
            transition(imported.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            result = commands->Close(); if (FAILED(result)) return failed("CloseCommands", result);
            ID3D12CommandList* submitted[] = {commands.Get()}; native.queue->ExecuteCommandLists(1, submitted);
            ++retirementTarget;
            result = native.queue->Signal(retired.Get(), retirementTarget); if (FAILED(result)) return failed("SignalRetirement", result);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (retired->GetCompletedValue() < retirementTarget && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (retired->GetCompletedValue() != retirementTarget) return failed("Retirement", E_FAIL);
            void* mapped = nullptr; const D3D12_RANGE readable{0, static_cast<SIZE_T>(bandRows) * footprint.Footprint.RowPitch};
            result = readback->Map(0, &readable, &mapped); if (FAILED(result) || !mapped) return failed("ReadbackMap", result);
            bool matches = true;
            for (unsigned y = 0; y < bandRows && matches; ++y) {
                const auto row = static_cast<const std::uint8_t*>(mapped) + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
                for (unsigned x = 0; x < 3840 && matches; ++x)
                    for (unsigned channel = 0; channel < 4; ++channel) {
                        const auto value = row[x * 4 + channel];
                        if (value != expected[channel]) { matches = false; break; }
                        digest = (digest ^ value) * UINT64_C(1099511628211);
                    }
            }
            const D3D12_RANGE written{0,0}; readback->Unmap(0, &written);
            if (!matches) return failed("SharedPixels", E_FAIL);
            ++bands;
        }
        std::printf("MESA_RESOURCE_FD_SHARED_STORAGE round=%u writer=%s importedRead=true sourceReleased=%s fdClosed=true width=3840 height=2160 pixels=8294400 rgba=%s bands=%u readbackBytes=%llu fnv1a=%016llx\n",
            round + 1, round ? "imported" : "source", round ? "true" : "false", round ? "00ff00ff" : "ff0000ff", bands,
            static_cast<unsigned long long>(readbackBytes), static_cast<unsigned long long>(digest));
    }
#ifdef WDDM_BRIDGE_RESOURCE_DMABUF_TEST
    std::puts("MESA_RESOURCE_FD_COMPLETE rounds=2 pixelsPerRound=8294400 native=true dmaBuf=true desktop=false");
    std::puts("NATIVE_BUFFER_GPU_COMPLETE sharedStorage=true sourceReleased=true eglImage=false gbm=false drm=false desktop=false");
#else
    std::puts("MESA_RESOURCE_FD_COMPLETE rounds=2 pixelsPerRound=8294400 native=true dmaBuf=false desktop=false");
#endif
#ifdef WDDM_BRIDGE_RESOURCE_FD_RECIPIENT
    // This recipient holds a real exported kernel capability, but does no
    // GPU work. GPU imports in independent processes require a coordinator.
    HANDLE retained=nullptr;
    result=native.device->CreateSharedHandle(imported.Get(),nullptr,GENERIC_ALL,nullptr,&retained);
    if (FAILED(result)) return failed("RecipientExport",result);
    const auto retainedValue=reinterpret_cast<intptr_t>(retained);
    if (retainedValue<0 || retainedValue>INT32_MAX) return failed("RecipientDescriptor",E_FAIL);
    const auto retainedFd=static_cast<int>(retainedValue);
    int ready[2];
    if (pipe2(ready,O_CLOEXEC)) return failed("RecipientPipe",E_FAIL);
    const auto producer=getpid(),recipient=fork();
    if (recipient<0) return failed("RecipientFork",E_FAIL);
    if (!recipient) {
        syscall(SYS_close,ready[0]);
        const auto pidfd=static_cast<int>(syscall(SYS_pidfd_open,producer,0));
        const char marker=pidfd>=0 ? 'R' : 'F';
        if (write(ready[1],&marker,1)!=1 || pidfd<0) _exit(1);
        syscall(SYS_close,ready[1]);
        pollfd exited{pidfd,POLLIN,0};
        const auto exitDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        int waitResult=0;
        do {
            const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(exitDeadline-std::chrono::steady_clock::now()).count();
            if (remaining<=0) break;
            waitResult=poll(&exited,1,static_cast<int>(remaining));
        } while (waitResult<0 && errno==EINTR);
        if (waitResult!=1 || !(exited.revents&POLLIN) || fcntl(retainedFd,F_GETFD)<0) {
            static constexpr char failure[]="MESA_RESOURCE_FD_FAILURE stage=RecipientProducerExit status=80004005 errno=0\n";
            if (write(STDERR_FILENO,failure,sizeof(failure)-1)!=sizeof(failure)-1) _exit(2);
            _exit(1);
        }
        poll(nullptr,0,250); // let the broker enter its post-producer drain
        static constexpr char alive[]="MESA_RESOURCE_FD_RECIPIENT producerExited=true capabilityOpen=true gpuWork=false\n";
        if (write(STDERR_FILENO,alive,sizeof(alive)-1)!=sizeof(alive)-1) _exit(1);
        poll(nullptr,0,1750);
        static constexpr char closing[]="MESA_RESOURCE_FD_RECIPIENT closingCapability=true producerExited=true gpuWork=false\n";
        if (write(STDERR_FILENO,closing,sizeof(closing)-1)!=sizeof(closing)-1 || syscall(SYS_close,retainedFd)) _exit(1);
        _exit(0);
    }
    syscall(SYS_close,ready[1]); pollfd readable{ready[0],POLLIN,0}; char marker=0;
    if (poll(&readable,1,5000)!=1 || read(ready[0],&marker,1)!=1 || marker!='R') return failed("RecipientReady",E_FAIL);
    syscall(SYS_close,ready[0]);
    if (syscall(SYS_close,retainedFd)) return failed("RecipientProducerClose",E_FAIL);
    std::puts("MESA_RESOURCE_FD_RECIPIENT_ARMED producerFdClosed=true recipientFdInherited=true gpuWork=false");
#endif
    return 0;
}
