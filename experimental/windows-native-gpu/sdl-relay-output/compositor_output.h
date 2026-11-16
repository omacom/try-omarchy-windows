// SPDX-License-Identifier: MIT
#pragma once
#include "sdl_gpu_output.h"

namespace driver_shared {
class CompositorOutput {
    TexturePresenter window;
    std::unique_ptr<driver_sdl::GpuOutput> sdl;
public:
    explicit CompositorOutput(std::shared_ptr<driver_sdl::Control> control) {
        if(control) sdl=std::make_unique<driver_sdl::GpuOutput>(std::move(control));
    }
    HRESULT initialize(IDXGIFactory4* factory,ID3D12Device* device,ID3D12CommandQueue* queue,
                       UINT width,UINT height,DXGI_FORMAT format,bool compositor,GpuUsageGuard budget,
                       std::shared_ptr<driver_input::Channel> input,bool diagnostic) {
        if(sdl) {
            // The existing isolated QMP diagnostic may run after frame one;
            // it supplies no HWND callbacks or physical input implementation.
            if(!compositor || static_cast<bool>(input)!=diagnostic) return E_INVALIDARG;
            return sdl->initialize(device,queue,width,height,format,std::move(budget));
        }
        return window.initialize(factory,device,queue,width,height,format,compositor,std::move(budget),std::move(input),diagnostic);
    }
    bool serviceMessages() { return sdl?sdl->serviceMessages():window.serviceMessages(); }
    HRESULT copy(ID3D12GraphicsCommandList* commands,ID3D12Resource* source,ID3D12Resource* readback,
                 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
        if(sdl) return readback?E_INVALIDARG:sdl->copy(commands,source);
        return window.copy(commands,source,readback,footprint);
    }
    HRESULT copyAuditTile(ID3D12GraphicsCommandList* commands,ID3D12Resource* readback,
                          D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint,UINT top,UINT rows) {
        return sdl?sdl->copyAuditTile(commands,readback,footprint,top,rows):window.copyAuditTile(commands,readback,footprint,top,rows);
    }
    HRESULT presentQueued(ID3D12CommandQueue* queue,std::uint64_t frame,bool audit) {
        return sdl?sdl->presentQueued(queue,frame,audit):window.presentQueued(queue,frame);
    }
    void drainQueued() { if(sdl) sdl->drainQueued();else window.drainQueued(); }
    void reportQueuedDisplay() { if(sdl) sdl->reportQueuedDisplay();else window.reportQueuedDisplay(); }
};
}
