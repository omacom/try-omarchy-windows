// SPDX-License-Identifier: MIT
// Explicit pre-stream diagnostic in the real compositor's D3D12 device.
#include <wsl/winadapter.h>
#include <wsl/wrladapter.h>
#include <directx/d3d12.h>
#include <dxguids/dxguids.h>
#include <EGL/egl.h>
struct HGLRC__;using HGLRC=HGLRC__*;
#define _WINDEF_
#include <GL/mesa_glinterop.h>
#undef _WINDEF_
#include <d3d12_interop_public.h>
#include "gpu_pressure_pattern.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dlfcn.h>
#include <unistd.h>
using Microsoft::WRL::ComPtr;
namespace {
[[noreturn]] void fail(const char* stage,HRESULT status=E_FAIL) {
    std::fprintf(stderr,"D3D12_VA_PRESSURE_FAILURE stage=%s hresult=%08x retainUntilOwnedVmExit=true\n",stage,static_cast<unsigned>(status));
    std::fflush(nullptr);
    // A timeout or partial native failure must not release objects underneath
    // GPU work. The existing primary-pidfd path stops the VM before teardown.
    _exit(125);
}
void check(HRESULT result,const char* stage) { if(FAILED(result)) fail(stage,result); }
void completed(ID3D12Fence* fence,std::uint64_t target) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    do {
        const auto value=fence->GetCompletedValue();
        if(value==UINT64_MAX) fail("DeviceRemoved");
        if(value>=target) return;
        usleep(1000);
    } while(std::chrono::steady_clock::now()<deadline);
    fail("GpuFenceDeadline");
}
D3D12_RESOURCE_DESC buffer(std::uint64_t bytes) {
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;
    desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}
void transition(ID3D12GraphicsCommandList* command,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;command->ResourceBarrier(1,&barrier);
}
void expected(std::array<unsigned char,gpu_pressure::TransferBytes>& output,unsigned pass,unsigned index) {
    for(unsigned block=0;block<gpu_pressure::Blocks;++block)
        for(std::size_t n=0;n<gpu_pressure::BlockBytes;++n)
            output[block*gpu_pressure::BlockBytes+n]=gpu_pressure::byte(pass,index,block,n);
}
}
extern "C" int wddm_gpu_va_pressure_probe(EGLDisplay display,EGLContext context) {
    using namespace gpu_pressure;
    auto query=reinterpret_cast<int(*)(EGLDisplay,EGLContext,mesa_glinterop_device_info*)>(dlsym(RTLD_DEFAULT,"MesaGLInteropEGLQueryDeviceInfo"));
    auto glString=reinterpret_cast<const unsigned char*(*)(unsigned)>(dlsym(RTLD_DEFAULT,"glGetString"));
    if(!query || !glString || !display || !context) fail("CurrentContextHooks");
    const auto renderer=reinterpret_cast<const char*>(glString(0x1f01));
    if(!renderer || !std::strstr(renderer,"D3D12 (NVIDIA") || std::strstr(renderer,"llvmpipe") || std::strstr(renderer,"softpipe")) fail("NvidiaRenderer");
    d3d12_interop_device_info native{};mesa_glinterop_device_info interop{};
    interop.version=2;interop.driver_data_size=sizeof native;interop.driver_data=&native;
    const int queried=query(display,context,&interop);
    std::printf("D3D12_VA_PRESSURE_DEVICE queryResult=%d mesaVendor=%u driverBytes=%u nativeDevice=%s nativeQueue=%s adapterLuid=%016llx\n",
        queried,interop.vendor_id,interop.driver_data_size,native.device?"true":"false",native.queue?"true":"false",
        static_cast<unsigned long long>(native.adapter_luid));std::fflush(stdout);
    if(queried!=MESA_GLINTEROP_SUCCESS || interop.driver_data_size!=sizeof native ||
       !native.device || !native.queue) fail("MesaNativeDevice");
    // This Mesa backend leaves pipe_caps.vendor_id at its default. Match the
    // actual D3D12 device instead; host provenance also pins the NVIDIA adapter.
    const auto luid=native.device->GetAdapterLuid();
    std::uint64_t deviceLuid=0;static_assert(sizeof luid==sizeof deviceLuid);
    std::memcpy(&deviceLuid,&luid,sizeof luid);
    if(!deviceLuid || deviceLuid!=native.adapter_luid) fail("NativeAdapterLuid");
    if(native.queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT) fail("QueueType");
    std::printf("D3D12_VA_PRESSURE_BEGIN heaps=%u heapBytes=%llu passes=%u totalVirtualBytes=%llu preStream=true nvidia=true ordinaryFrameCodeChanged=false\n",
        HeapCount,static_cast<unsigned long long>(HeapBytes),PassCount,static_cast<unsigned long long>(HeapCount*HeapBytes));std::fflush(stdout);
    std::array<ComPtr<ID3D12Heap>,HeapCount> heaps;
    std::array<ComPtr<ID3D12Resource>,HeapCount> resources;
    std::array<std::uint64_t,HeapCount> addresses{};
    const auto resourceDesc=buffer(HeapBytes);
    for(unsigned index=0;index<HeapCount;++index) {
        D3D12_HEAP_DESC heap{};heap.SizeInBytes=HeapBytes;heap.Alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        heap.Properties.Type=D3D12_HEAP_TYPE_DEFAULT;heap.Properties.CreationNodeMask=heap.Properties.VisibleNodeMask=1;
        heap.Flags=static_cast<D3D12_HEAP_FLAGS>(D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS|D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT);
        check(native.device->CreateHeap(&heap,IID_PPV_ARGS(&heaps[index])),"CreateNotResidentHeap");
        check(native.device->CreatePlacedResource(heaps[index].Get(),0,&resourceDesc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resources[index])),"CreatePlacedBuffer");
        addresses[index]=resources[index]->GetGPUVirtualAddress();
        if(!addresses[index] || addresses[index]%65536 || addresses[index]>(1ull<<48)-HeapBytes) fail("GpuAddress");
        for(unsigned other=0;other<index;++other)
            if(addresses[index]<addresses[other]+HeapBytes && addresses[other]<addresses[index]+HeapBytes) fail("OverlappingGpuAddresses");
        std::printf("D3D12_VA_PRESSURE_ALLOCATION index=%u gpuVa=%llu bytes=%llu createNotResident=true cpuMapped=false\n",index,
            static_cast<unsigned long long>(addresses[index]),static_cast<unsigned long long>(HeapBytes));std::fflush(stdout);
    }
    ComPtr<ID3D12Resource> upload,readback;
    D3D12_HEAP_PROPERTIES properties{};properties.CreationNodeMask=properties.VisibleNodeMask=1;
    auto transferDesc=buffer(TransferBytes);properties.Type=D3D12_HEAP_TYPE_UPLOAD;
    check(native.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&transferDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"UploadBuffer");
    transferDesc=buffer(2*TransferBytes);properties.Type=D3D12_HEAP_TYPE_READBACK;
    check(native.device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&transferDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"ReadbackBuffer");
    void* mappedUpload=nullptr;const D3D12_RANGE noRead{0,0};
    check(upload->Map(0,&noRead,&mappedUpload),"UploadMap");if(!mappedUpload) fail("UploadPointer");
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;ComPtr<ID3D12Fence> fence;
    check(native.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Allocator");
    check(native.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"CommandList");
    check(commands->Close(),"InitialClose");
    check(native.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
    std::uint64_t target=0;
    for(unsigned pass=0;pass<PassCount;++pass) for(unsigned index=0;index<HeapCount;++index) {
        std::array<unsigned char,TransferBytes> pattern{},prior{};expected(pattern,pass,index);
        if(pass) expected(prior,pass-1,index);
        std::memcpy(mappedUpload,pattern.data(),pattern.size());
        ID3D12Pageable* resident[]={heaps[index].Get()};
        check(native.device->MakeResident(1,resident),"MakeResident");
        if(resources[index]->GetGPUVirtualAddress()!=addresses[index]) fail("AddressChangedAfterResidency");
        check(allocator->Reset(),"AllocatorReset");check(commands->Reset(allocator.Get(),nullptr),"ListReset");
        if(pass) {
            transition(commands.Get(),resources[index].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            for(unsigned block=0;block<Blocks;++block) commands->CopyBufferRegion(readback.Get(),block*BlockBytes,resources[index].Get(),offsets[block],BlockBytes);
            transition(commands.Get(),resources[index].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        } else transition(commands.Get(),resources[index].Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        for(unsigned block=0;block<Blocks;++block) commands->CopyBufferRegion(resources[index].Get(),offsets[block],upload.Get(),block*BlockBytes,BlockBytes);
        transition(commands.Get(),resources[index].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        const auto destination=pass?TransferBytes:0;
        for(unsigned block=0;block<Blocks;++block) commands->CopyBufferRegion(readback.Get(),destination+block*BlockBytes,resources[index].Get(),offsets[block],BlockBytes);
        transition(commands.Get(),resources[index].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        check(commands->Close(),"SubmitClose");ID3D12CommandList* lists[]={commands.Get()};native.queue->ExecuteCommandLists(1,lists);
        check(native.queue->Signal(fence.Get(),++target),"GpuSignal");completed(fence.Get(),target);
        void* mappedReadback=nullptr;const D3D12_RANGE readRange{0,(pass?2u:1u)*TransferBytes};
        check(readback->Map(0,&readRange,&mappedReadback),"ReadbackMap");if(!mappedReadback) fail("ReadbackPointer");
        const auto pixels=static_cast<const unsigned char*>(mappedReadback);
        if(std::memcmp(pixels+destination,pattern.data(),TransferBytes) || (pass && std::memcmp(pixels,prior.data(),TransferBytes))) fail("GpuCopyOrEvictedContents");
        const auto actual=hash(pixels+destination,TransferBytes),previous=pass?hash(pixels,TransferBytes):0;
        readback->Unmap(0,&noRead);
        check(native.device->Evict(1,resident),"EvictAfterGpuRetirement");
        if(resources[index]->GetGPUVirtualAddress()!=addresses[index]) fail("AddressChangedAfterEviction");
        std::printf("D3D12_VA_PRESSURE_CYCLE pass=%u index=%u target=%llu gpuVa=%llu fnv1a=%016llx priorFnv1a=%016llx comparedBytes=%zu priorComparedBytes=%zu gpuRetired=true evictAccepted=true cpuFenceWrite=false\n",
            pass,index,static_cast<unsigned long long>(target),static_cast<unsigned long long>(addresses[index]),
            static_cast<unsigned long long>(actual),static_cast<unsigned long long>(previous),TransferBytes,pass?TransferBytes:0);std::fflush(stdout);
    }
    const D3D12_RANGE written{0,TransferBytes};upload->Unmap(0,&written);
    commands.Reset();allocator.Reset();fence.Reset();upload.Reset();readback.Reset();
    for(unsigned index=0;index<HeapCount;++index) { resources[index].Reset();heaps[index].Reset(); }
    std::printf("D3D12_VA_PRESSURE_COMPLETE allocations=%u cycles=%llu gpuVirtualBytes=%llu diagnosticUploadBytes=%llu diagnosticReadbackBytes=%llu nativeObjectsReleased=true ordinaryFramesStarted=false\n",
        HeapCount,static_cast<unsigned long long>(target),static_cast<unsigned long long>(HeapCount*HeapBytes),
        static_cast<unsigned long long>(HeapCount*PassCount*TransferBytes),
        static_cast<unsigned long long>(HeapCount*TransferBytes*(PassCount*(PassCount+1)/2)));std::fflush(stdout);
    return 0;
}
