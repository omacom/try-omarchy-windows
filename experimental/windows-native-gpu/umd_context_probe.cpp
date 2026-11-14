// SPDX-License-Identifier: MIT
// Local diagnostic: genuine NVIDIA Linux UMD, optionally through the QEMU bridge.
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <wsl/wrladapter.h>
#include <dxguids/dxguids.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <string>
#include <link.h>
#include "triangle_workload.h"
#ifdef WDDM_BRIDGE_TEST_ASYNC_CPU_WAITS
#include "async_cpu_wait_workload.h"
#endif
#ifdef WDDM_BRIDGE_TEST_GPU_TIMELINE
#include "guest_gpu_timeline.h"
#endif
using Microsoft::WRL::ComPtr;
static constexpr std::size_t CopyBytes = 65536;
static constexpr unsigned CopyRounds = 2;
static std::uint8_t pattern(std::size_t offset, unsigned round) {
    return static_cast<std::uint8_t>((offset * 37 + (offset >> 8) * 11 + round * 73) & 255);
}
static std::uint64_t hashBytes(const std::uint8_t* data, std::size_t bytes) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < bytes; ++i) { hash ^= data[i]; hash *= UINT64_C(1099511628211); }
    return hash;
}
static bool succeeded(const char* stage, HRESULT hr) {
    std::printf("gpuCopy%s=%08x\n", stage, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}
static int copyWorkload(ID3D12Device* device, ID3D12CommandQueue* queue) {
    std::printf("GPU_COPY_TEST_BEGIN bytes=%zu rounds=%u\n", CopyBytes, CopyRounds);
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = CopyBytes;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{}; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> upload, gpu, readback;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (!succeeded("Upload", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))) return 3;
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (!succeeded("Default", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpu)))) return 3;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (!succeeded("Readback", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 3;
    ComPtr<ID3D12CommandAllocator> allocator;
    if (!succeeded("Allocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&allocator)))) return 3;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (!succeeded("CommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return 3;
    ComPtr<ID3D12Fence> fence;
    if (!succeeded("Fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 3;
    std::uint8_t expected[CopyBytes];
    for (unsigned round = 1; round <= CopyRounds; ++round) {
        if (round > 1 && (!succeeded("AllocatorReset", allocator->Reset()) ||
                         !succeeded("CommandListReset", commands->Reset(allocator.Get(), nullptr)))) return 3;
        void* pointer = nullptr;
        const D3D12_RANGE empty{};
        if (!succeeded("UploadMap", upload->Map(0, &empty, &pointer))) return 3;
        if (!pointer) return 3;
        for (std::size_t i = 0; i < CopyBytes; ++i) expected[i] = pattern(i, round);
        std::memcpy(pointer, expected, CopyBytes);
        const D3D12_RANGE written{0, CopyBytes}; upload->Unmap(0, &written);
        commands->CopyBufferRegion(gpu.Get(), 0, upload.Get(), 0, CopyBytes);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = gpu.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        commands->CopyBufferRegion(readback.Get(), 0, gpu.Get(), 0, CopyBytes);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commands->ResourceBarrier(1, &barrier);
        if (!succeeded("Close", commands->Close())) return 3;
        ID3D12CommandList* lists[] = {commands.Get()}; queue->ExecuteCommandLists(1, lists);
        if (!succeeded("Signal", queue->Signal(fence.Get(), round))) return 3;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::uint64_t observed = 0;
        do {
            observed = fence->GetCompletedValue();
            if (observed == UINT64_MAX) { succeeded("DeviceRemoved", device->GetDeviceRemovedReason()); return 3; }
            if (observed >= round) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (observed < round) { std::printf("GPU_COPY_TIMEOUT round=%u observed=%llu\n", round, static_cast<unsigned long long>(observed)); return 3; }
        if (!succeeded("ReadbackMap", readback->Map(0, &written, &pointer))) return 3;
        if (!pointer) return 3;
        const bool matches = std::memcmp(pointer, expected, CopyBytes) == 0;
        const auto actualHash = hashBytes(static_cast<const std::uint8_t*>(pointer), CopyBytes);
        readback->Unmap(0, &empty);
        std::printf("GPU_COPY_ROUND round=%u verifiedBytes=%zu expectedHash=%016llx observedHash=%016llx fenceTarget=%u fenceObserved=%llu\n",
            round, matches ? CopyBytes : 0, static_cast<unsigned long long>(hashBytes(expected, CopyBytes)),
            static_cast<unsigned long long>(actualHash), round, static_cast<unsigned long long>(observed));
        if (!matches) return 3;
    }
    std::printf("GPU_COPY_TEST_COMPLETE verified=true bytes=%zu rounds=%u\n", CopyBytes, CopyRounds);
    return 0;
}
static constexpr unsigned ClearWidth = 130, ClearHeight = 73, ClearRounds = 2;
static constexpr std::size_t ClearBytes = ClearWidth * ClearHeight * 4;
static bool clearSucceeded(const char* stage, HRESULT hr) {
    std::printf("gpuClear%s=%08x\n", stage, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}
static int clearWorkload(ID3D12Device* device) {
    std::printf("GPU_CLEAR_TEST_BEGIN width=%u height=%u format=R8G8B8A8_UNORM rounds=%u\n", ClearWidth, ClearHeight, ClearRounds);
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!clearSucceeded("DirectQueue", device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return 4;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = ClearWidth; desc.Height = ClearHeight;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> target, readback;
    if (!clearSucceeded("RenderTarget", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)))) return 4;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows = 0; UINT64 rowBytes = 0, requiredBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 512, &footprint, &rows, &rowBytes, &requiredBytes);
    if (footprint.Offset != 512 || footprint.Footprint.Format != desc.Format || footprint.Footprint.Width != ClearWidth ||
        footprint.Footprint.Height != ClearHeight || footprint.Footprint.Depth != 1 || rows != ClearHeight || rowBytes != ClearWidth * 4 ||
        footprint.Footprint.RowPitch < rowBytes || footprint.Footprint.RowPitch > 4096 || footprint.Footprint.RowPitch % 256 ||
        requiredBytes < (ClearHeight - 1) * footprint.Footprint.RowPitch + rowBytes || requiredBytes > 1048576 - footprint.Offset) return 4;
    // GetCopyableFootprints reports the required span without BaseOffset.
    // The readback buffer and map range must also include its placement offset.
    const UINT64 totalBytes = footprint.Offset + requiredBytes;
    std::printf("GPU_CLEAR_FOOTPRINT width=%u height=%u offset=%llu rowPitch=%u rowBytes=%llu rows=%u totalBytes=%llu\n",
        ClearWidth, ClearHeight, static_cast<unsigned long long>(footprint.Offset), footprint.Footprint.RowPitch,
        static_cast<unsigned long long>(rowBytes), rows, static_cast<unsigned long long>(totalBytes));
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = totalBytes; buffer.Height = 1;
    buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (!clearSucceeded("Readback", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 4;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    if (!clearSucceeded("RtvHeap", device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors)))) return 4;
    const auto rtv = descriptors->GetCPUDescriptorHandleForHeapStart(); if (!rtv.ptr) return 4;
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    ComPtr<ID3D12CommandAllocator> allocator;
    if (!clearSucceeded("Allocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return 4;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (!clearSucceeded("CommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)))) return 4;
    ComPtr<ID3D12Fence> fence;
    if (!clearSucceeded("Fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 4;
    const D3D12_RECT rectangle{11, 7, 119, 65};
    const FLOAT background[2][4]{{1, 0, 0, 1}, {0, 0, 1, 1}};
    const FLOAT inner[2][4]{{0, 1, 1, 1}, {1, 1, 0, 1}};
    std::uint8_t expected[ClearBytes], actual[ClearBytes];
    for (unsigned round = 1; round <= ClearRounds; ++round) {
        if (round > 1 && (!clearSucceeded("AllocatorReset", allocator->Reset()) ||
                         !clearSucceeded("CommandListReset", commands->Reset(allocator.Get(), nullptr)))) return 4;
        commands->ClearRenderTargetView(rtv, background[round - 1], 0, nullptr);
        commands->ClearRenderTargetView(rtv, inner[round - 1], 1, &rectangle);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = target.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1, &barrier);
        if (!clearSucceeded("Close", commands->Close())) return 4;
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
        if (!clearSucceeded("Signal", queue->Signal(fence.Get(), round))) return 4;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); std::uint64_t observed = 0;
        do {
            observed = fence->GetCompletedValue();
            if (observed == UINT64_MAX) { clearSucceeded("DeviceRemoved", device->GetDeviceRemovedReason()); return 4; }
            if (observed >= round) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (observed < round) { std::printf("GPU_CLEAR_TIMEOUT round=%u observed=%llu\n", round, static_cast<unsigned long long>(observed)); return 4; }
        void* pointer = nullptr; const D3D12_RANGE range{static_cast<SIZE_T>(footprint.Offset), static_cast<SIZE_T>(totalBytes)};
        if (!clearSucceeded("ReadbackMap", readback->Map(0, &range, &pointer))) return 4;
        if (!pointer) return 4;
        for (unsigned y = 0; y < ClearHeight; ++y) {
            std::memcpy(actual + y * ClearWidth * 4, static_cast<const std::uint8_t*>(pointer) + footprint.Offset + y * footprint.Footprint.RowPitch, ClearWidth * 4);
            for (unsigned x = 0; x < ClearWidth; ++x) {
                const bool inside = x >= 11 && x < 119 && y >= 7 && y < 65;
                const auto* color = inside ? inner[round - 1] : background[round - 1];
                for (unsigned channel = 0; channel < 4; ++channel) expected[(y * ClearWidth + x) * 4 + channel] = color[channel] == 1 ? 255 : 0;
            }
        }
        const D3D12_RANGE empty{}; readback->Unmap(0, &empty);
        const bool matches = std::memcmp(expected, actual, ClearBytes) == 0;
        std::printf("GPU_CLEAR_ROUND round=%u verifiedPixels=%u expectedHash=%016llx observedHash=%016llx fenceTarget=%u fenceObserved=%llu\n",
            round, matches ? ClearWidth * ClearHeight : 0, static_cast<unsigned long long>(hashBytes(expected, ClearBytes)),
            static_cast<unsigned long long>(hashBytes(actual, ClearBytes)), round, static_cast<unsigned long long>(observed));
        if (!matches) return 4;
        if (round == ClearRounds) {
            const char hex[] = "0123456789abcdef";
            for (unsigned y = 0; y < ClearHeight; ++y) {
                std::string row; row.reserve(ClearWidth * 8);
                for (unsigned i = 0; i < ClearWidth * 4; ++i) {
                    const auto value = actual[y * ClearWidth * 4 + i]; row.push_back(hex[value >> 4]); row.push_back(hex[value & 15]);
                }
                std::printf("GPU_CLEAR_PIXEL_ROW round=%u y=%u rgba=%s\n", round, y, row.c_str());
            }
        }
    }
    std::printf("GPU_CLEAR_TEST_COMPLETE verified=true width=%u height=%u rounds=%u\n", ClearWidth, ClearHeight, ClearRounds);
    return 0;
}
static int moduleLoaded(struct dl_phdr_info* info, size_t bytes, void* data) {
    (void)bytes;
    if (info->dlpi_name && std::strstr(info->dlpi_name, "/libnvwgf2umx.so")) *static_cast<bool*>(data) = true;
    return 0;
}
int main() {
    ComPtr<IDXCoreAdapterFactory> factory;
    auto hr=DXCoreCreateAdapterFactory(IID_PPV_ARGS(&factory));
    std::printf("factory=%08x\n",static_cast<unsigned>(hr)); if (FAILED(hr)) return 1;
    ComPtr<IDXCoreAdapterList> list;
    hr=factory->CreateAdapterList(1,&DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS,IID_PPV_ARGS(&list));
    std::printf("list=%08x\n",static_cast<unsigned>(hr)); if (FAILED(hr)) return 1;
    for (UINT i=0;i<list->GetAdapterCount();++i) {
        ComPtr<IDXCoreAdapter> adapter; hr=list->GetAdapter(i,IID_PPV_ARGS(&adapter)); if(FAILED(hr)) return 1;
        DXCoreHardwareID id{}; hr=adapter->GetProperty(DXCoreAdapterProperty::HardwareID,sizeof id,&id); if(FAILED(hr)) return 1;
        if(id.vendorID != 0x10de) continue;
        std::printf("nvidia vendor=%u device=%u\n",id.vendorID,id.deviceID);
        ComPtr<ID3D12Device> device; hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device));
        bool loaded = false; dl_iterate_phdr(moduleLoaded, &loaded);
        std::printf("LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=%s\n", loaded ? "true" : "false");
        std::printf("device=%08x\n",static_cast<unsigned>(hr)); if(FAILED(hr)) return 1;
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> queue; hr=device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue));
        std::printf("copyQueue=%08x\n",static_cast<unsigned>(hr)); if (FAILED(hr)) return 1;
#ifdef WDDM_BRIDGE_TEST_ASYNC_CPU_WAITS
        return async_wait_test::run(device.Get(), queue.Get());
#endif
#ifdef WDDM_BRIDGE_TEST_GPU_TIMELINE
        if (const auto result = guest_gpu_timeline::run(device.Get(), queue.Get())) return result;
#endif
        const char* workload = std::getenv("WDDM_BRIDGE_RUNTIME_WORKLOAD");
        if (workload && !std::strcmp(workload, "native-stream")) { queue.Reset(); return gpu_triangle::run(device.Get(), true, true, true, true); }
        if (workload && !std::strcmp(workload, "native")) { queue.Reset(); return gpu_triangle::run(device.Get(), true, true, true); }
        if (workload && !std::strcmp(workload, "consume")) { queue.Reset(); return gpu_triangle::run(device.Get(), true, true); }
        if (workload && !std::strcmp(workload, "shared")) { queue.Reset(); return gpu_triangle::run(device.Get(), true); }
        if (workload && !std::strcmp(workload, "triangle")) { queue.Reset(); return gpu_triangle::run(device.Get()); }
        if (workload && !std::strcmp(workload, "clear")) { queue.Reset(); return clearWorkload(device.Get()); }
        return workload && !std::strcmp(workload, "copy") ? copyWorkload(device.Get(), queue.Get()) : 0;
    }
    return 2;
}
