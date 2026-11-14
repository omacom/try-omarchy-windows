// SPDX-License-Identifier: MIT
// Optional real Linux D3D12 sharing/queue diagnostic, never a desktop claim.
#pragma once
#include <directx/d3d12.h>
#include <wsl/wrladapter.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdint>

namespace guest_gpu_timeline {
using Microsoft::WRL::ComPtr;
inline bool api(const char* stage, HRESULT status) {
    std::printf("GPU_TIMELINE_API stage=%s status=%08x\n", stage, static_cast<unsigned>(status));
    return status == S_OK;
}
inline bool completed(ID3D12Device* device, ID3D12Fence* fence, std::uint64_t target) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        const auto value = fence->GetCompletedValue();
        if (value == UINT64_MAX) { api("DeviceRemoved", device->GetDeviceRemovedReason()); return false; }
        if (value >= target) {
            std::printf("GPU_TIMELINE_OBSERVED target=%llu observed=%llu\n",
                        static_cast<unsigned long long>(target), static_cast<unsigned long long>(value));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    std::printf("GPU_TIMELINE_TIMEOUT target=%llu\n", static_cast<unsigned long long>(target));
    return false;
}
inline int run(ID3D12Device* device, ID3D12CommandQueue* queue) {
    puts("GPU_TIMELINE_BEGIN nativeLinuxRuntime=true cpuFenceWrites=false desktop=false");
    ComPtr<ID3D12Fence> source, imported;
    if (!api("SharedFence", device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&source)))) return 5;
    HANDLE handle = nullptr;
    if (!api("Export", device->CreateSharedHandle(source.Get(), nullptr, GENERIC_ALL, nullptr, &handle))) return 5;
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle));
    const auto status = device->OpenSharedHandle(handle, IID_PPV_ARGS(&imported));
    const bool closed = fd >= 0 && close(fd) == 0;
    if (!api("Import", status) || !closed || !imported) return 5;
    source.Reset();
    puts("GPU_TIMELINE_SOURCE_RELEASED original=true exportedFd=true importedAliasLive=true");
    for (std::uint64_t target = 1; target <= 4; ++target) {
        if (!api("QueueSignal", queue->Signal(imported.Get(), target)) || !completed(device, imported.Get(), target)) return 5;
    }
    ComPtr<ID3D12CommandQueue> releasingQueue;
    D3D12_COMMAND_QUEUE_DESC desc{}; desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!api("ReleasingQueue", device->CreateCommandQueue(&desc, IID_PPV_ARGS(&releasingQueue)))) return 5;
    ComPtr<ID3D12Fence> acknowledgement;
    if (!api("AcknowledgementFence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&acknowledgement)))) return 5;
    if (!api("FutureQueueWait", queue->Wait(imported.Get(), 5))) return 5;
    if (!api("DependentSignal", queue->Signal(acknowledgement.Get(), 1))) return 5;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto beforeRelease = imported->GetCompletedValue(), gated = acknowledgement->GetCompletedValue();
    std::printf("GPU_TIMELINE_GATED source=%llu acknowledgement=%llu expectedSource=4 expectedAcknowledgement=0\n",
                static_cast<unsigned long long>(beforeRelease), static_cast<unsigned long long>(gated));
    if (beforeRelease != 4 || gated != 0) return 5;
    if (!api("ReleaseFromSecondQueue", releasingQueue->Signal(imported.Get(), 5)) ||
        !completed(device, imported.Get(), 5) || !completed(device, acknowledgement.Get(), 1)) return 5;
    puts("GPU_TIMELINE_ORDERING_VERIFIED futureWait=5 gatedBeforeRelease=true dependentSignalCompleted=true cpuFenceWrites=false");
    acknowledgement.Reset(); releasingQueue.Reset(); imported.Reset();
    puts("GPU_TIMELINE_COMPLETE gpuSignals=6 gpuQueueWaits=1 originalAndFdReleased=true cleanup=true desktop=false");
    return 0;
}
}
