// SPDX-License-Identifier: MIT
// Real NVIDIA NT-resource alias lifetime; no guest or desktop acceptance.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "shared_resource_handle.h"
using Microsoft::WRL::ComPtr;
static void check(HRESULT result,const char* stage) { if(FAILED(result)) throw std::runtime_error(stage); }
static void retire(ID3D12CommandQueue* queue,ID3D12Fence* fence,UINT64 value) {
    if(FAILED(queue->Signal(fence,value))) ExitProcess(3);
    const auto deadline=GetTickCount64()+5000;
    for(;;) {
        const auto observed=fence->GetCompletedValue();
        if(observed==UINT64_MAX) throw std::runtime_error("DeviceRemoved");
        if(observed>=value) return;
        if(GetTickCount64()>=deadline) ExitProcess(3);
        Sleep(1);
    }
}
static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=from;barrier.Transition.StateAfter=to;return barrier;
}
static void probe() {
    ComPtr<IDXGIFactory4> factory;check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");
    ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 description{};
    for(UINT n=0;;++n) {
        ComPtr<IDXGIAdapter1> candidate;const auto result=factory->EnumAdapters1(n,&candidate);
        if(result==DXGI_ERROR_NOT_FOUND) break;
        check(result,"Enumerate");check(candidate->GetDesc1(&description),"Description");
        if(description.VendorId==0x10de && !(description.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter=candidate;break; }
    }
    if(!adapter) throw std::runtime_error("NvidiaMissing");
    ComPtr<ID3D12Device> device;check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Device");
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;texture.Width=130;texture.Height=73;
    texture.DepthOrArraySize=texture.MipLevels=1;texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;
    texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ComPtr<ID3D12Resource> source;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&texture,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&source)),"Source");
    HANDLE handle=nullptr;check(device->CreateSharedHandle(source.Get(),nullptr,GENERIC_ALL,nullptr,&handle),"Export");
    driver_shared::ResourceHandle original,alias;
    if(original.duplicate(handle) || !CloseHandle(handle) || alias.duplicate(original.get()) || alias.get()==original.get()) throw std::runtime_error("Duplicate");
    DWORD flags=0;
    if(!GetHandleInformation(alias.get(),&flags) || (flags&HANDLE_FLAG_INHERIT)) throw std::runtime_error("Inheritance");
    driver_shared::ResourceHandle invalid;
    if(!invalid.duplicate(nullptr) || !invalid.duplicate(INVALID_HANDLE_VALUE) || invalid.get() || !alias.duplicate(original.get()))
        throw std::runtime_error("MalformedAliasAccepted");
    D3D12_DESCRIPTOR_HEAP_DESC descriptor{};descriptor.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;descriptor.NumDescriptors=1;
    ComPtr<ID3D12DescriptorHeap> rtv;check(device->CreateDescriptorHeap(&descriptor,IID_PPV_ARGS(&rtv)),"Descriptor");
    const auto slot=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(source.Get(),nullptr,slot);
    ComPtr<ID3D12CommandAllocator> allocator;check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"Allocator");
    ComPtr<ID3D12GraphicsCommandList> commands;check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"Commands");
    auto before=transition(source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);
    commands->ResourceBarrier(1,&before);const float green[]{0,1,0,1};commands->ClearRenderTargetView(slot,green,0,nullptr);
    auto after=transition(source.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);
    commands->ResourceBarrier(1,&after);check(commands->Close(),"CloseSource");
    ID3D12CommandList* lists[]{commands.Get()};queue->ExecuteCommandLists(1,lists);retire(queue.Get(),fence.Get(),1);
    // The command list/descriptor do not retain the source after reset.
    commands.Reset();allocator.Reset();rtv.Reset();source.Reset();
    if(!original.close()) throw std::runtime_error("OriginalClose");
    ComPtr<ID3D12Resource> recipient;check(device->OpenSharedHandle(alias.get(),IID_PPV_ARGS(&recipient)),"OpenAfterProducerRelease");
    const auto actual=recipient->GetDesc();
    if(actual.Width!=130 || actual.Height!=73 || actual.Format!=texture.Format || actual.Flags!=texture.Flags) throw std::runtime_error("Layout");
    if(!alias.close()) throw std::runtime_error("AliasClose");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowBytes=0,bytes=0;
    device->GetCopyableFootprints(&texture,0,1,0,&footprint,&rows,&rowBytes,&bytes);
    if(rows!=73 || rowBytes!=520 || footprint.Offset || bytes>1024*1024) throw std::runtime_error("Footprint");
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;
    buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type=D3D12_HEAP_TYPE_READBACK;ComPtr<ID3D12Resource> readback;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Readback");
    check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"RecipientAllocator");
    check(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"RecipientCommands");
    before=transition(recipient.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);commands->ResourceBarrier(1,&before);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=recipient.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
    commands->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    after=transition(recipient.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);commands->ResourceBarrier(1,&after);
    check(commands->Close(),"CloseRecipient");lists[0]=commands.Get();queue->ExecuteCommandLists(1,lists);retire(queue.Get(),fence.Get(),2);
    void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};check(readback->Map(0,&range,&mapped),"Map");
    bool pixels=true;
    for(UINT y=0;y<73;++y) for(UINT x=0;x<130;++x) {
        const auto p=static_cast<const unsigned char*>(mapped)+y*footprint.Footprint.RowPitch+x*4;
        pixels=pixels && p[0]==0 && p[1]==255 && p[2]==0 && p[3]==255;
    }
    const D3D12_RANGE empty{};readback->Unmap(0,&empty);if(!pixels) throw std::runtime_error("Pixels");
    std::printf("NATIVE_RESOURCE_ALIAS vendor=%u device=%u width=130 height=73 pixels=9490 originalResourceReleasedBeforeImport=true originalNtClosedBeforeImport=true aliasNtClosedBeforeCopy=true gpuClear=true gpuCopyRetired=true cpuUpload=false inheritable=false guest=false desktop=false\n",description.VendorId,description.DeviceId);
}
int main() {
    setvbuf(stdout,nullptr,_IONBF,0);
    try { probe();puts("NATIVE_RESOURCE_ALIAS_COMPLETE ownedReferencesReleased=true cleanup=true guest=false desktop=false");return 0; }
    catch(const std::exception& error) { std::printf("NATIVE_RESOURCE_ALIAS_FAILURE stage=%s\n",error.what());return 1; }
}
