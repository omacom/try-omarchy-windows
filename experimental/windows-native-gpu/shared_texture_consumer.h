// SPDX-License-Identifier: MIT
// Retained same-adapter GPU consumer. CPU readback is diagnostic verification.
#pragma once
#include "shared_texture_presenter.h"
#include "binary_capture.h"
#include "stream_profile.h"
namespace driver_shared {
class TextureConsumer {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12Resource> source, destination, readback;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 totalBytes = 0, sequence = 0;
    std::unique_ptr<TexturePresenter> presenter;
    Microsoft::WRL::ComPtr<ID3D12Resource> backBufferReadback;
    HRESULT exportPixels(ID3D12Resource* buffer, const char* prefix, std::uint64_t& hash, unsigned auditRound = 0) {
        void* mapped = nullptr; const D3D12_RANGE range{static_cast<SIZE_T>(footprint.Offset), static_cast<SIZE_T>(totalBytes)};
        const auto hr = buffer->Map(0, &range, &mapped); if (FAILED(hr)) return hr;
        struct ScopedUnmap {
            ID3D12Resource* resource;
            ~ScopedUnmap() { const D3D12_RANGE empty{}; resource->Unmap(0, &empty); }
        } unmap{buffer};
        if (!mapped) return E_FAIL;
        hash = UINT64_C(14695981039346656037);
        if (gpu_diagnostic::HighResolution) {
            return driver_capture::write(prefix, auditRound ? auditRound : static_cast<unsigned>(sequence), mapped,
                                         footprint.Offset, footprint.Footprint.RowPitch, hash);
        }
        const char hex[] = "0123456789abcdef";
        for (unsigned y = 0; y < 73; ++y) {
            const auto bytes = static_cast<const std::uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch;
            std::string row; row.reserve(1040);
            for (unsigned x = 0; x < 520; ++x) {
                hash = (hash ^ bytes[x]) * UINT64_C(1099511628211);
                row.push_back(hex[bytes[x] >> 4]); row.push_back(hex[bytes[x] & 15]);
            }
            std::cerr << prefix << " round=" << sequence << " y=" << y << " rgba=" << row << "\n";
        }
        return S_OK;
    }
public:
    HRESULT initialize(D3DKMT_HANDLE resource, LUID luid, bool present = false) {
        OBJECT_ATTRIBUTES attributes{}; attributes.Length = sizeof attributes;
        HANDLE handle = nullptr;
        const auto status = D3DKMTShareObjects(1, &resource, &attributes, GENERIC_ALL, &handle);
        if (status != 0 || !handle || handle == INVALID_HANDLE_VALUE) {
            if (handle && handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) ExitProcess(3);
            return E_FAIL;
        }
        return initializeOwnedHandle(handle,luid,present);
    }
    // Host-private ownership transfer, never a guest wire operation. The
    // caller supplies one duplicated NT reference owned by this process.
    HRESULT initializeOwnedHandle(HANDLE handle,LUID luid,bool present=false) {
        if (!handle || handle==INVALID_HANDLE_VALUE) return E_INVALIDARG;
        if (device || source) { if (!CloseHandle(handle)) ExitProcess(3); return E_INVALIDARG; }
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
        if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (SUCCEEDED(hr)) hr = device->OpenSharedHandle(handle, IID_PPV_ARGS(&source));
        const bool closed = CloseHandle(handle) != FALSE;
        std::cerr << "{\"nativeSharedTextureConsumerOpened\":true,\"ntstatus\":0"
                  << ",\"hresult\":" << hr << ",\"sharedNtHandleClosed\":" << (closed ? "true" : "false") << "}\n";
        if (!closed) ExitProcess(3);
        if (FAILED(hr)) return hr;
        auto desc = source->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width != gpu_diagnostic::Width || desc.Height != gpu_diagnostic::Height ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality || desc.Flags != D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
            return E_INVALIDARG;
        UINT rows = 0; UINT64 rowBytes = 0, required = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 512, &footprint, &rows, &rowBytes, &required);
        if (rows != gpu_diagnostic::Height || rowBytes != gpu_diagnostic::Width * 4 || footprint.Offset != 512 || footprint.Footprint.RowPitch < rowBytes ||
            footprint.Footprint.RowPitch > gpu_diagnostic::MaxRowPitch || footprint.Footprint.RowPitch % 256 ||
            required < std::uint64_t(gpu_diagnostic::Height - 1) * footprint.Footprint.RowPitch + rowBytes || required > gpu_diagnostic::MaxReadbackBytes - footprint.Offset)
            return E_INVALIDARG;
        totalBytes = footprint.Offset + required;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&destination));
        if (FAILED(hr)) return hr;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = totalBytes; buffer.Height = 1; buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return hr;
        if (present) {
            hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&backBufferReadback));
            if (FAILED(hr)) return hr;
        }
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)); if (FAILED(hr)) return hr;
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)); if (FAILED(hr)) return hr;
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands));
        if (FAILED(hr)) return hr;
        hr = commands->Close(); if (FAILED(hr)) return hr;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)); if (FAILED(hr)) return hr;
        if (present) {
            presenter = std::make_unique<TexturePresenter>();
            hr = presenter->initialize(factory.Get(), device.Get(), queue.Get());
        }
        return hr;
    }
    HRESULT consume(std::uint64_t& hash, bool streaming = false) {
        if (!fence || sequence >= (streaming ? driver_bridge::GuestStreamProfile.frames : 2) ||
            (streaming && !gpu_diagnostic::Streaming)) return E_INVALIDARG;
        const unsigned auditRound = streaming ? driver_bridge::streamAuditRound(sequence + 1) : static_cast<unsigned>(sequence + 1);
        const bool audit = auditRound != 0; hash = 0;
        auto hr = allocator->Reset(); if (FAILED(hr)) return hr;
        hr = commands->Reset(allocator.Get(), nullptr); if (FAILED(hr)) return hr;
        const auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = resource; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before; b.Transition.StateAfter = after; commands->ResourceBarrier(1, &b);
        };
        barrier(source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(destination.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(destination.Get(), source.Get());
        barrier(source.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        barrier(destination.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = destination.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        if (audit) commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (presenter) {
            hr = presenter->copy(commands.Get(), destination.Get(), audit ? backBufferReadback.Get() : nullptr, footprint);
            if (FAILED(hr)) return hr;
        }
        barrier(destination.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        hr = commands->Close(); if (FAILED(hr)) return hr;
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
        const auto target = ++sequence;
        hr = queue->Signal(fence.Get(), target);
        const auto deadline = GetTickCount64() + 5000;
        UINT64 observed = 0;
        while (SUCCEEDED(hr) && (observed = fence->GetCompletedValue()) < target && GetTickCount64() < deadline) Sleep(1);
        if (FAILED(hr) || observed == UINT64_MAX || observed < target) {
            // Submitted GPU work may retain every resource. Do not unwind it.
            std::cerr << "FAIL: Native shared texture copy did not retire; backing retained until process teardown\n" << std::flush;
            ExitProcess(3);
        }
        if (audit) { hr = exportPixels(readback.Get(), "NATIVE_SHARED_TEXTURE_PIXEL_ROW", hash, auditRound); if (FAILED(hr)) return hr; }
        if (streaming) std::cerr << "{\"guestStreamCopy\":true,\"frame\":" << target << ",\"auditRound\":" << auditRound
            << ",\"gpuFenceObserved\":" << observed << ",\"fnv1a\":" << hash
            << ",\"cpuReadback\":" << (audit ? "true" : "false") << ",\"sourceReturnedToCommon\":true,\"cpuUpload\":false}\n";
        std::cerr << "{\"nativeSharedTextureCopied\":true,\"round\":" << target << ",\"width\":" << gpu_diagnostic::Width << ",\"height\":" << gpu_diagnostic::Height
                  << ",\"gpuFenceTarget\":" << target << ",\"gpuFenceObserved\":" << observed
                  << ",\"fnv1a\":" << hash << ",\"sourceReturnedToCommon\":true,\"cpuUpload\":false,\"presented\":false}\n";
        if (presenter) {
            std::uint64_t backHash = 0;
            if (audit) {
                hr = exportPixels(backBufferReadback.Get(), "NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW", backHash, auditRound);
                if (FAILED(hr) || backHash != hash) return E_FAIL;
            }
            hr = streaming ? presenter->presentStreaming(queue.Get(), target) : presenter->present(queue.Get(), target, backHash);
            if (hr != S_OK) return E_FAIL;
        }
        return S_OK;
    }
};
}
