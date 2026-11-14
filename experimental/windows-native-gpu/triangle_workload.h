// SPDX-License-Identifier: MIT
// Diagnostic shader drawing through the genuine Linux D3D12 runtime.
#pragma once
#include <directx/d3d12.h>
#include <wsl/wrladapter.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <dlfcn.h>
#include "triangle_shaders.h"
#include "diagnostic_profile.h"
#include "stream_profile.h"

namespace gpu_triangle {
using Microsoft::WRL::ComPtr;
constexpr unsigned Width = gpu_diagnostic::Width, Height = gpu_diagnostic::Height, Rounds = 2, Tolerance = 2;
constexpr std::size_t Bytes = Width * Height * 4;
inline std::uint64_t hash(const std::uint8_t* data, std::size_t bytes) {
    std::uint64_t result = UINT64_C(14695981039346656037);
    for (std::size_t i = 0; i < bytes; ++i) { result ^= data[i]; result *= UINT64_C(1099511628211); }
    return result;
}
inline bool succeeded(const char* stage, HRESULT hr) {
    std::printf("gpuTriangle%s=%08x\n", stage, static_cast<unsigned>(hr));
    return SUCCEEDED(hr);
}
// Compute barycentric interpolation at pixel centers in screen coordinates.
// The host independently uses integer edge equations and checks both rounds.
inline bool reference(unsigned x, unsigned y, unsigned round, std::uint8_t* rgba) {
    const double px = x + 0.5, py = y + 0.5;
    const double top = Height * 0.125, bottom = Height * 0.875;
    const double middle = Width * 0.5, left = Width * 0.125, right = Width * 0.875;
    const double b = (bottom - py) / (bottom - top);
    const double c = (px - left - b * (middle - left)) / (right - left);
    const double a = 1 - b - c;
    const bool inside = a > 0 && b > 0 && c > 0;
    rgba[0] = 0; rgba[1] = 0; rgba[2] = round == 1 ? 255 : 0; rgba[3] = 255;
    if (inside) {
        const double weights[]{a, b, c};
        for (unsigned vertex = 0; vertex < 3; ++vertex)
            rgba[(vertex + round - 1) % 3] = static_cast<std::uint8_t>(std::lround(weights[vertex] * 255));
    }
    return inside;
}

inline int run(ID3D12Device* device, bool shared = false, bool consume = false, bool nativeMetadata = false, bool streaming = false) {
    if (streaming && (!gpu_diagnostic::Streaming || !nativeMetadata || !consume || !shared)) return 5;
    const unsigned frames = streaming ? driver_bridge::GuestStreamProfile.frames : Rounds;
    const auto submittedFailure = [streaming]() {
        // The owned VM stops before the host releases native backing. A failed
        // Signal/wait cannot justify unwinding resources potentially in use.
        if (streaming) { std::fflush(nullptr); std::_Exit(5); }
        return 5;
    };
    using BeginStream = int (*)();
    const auto beginStream = streaming ? reinterpret_cast<BeginStream>(dlsym(RTLD_DEFAULT, "wddm_bridge_begin_streaming")) : nullptr;
    if (streaming && (!beginStream || beginStream())) return 5;
    using Consume = int (*)(std::uint64_t, std::uint64_t*);
    const auto consumeTexture = consume ? reinterpret_cast<Consume>(dlsym(RTLD_DEFAULT, streaming ? "wddm_bridge_consume_streaming_texture" : "wddm_bridge_consume_shared_texture")) : nullptr;
    if (consume && (!shared || !consumeTexture)) return 5;
    using Prepare = int (*)(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
    const auto prepareTexture = nativeMetadata ? reinterpret_cast<Prepare>(dlsym(RTLD_DEFAULT, "wddm_bridge_prepare_native_texture")) : nullptr;
    if (nativeMetadata && (!consume || !prepareTexture)) return 5;
    if (streaming) std::printf("GPU_STREAM_TEST_BEGIN frames=64 width=%u height=%u audits=2 pendingCommands=16 pendingSignals=16\n", Width, Height);
    if (nativeMetadata) std::printf("GPU_NATIVE_METADATA_TEST_BEGIN width=%u height=%u format=28 flags=1\n", Width, Height);
    if (consume) std::printf("GPU_SHARED_CONSUME_TEST_BEGIN rounds=2 state=COMMON\n");
    if (shared) std::printf("GPU_SHARED_RESOURCE_TEST_BEGIN width=%u height=%u format=R8G8B8A8_UNORM\n", Width, Height);
    std::printf("GPU_TRIANGLE_TEST_BEGIN width=%u height=%u format=R8G8B8A8_UNORM rounds=%u vertices=3 tolerance=%u\n", Width, Height, Rounds, Tolerance);
    std::printf("GPU_TRIANGLE_SHADERS vertexBytes=%zu pixelBytes=%zu vertexHash=%016llx pixelHash=%016llx\n",
        sizeof(TriangleVertexShader), sizeof(TrianglePixelShader),
        static_cast<unsigned long long>(hash(TriangleVertexShader, sizeof(TriangleVertexShader))),
        static_cast<unsigned long long>(hash(TrianglePixelShader, sizeof(TrianglePixelShader))));
    D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (!succeeded("DirectQueue", device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return 5;
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.ShaderRegister = 0; parameter.Constants.Num32BitValues = 1;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rootDesc.NumParameters = 1; rootDesc.pParameters = &parameter;
    ComPtr<ID3DBlob> serialized, errors;
    if (!succeeded("SerializeRootSignature", D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized, &errors))) return 5;
    ComPtr<ID3D12RootSignature> root;
    if (!succeeded("RootSignature", device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&root)))) return 5;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc{};
    pipelineDesc.pRootSignature = root.Get();
    pipelineDesc.VS = {TriangleVertexShader, sizeof(TriangleVertexShader)};
    pipelineDesc.PS = {TrianglePixelShader, sizeof(TrianglePixelShader)};
    auto& blend = pipelineDesc.BlendState.RenderTarget[0];
    blend.SrcBlend = D3D12_BLEND_ONE; blend.DestBlend = D3D12_BLEND_ZERO; blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE; blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipelineDesc.SampleMask = UINT32_MAX;
    pipelineDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pipelineDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipelineDesc.RasterizerState.DepthClipEnable = TRUE;
    pipelineDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pipelineDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pipelineDesc.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pipelineDesc.DepthStencilState.BackFace = pipelineDesc.DepthStencilState.FrontFace;
    pipelineDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDesc.NumRenderTargets = 1; pipelineDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pipelineDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    if (!succeeded("Pipeline", device->CreateGraphicsPipelineState(&pipelineDesc, IID_PPV_ARGS(&pipeline)))) return 5;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = Width; desc.Height = Height;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = 1; heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> target, readback;
    if (nativeMetadata && prepareTexture(Width, Height, static_cast<std::uint32_t>(desc.Format), static_cast<std::uint32_t>(desc.Flags))) return 5;
    if (!succeeded("RenderTarget", device->CreateCommittedResource(&heap, shared ? D3D12_HEAP_FLAG_SHARED : D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)))) return 5;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT rows = 0; UINT64 rowBytes = 0, requiredBytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 512, &footprint, &rows, &rowBytes, &requiredBytes);
    if (footprint.Offset != 512 || footprint.Footprint.Format != desc.Format || footprint.Footprint.Width != Width ||
        footprint.Footprint.Height != Height || footprint.Footprint.Depth != 1 || rows != Height || rowBytes != Width * 4 ||
        footprint.Footprint.RowPitch < rowBytes || footprint.Footprint.RowPitch > gpu_diagnostic::MaxRowPitch || footprint.Footprint.RowPitch % 256 ||
        requiredBytes < (Height - 1) * footprint.Footprint.RowPitch + rowBytes || requiredBytes > gpu_diagnostic::MaxReadbackBytes - footprint.Offset) return 5;
    const UINT64 totalBytes = footprint.Offset + requiredBytes;
    std::printf("GPU_TRIANGLE_FOOTPRINT width=%u height=%u offset=%llu rowPitch=%u rowBytes=%llu rows=%u totalBytes=%llu\n",
        Width, Height, static_cast<unsigned long long>(footprint.Offset), footprint.Footprint.RowPitch,
        static_cast<unsigned long long>(rowBytes), rows, static_cast<unsigned long long>(totalBytes));
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width = totalBytes; buffer.Height = 1;
    buffer.DepthOrArraySize = 1; buffer.MipLevels = 1; buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (!succeeded("Readback", device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 5;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{}; heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heapDesc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    if (!succeeded("RtvHeap", device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptors)))) return 5;
    const auto rtv = descriptors->GetCPUDescriptorHandleForHeapStart(); if (!rtv.ptr) return 5;
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    ComPtr<ID3D12CommandAllocator> allocator;
    if (!succeeded("Allocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))) return 5;
    ComPtr<ID3D12GraphicsCommandList> commands;
    if (!succeeded("CommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), pipeline.Get(), IID_PPV_ARGS(&commands)))) return 5;
    ComPtr<ID3D12Fence> fence;
    if (!succeeded("Fence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 5;
    ComPtr<ID3D12Fence> handoffFence;
    if (consume && !streaming && FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&handoffFence)))) return 5;
    const FLOAT backgrounds[2][4]{{0, 0, 1, 1}, {0, 0, 0, 1}};
    const D3D12_VIEWPORT viewport{0, 0, static_cast<FLOAT>(Width), static_cast<FLOAT>(Height), 0, 1};
    const D3D12_RECT scissor{0, 0, Width, Height};
    // Larger diagnostic profiles must not place two full images on the stack.
    std::vector<std::uint8_t> expectedStorage(Bytes), actualStorage(Bytes);
    auto* expected = expectedStorage.data(); auto* actual = actualStorage.data();
    for (unsigned round = 1; round <= frames; ++round) {
        const unsigned renderRound = streaming ? 1 + (round - 1) % 2 : round;
        const unsigned auditRound = streaming ? driver_bridge::streamAuditRound(round) : round;
        const bool audit = auditRound != 0; std::uint64_t observedHash = 0;
        if (round > 1 && (!succeeded("AllocatorReset", allocator->Reset()) ||
                         !succeeded("CommandListReset", commands->Reset(allocator.Get(), pipeline.Get())))) return 5;
        if (consume && round > 1) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = target.Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            commands->ResourceBarrier(1, &b);
        }
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetGraphicsRoot32BitConstant(0, renderRound - 1, 0);
        commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->ClearRenderTargetView(rtv, backgrounds[renderRound - 1], 0, nullptr);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        std::printf("GPU_TRIANGLE_DRAW round=%u vertices=3 startVertex=0 instances=1 colorRotation=%u\n", round, renderRound - 1);
        commands->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.Get(); barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (audit) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{}; source.pResource = target.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{}; destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; destination.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commands->ResourceBarrier(1, &barrier);
        }
        if (streaming) {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            commands->ResourceBarrier(1, &barrier);
        }
        if (!succeeded("Close", commands->Close())) return 5;
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
        if (!succeeded("Signal", queue->Signal(fence.Get(), round))) return submittedFailure();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10); std::uint64_t observed = 0;
        do {
            observed = fence->GetCompletedValue();
            if (observed == UINT64_MAX) { succeeded("DeviceRemoved", device->GetDeviceRemovedReason()); return submittedFailure(); }
            if (observed >= round) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (observed < round) { std::printf("GPU_TRIANGLE_TIMEOUT round=%u observed=%llu\n", round, static_cast<unsigned long long>(observed)); return submittedFailure(); }
        if (audit) {
        void* pointer = nullptr; const D3D12_RANGE range{static_cast<SIZE_T>(footprint.Offset), static_cast<SIZE_T>(totalBytes)};
        if (!succeeded("ReadbackMap", readback->Map(0, &range, &pointer)) || !pointer) return 5;
        unsigned trianglePixels = 0, verified = 0, maximumError = 0;
        bool reportedMismatch = false;
        for (unsigned y = 0; y < Height; ++y) {
            std::memcpy(actual + y * Width * 4, static_cast<const std::uint8_t*>(pointer) + footprint.Offset + y * footprint.Footprint.RowPitch, Width * 4);
            for (unsigned x = 0; x < Width; ++x) {
                const std::size_t offset = (y * Width + x) * 4;
                const bool inside = reference(x, y, auditRound, expected + offset); trianglePixels += inside;
                bool matches = true;
                for (unsigned channel = 0; channel < 4; ++channel) {
                    const unsigned error = static_cast<unsigned>(std::abs(static_cast<int>(actual[offset + channel]) - expected[offset + channel]));
                    maximumError = std::max(maximumError, error);
                    if (error > (inside && channel < 3 ? Tolerance : 0)) matches = false;
                }
                verified += matches;
                if (!matches && !reportedMismatch) {
                    reportedMismatch = true;
                    std::printf("GPU_TRIANGLE_MISMATCH round=%u x=%u y=%u inside=%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
                        auditRound, x, y, inside, actual[offset], actual[offset+1], actual[offset+2], actual[offset+3],
                        expected[offset], expected[offset+1], expected[offset+2], expected[offset+3]);
                }
            }
        }
        const D3D12_RANGE empty{}; readback->Unmap(0, &empty);
        std::printf("GPU_TRIANGLE_ROUND round=%u verifiedPixels=%u trianglePixels=%u backgroundPixels=%u maxChannelError=%u referenceHash=%016llx observedHash=%016llx fenceTarget=%u fenceObserved=%llu\n",
            auditRound, verified, trianglePixels, Width * Height - trianglePixels, maximumError,
            static_cast<unsigned long long>(hash(expected, Bytes)), static_cast<unsigned long long>(hash(actual, Bytes)),
            round, static_cast<unsigned long long>(observed));
        // Export actual GPU bytes, including failing results, for independent inspection.
        if (gpu_diagnostic::HighResolution) {
            // A 66 MB UART hex stream per frame can exceed the transport deadline.
            // The guest still verifies every pixel and the host independently captures RGBA.
            std::printf("GPU_TRIANGLE_COMPACT_CAPTURE round=%u bytes=%zu hostBinaryExport=true\n", auditRound, Bytes);
        } else {
        const char hex[] = "0123456789abcdef";
        for (unsigned y = 0; y < Height; ++y) {
            std::string row; row.reserve(Width * 8);
            for (unsigned i = 0; i < Width * 4; ++i) {
                const auto value = actual[y * Width * 4 + i]; row.push_back(hex[value >> 4]); row.push_back(hex[value & 15]);
            }
            std::printf("GPU_TRIANGLE_PIXEL_ROW round=%u y=%u rgba=%s\n", auditRound, y, row.c_str());
        }
        }
        if (verified != Width * Height) return 5;
        observedHash = hash(actual, Bytes);
        }
        if (consume) {
            std::uint64_t retired = observed;
            if (!streaming) {
            if (FAILED(allocator->Reset()) || FAILED(commands->Reset(allocator.Get(), nullptr))) return 5;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            commands->ResourceBarrier(1, &barrier);
            if (FAILED(commands->Close())) return 5;
            queue->ExecuteCommandLists(1, lists);
            if (FAILED(queue->Signal(handoffFence.Get(), round))) return 5;
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            retired = 0;
            do {
                retired = handoffFence->GetCompletedValue();
                if (retired == UINT64_MAX) return 5;
                if (retired >= round) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (std::chrono::steady_clock::now() < end);
            if (retired < round) return 5;
            }
            std::printf("GPU_SHARED_CONSUME_HANDOFF round=%u state=COMMON fenceTarget=%u fenceObserved=%llu\n",
                        round, round, static_cast<unsigned long long>(retired));
            std::uint64_t hostHash = 0;
            if (consumeTexture(round, &hostHash) || hostHash != observedHash) return 5;
            std::printf("GPU_SHARED_CONSUME_HOST round=%u hash=%016llx returnedState=COMMON\n",
                        round, static_cast<unsigned long long>(hostHash));
            if (streaming) std::printf("GPU_STREAM_FRAME frame=%u auditRound=%u renderRound=%u guestFenceObserved=%llu hostHash=%016llx cpuReadback=%s returnedState=COMMON\n",
                round, auditRound, renderRound, static_cast<unsigned long long>(retired), static_cast<unsigned long long>(hostHash), audit ? "true" : "false");
        }
    }
    if (streaming) {
        std::printf("GPU_STREAM_TEST_COMPLETE verified=true frames=64 width=%u height=%u audits=2 allFramesDisplayedVerified=false\n", Width, Height);
        return 0;
    }
    std::printf("GPU_TRIANGLE_TEST_COMPLETE verified=true width=%u height=%u rounds=%u\n", Width, Height, Rounds);
    if (shared) std::printf("GPU_SHARED_RESOURCE_TEST_COMPLETE verified=true width=%u height=%u rounds=2\n", Width, Height);
    if (consume) std::printf("GPU_SHARED_CONSUME_TEST_COMPLETE verified=true rounds=2\n");
    if (nativeMetadata) std::printf("GPU_NATIVE_METADATA_TEST_COMPLETE verified=true rounds=2\n");
    return 0;
}
} // namespace gpu_triangle
