// SPDX-License-Identifier: MIT
// Real compositor GPU copies into QEMU SDL; no HWND or input implementation.
#pragma once
#include <array>
#include <iostream>
#include <utility>
#include "control.h"
#include "../shared_texture_presenter.h"

namespace driver_sdl {
class GpuOutput {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    std::shared_ptr<Control> control;
    Ptr<ID3D12Device> device;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12Resource> texture;
    Ptr<ID3D12Fence> producer,consumer;
    std::array<HANDLE,3> exports{};
    HANDLE event=nullptr;
    std::uint64_t submitted=0;
    unsigned audits=0;
    bool opened=false,healthy=true,prepared=false;
    [[noreturn]] void fatalExit() const noexcept {
        if(control && control->beforeFatalExit) control->beforeFatalExit();
        ExitProcess(3);
    }
    void reportFailure(const char* phase,const char* message) const noexcept {
        // Record the device state before teardown can replace the first fault.
        // UINT64_MAX fence observations are retained verbatim, never retirement.
        try {
            const auto removal=device.Get()?device->GetDeviceRemovedReason():E_POINTER;
            std::cerr<<driver_qmp::Json{{"nativeSdlCompositorFailure",true},{"phase",phase},{"message",message},
                {"submitted",submitted},{"audits",audits},{"deviceRemovalHresult",static_cast<std::int64_t>(removal)},
                {"producerFenceObserved",producer.Get()?producer->GetCompletedValue():0},
                {"consumerFenceObserved",consumer.Get()?consumer->GetCompletedValue():0},
                {"outputAttached",opened},{"healthyBeforeFailure",healthy},{"performanceAccepted",false}}.dump()<<'\n';
        } catch(...) {
            std::fprintf(stderr,"SDL_FAILURE_RECEIPT_UNAVAILABLE phase=%s original=%s\n",phase,message);
        }
    }
    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER value{};value.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        value.Transition.pResource=resource;value.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        value.Transition.StateBefore=from;value.Transition.StateAfter=to;return value;
    }
    static void check(HRESULT hr,const char* message) {
        if(FAILED(hr)) throw std::runtime_error(message);
    }
    void closeExports() {
        for(auto& handle:exports) {
            if(handle && !CloseHandle(handle)) fatalExit();
            handle=nullptr;
        }
    }
    void waitConsumer() {
        if(!submitted) return;
        auto observed=consumer->GetCompletedValue();
        if(observed==UINT64_MAX) throw std::runtime_error("SDL consumer device lost");
        if(observed<submitted) {
            check(consumer->SetEventOnCompletion(submitted,event),"Cannot arm SDL consumer retirement");
            if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0) throw std::runtime_error("SDL consumer retirement timed out");
            observed=consumer->GetCompletedValue();
        }
        if(observed==UINT64_MAX || observed<submitted) throw std::runtime_error("SDL consumer did not retire");
    }
public:
    explicit GpuOutput(std::shared_ptr<Control> value):control(std::move(value)) {}
    ~GpuOutput() {
        drainQueued();closeExports();
        if(event && !CloseHandle(event)) fatalExit();
    }
    HRESULT initialize(ID3D12Device* selected,ID3D12CommandQueue* output,
                       UINT width,UINT height,DXGI_FORMAT format,driver_shared::GpuUsageGuard budget) noexcept {
        try {
            if(opened || !selected || !output || !control || !control->command || !control->beforeFatalExit ||
               !canonicalHex(control->session,32) || !canonicalHex(control->adapterLuid,16) ||
               width!=3840 || height!=2160 || format!=DXGI_FORMAT_B8G8R8A8_UNORM)
                return E_INVALIDARG;
            const auto luid=selected->GetAdapterLuid();
            if(luidText(&luid)!=control->adapterLuid) return E_INVALIDARG;
            device=selected;queue=output;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width=width;desc.Height=height;desc.DepthOrArraySize=desc.MipLevels=1;
            desc.Format=format;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            const auto allocation=device->GetResourceAllocationInfo(0,1,&desc);
            if(!allocation.SizeInBytes || allocation.SizeInBytes==UINT64_MAX ||
               (budget && !budget("sdl-shared-before",allocation.SizeInBytes))) return E_OUTOFMEMORY;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_SHARED,&desc,D3D12_RESOURCE_STATE_COMMON,
                  nullptr,IID_PPV_ARGS(&texture)),"Cannot allocate shared SDL GPU output");
            check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&producer)),"Cannot create SDL producer fence");
            check(device->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&consumer)),"Cannot create SDL consumer fence");
            if(budget && !budget("sdl-shared-after",0)) return E_OUTOFMEMORY;
            const auto prefix=L"Local\\TryOmarchy-SDL-"+std::wstring(control->session.begin(),control->session.end())+L"-0000000000000001";
            check(device->CreateSharedHandle(texture.Get(),nullptr,GENERIC_ALL,(prefix+L"-texture").c_str(),&exports[0]),"Cannot export SDL texture");
            check(device->CreateSharedHandle(producer.Get(),nullptr,GENERIC_ALL,(prefix+L"-producer").c_str(),&exports[1]),"Cannot export SDL producer fence");
            check(device->CreateSharedHandle(consumer.Get(),nullptr,GENERIC_ALL,(prefix+L"-consumer").c_str(),&exports[2]),"Cannot export SDL consumer fence");
            for(const auto handle:exports) {
                DWORD flags=0;
                if(!GetHandleInformation(handle,&flags) || (flags&HANDLE_FLAG_INHERIT))
                    throw std::runtime_error("SDL GPU export is inheritable or invalid");
            }
            event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!event) throw std::runtime_error("Cannot create SDL retirement event");
            const auto state=control->command("attach:1");
            // A successful attach command owns imported objects even when its
            // state reply is rejected. Keep them tracked for ordered teardown.
            opened=true;validateState(*control,state,0,0,true);
            closeExports();
            std::cerr<<"{\"nativeSdlCompositorOpened\":true,\"session\":\""<<control->session
                <<"\",\"adapterLuid\":\""<<control->adapterLuid<<"\",\"width\":3840,\"height\":2160,\"format\":\"BGRA8\""
                <<",\"exportHandlesClosed\":3,\"inputImplementation\":\"official-qemu-sdl\",\"physicalInputVerified\":false"
                <<",\"sharedTextureBytes\":"<<allocation.SizeInBytes<<",\"qemuState\":"<<state.dump()<<"}\n";
            return S_OK;
        } catch(const std::exception& error) {
            reportFailure("initialization",error.what());
            healthy=false;std::cerr<<"SDL compositor initialization failed: "<<error.what()<<'\n';return E_FAIL;
        }
    }
    bool serviceMessages() const { return opened && healthy; }
    HRESULT copy(ID3D12GraphicsCommandList* commands,ID3D12Resource* source) {
        if(!opened || !healthy || prepared || !commands || !source) return E_INVALIDARG;
        const auto desc=source->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width!=3840 || desc.Height!=2160 ||
           desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM || desc.DepthOrArraySize!=1 || desc.MipLevels!=1 ||
           desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality) return E_INVALIDARG;
        // Queue the reuse dependency on the GPU; ordinary frames never block
        // the CPU waiting for the previous cross-process consumer fence.
        if(submitted) {
            const auto hr=queue->Wait(consumer.Get(),submitted);if(FAILED(hr)) return hr;
        }
        auto barrier=transition(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        commands->ResourceBarrier(1,&barrier);commands->CopyResource(texture.Get(),source);
        barrier=transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
        commands->ResourceBarrier(1,&barrier);prepared=true;return S_OK;
    }
    HRESULT copyAuditTile(ID3D12GraphicsCommandList* commands,ID3D12Resource* readback,
                          D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint,UINT top,UINT rows) {
        if(!opened || !healthy || !prepared || !commands || !readback || !rows || top>=2160 || rows>2160-top ||
           footprint.Offset || footprint.Footprint.Width!=3840 || footprint.Footprint.Height!=rows ||
           footprint.Footprint.Depth!=1 || footprint.Footprint.Format!=DXGI_FORMAT_B8G8R8A8_UNORM ||
           footprint.Footprint.RowPitch<3840*4 || footprint.Footprint.RowPitch>16384 || footprint.Footprint.RowPitch%256 ||
           readback->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER ||
           readback->GetDesc().Width<std::uint64_t(rows)*footprint.Footprint.RowPitch) return E_INVALIDARG;
        auto barrier=transition(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        commands->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=texture.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        const D3D12_BOX region{0,top,0,3840,top+rows,1};commands->CopyTextureRegion(&to,0,0,0,&from,&region);
        barrier=transition(texture.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        commands->ResourceBarrier(1,&barrier);return S_OK;
    }
    HRESULT presentQueued(ID3D12CommandQueue* output,std::uint64_t frame,bool audit) noexcept {
        try {
            if(!opened || !healthy || !prepared || output!=queue.Get() || frame!=submitted+1 ||
               frame>=(1ull<<48) || (audit && audits==2)) return E_INVALIDARG;
            check(queue->Signal(producer.Get(),frame),"Cannot signal SDL producer completion");
            submitted=frame;prepared=false;
            const auto state=control->command(std::string(audit?"audit:1:":"present:1:")+std::to_string(frame));
            validateState(*control,state,frame,audits+(audit?1u:0u),true);
            if(audit) ++audits;
            std::cerr<<"{\"nativeSdlCompositorQueued\":true,\"frame\":"<<frame<<",\"audit\":"<<(audit?"true":"false")
                <<",\"producerFenceObserved\":"<<producer->GetCompletedValue()<<",\"consumerFenceObserved\":"<<consumer->GetCompletedValue()
                <<",\"gpuReuseWaitQueued\":"<<(frame>1?"true":"false")<<",\"cpuUpload\":false,\"ordinaryCpuFenceWait\":false"
                <<",\"qemuState\":"<<state.dump()<<"}\n";
            return S_OK;
        } catch(const std::exception& error) {
            reportFailure("submission",error.what());
            healthy=false;std::cerr<<"SDL compositor submission failed: "<<error.what()<<'\n';return E_FAIL;
        }
    }
    void drainQueued() noexcept {
        if(!opened) return;
        try {
            waitConsumer();const auto producerValue=producer->GetCompletedValue(),consumerValue=consumer->GetCompletedValue();
            if(producerValue==UINT64_MAX || producerValue<submitted) throw std::runtime_error("SDL producer did not retire");
            const auto state=control->command("detach:1");validateState(*control,state,submitted,audits,false);
            std::cerr<<"{\"nativeSdlCompositorDetached\":true,\"frames\":"<<submitted<<",\"audits\":"<<audits
                <<",\"producerFenceObserved\":"<<producerValue<<",\"consumerFenceObserved\":"<<consumerValue
                <<",\"qemuState\":"<<state.dump()<<",\"physicalScanoutVerified\":false}\n";
            opened=false;texture.Reset();producer.Reset();consumer.Reset();queue.Reset();device.Reset();
        } catch(const std::exception& error) {
            reportFailure("retirement",error.what());
            std::cerr<<"SDL compositor retirement failed: "<<error.what()<<'\n';fatalExit();
        }
    }
    void reportQueuedDisplay() const {
        std::cerr<<"{\"nativeSdlCompositorDisplayObservation\":true,\"submitted\":"<<submitted
            <<",\"physicalScanoutVerified\":false,\"allFramesDisplayedVerified\":false}\n";
    }
};
}
