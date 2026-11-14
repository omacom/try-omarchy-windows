// SPDX-License-Identifier: MIT
// Native Windows control experiment. It does not exercise the QEMU bridge.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "third_party/nlohmann-json/json.hpp"
#include "binary_capture.h"
#include "triangle_shaders.h"
using Microsoft::WRL::ComPtr;
using nlohmann::json;
namespace {
constexpr UINT Width = gpu_diagnostic::Width, Height = gpu_diagnostic::Height;
constexpr UINT Buffers = 2, Warmup = 60, MaxFrames = 3600;
constexpr wchar_t ClassName[] = L"7WdevNativeShaderStreamReference";
void check(HRESULT hr, const char* stage) {
    if (FAILED(hr)) throw std::runtime_error(std::string(stage) + ": HRESULT " + std::to_string(hr));
}
UINT64 qpc() {
    LARGE_INTEGER value{};
    if (!QueryPerformanceCounter(&value)) throw std::runtime_error("QueryPerformanceCounter");
    return static_cast<UINT64>(value.QuadPart);
}
class Stream {
    HWND window = nullptr;
    ATOM windowClass = 0;
    HANDLE ready = nullptr, retiredEvent = nullptr;
    bool cancelled = false;
    UINT64 submitted = 0;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12QueryHeap> queries;
    ComPtr<ID3D12Resource> timestamps, pixels;
    std::array<ComPtr<ID3D12Resource>, Buffers> buffers;
    std::array<ComPtr<ID3D12CommandAllocator>, Buffers> allocators;
    std::array<UINT64, Buffers> reusableAfter{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 pixelBytes = 0, timestampFrequency = 0, cpuFrequency = 0;
    UINT descriptorSize = 0;
    json frames = json::array();
    json visibility() const {
        RECT client{}; POINT center{};
        const bool valid = GetClientRect(window, &client) != FALSE;
        center.x = (client.left + client.right) / 2; center.y = (client.top + client.bottom) / 2;
        const bool converted = valid && ClientToScreen(window, &center);
        const auto surface = converted ? WindowFromPoint(center) : nullptr;
        return {{"windowVisible", IsWindowVisible(window) != FALSE}, {"windowMinimized", IsIconic(window) != FALSE},
            {"centerOwned", converted && surface && GetAncestor(surface, GA_ROOT) == window},
            {"centerSurfaceFound", surface != nullptr}, {"centerX", center.x}, {"centerY", center.y}};
    }
    json desktop() const {
        const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        wchar_t inputName[256]{}, threadName[256]{}, stationName[256]{}; DWORD needed = 0;
        const bool names = input && GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof inputName, &needed) &&
            GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, threadName, sizeof threadName, &needed);
        const bool station = GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, stationName, sizeof stationName, &needed) != FALSE;
        if (input && !CloseDesktop(input)) throw std::runtime_error("input desktop handle cleanup");
        return {{"inputDesktopReadable", input != nullptr}, {"desktopNamesVerified", names},
            {"threadDesktopMatchesInput", names && std::wstring(inputName) == threadName},
            {"interactiveWindowStation", station && std::wstring(stationName) == L"WinSta0"}};
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        if (message == WM_CLOSE) {
            auto* self = reinterpret_cast<Stream*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self) self->cancelled = true;
            return 0; // Drain owned GPU work before destroying the window.
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    void pump() {
        MSG message{};
        while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    bool wait(HANDLE event) {
        const auto deadline = GetTickCount64() + 5000;
        while (GetTickCount64() < deadline) {
            const auto result = MsgWaitForMultipleObjectsEx(1, &event, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) return true;
            if (result != WAIT_TIMEOUT && result != WAIT_OBJECT_0 + 1) return false;
            pump();
        }
        return false;
    }
    void retire(UINT64 value) {
        if (!value) return;
        auto observed = fence->GetCompletedValue();
        if (observed < value) {
            if (FAILED(fence->SetEventOnCompletion(value, retiredEvent)) || !wait(retiredEvent)) {
                std::cerr << "GPU retirement failed; resources retained until process exit\n" << std::flush;
                ExitProcess(3);
            }
            observed = fence->GetCompletedValue();
        }
        if (observed == UINT64_MAX || observed < value) ExitProcess(3);
    }
    ComPtr<ID3D12Resource> readback(UINT64 bytes) {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)), "readback");
        return result;
    }
    void transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        commands->ResourceBarrier(1, &barrier);
    }
    void draw(UINT frame, UINT rotation, bool capture) {
        pump();
        if (cancelled) throw std::runtime_error("user closed reference window");
        if (!wait(ready)) throw std::runtime_error("frame-latency wait timeout");
        if (cancelled) throw std::runtime_error("user closed reference window");
        const auto index = swapchain->GetCurrentBackBufferIndex();
        if (index >= Buffers) throw std::runtime_error("invalid backbuffer index");
        retire(reusableAfter[index]);
        check(allocators[index]->Reset(), "allocator reset");
        check(commands->Reset(allocators[index].Get(), pipeline.Get()), "command reset");
        transition(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto firstQuery = frame * 2;
        if (!capture) commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery);
        auto rtv = descriptors->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += SIZE_T(index) * descriptorSize;
        const FLOAT clear[]{0, 0, rotation ? 0.f : 1.f, 1};
        const D3D12_VIEWPORT viewport{0, 0, FLOAT(Width), FLOAT(Height), 0, 1};
        const D3D12_RECT scissor{0, 0, LONG(Width), LONG(Height)};
        commands->SetGraphicsRootSignature(root.Get()); commands->SetGraphicsRoot32BitConstant(0, rotation, 0);
        commands->RSSetViewports(1, &viewport); commands->RSSetScissorRects(1, &scissor);
        commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        commands->ClearRenderTargetView(rtv, clear, 0, nullptr);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->DrawInstanced(3, 1, 0, 0);
        if (!capture) {
            commands->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery + 1);
            commands->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, firstQuery, 2,
                timestamps.Get(), UINT64(firstQuery) * sizeof(UINT64));
        } else {
            transition(buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = buffers[index].Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = pixels.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        transition(buffers[index].Get(), capture ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PRESENT);
        check(commands->Close(), "command close");
        ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
        // No throwing operations between execution and the retirement signal.
        ++submitted;
        if (FAILED(queue->Signal(fence.Get(), submitted))) ExitProcess(3);
        reusableAfter[index] = submitted;
        const auto before = qpc(); const auto presented = swapchain->Present(1, 0); const auto after = qpc();
        DXGI_FRAME_STATISTICS stats{}; const auto statistics = swapchain->GetFrameStatistics(&stats);
        UINT count = 0; const auto counted = swapchain->GetLastPresentCount(&count);
        frames.push_back({{"frame", frame}, {"audit", capture}, {"rotation", rotation}, {"backBuffer", index},
            {"presentHresult", presented}, {"presentCallQpcBefore", before}, {"presentCallQpcAfter", after},
            {"countHresult", counted}, {"presentCount", count}, {"statisticsHresult", statistics},
            {"statisticsPresentCount", stats.PresentCount}, {"syncQpc", stats.SyncQPCTime.QuadPart}});
        frames.back()["visibility"] = visibility();
        if (presented != S_OK || counted != S_OK) throw std::runtime_error("Present/count failed or window occluded");
        if (capture) {
            retire(submitted);
            void* mapped = nullptr; const D3D12_RANGE range{0, SIZE_T(pixelBytes)};
            check(pixels->Map(0, &range, &mapped), "pixel map");
            struct Unmap { ID3D12Resource* resource; ~Unmap() { const D3D12_RANGE empty{}; resource->Unmap(0, &empty); } } unmap{pixels.Get()};
            UINT64 hash = 0;
            check(driver_capture::write("NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW", rotation + 1,
                mapped, footprint.Offset, footprint.Footprint.RowPitch, hash), "pixel export");
            frames.back()["fnv1a"] = hash;
        }
    }
public:
    ~Stream() {
        if (submitted) retire(submitted);
        for (auto& buffer : buffers) buffer.Reset();
        commands.Reset(); swapchain.Reset();
        if (ready && !CloseHandle(ready)) ExitProcess(3);
        if (retiredEvent && !CloseHandle(retiredEvent)) ExitProcess(3);
        if (window && !DestroyWindow(window)) ExitProcess(3);
        if (windowClass && !UnregisterClassW(ClassName, GetModuleHandleW(nullptr))) ExitProcess(3);
        std::cerr << "{\"nativeStreamCleanup\":true,\"submitted\":" << submitted << ",\"gpuRetired\":true}\n";
    }
    void initialize(UINT count, bool topmost) {
        const auto captureDirectory = driver_capture::directory();
        for (const auto* name : {"native-backbuffer-1.rgba", "native-backbuffer-2.rgba"})
            if (std::filesystem::exists(captureDirectory / name)) throw std::runtime_error("capture already exists");
        LARGE_INTEGER frequency{};
        if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) throw std::runtime_error("CPU frequency");
        cpuFrequency = static_cast<UINT64>(frequency.QuadPart);
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        DXGI_ADAPTER_DESC1 desc{};
        for (UINT i = 0;; ++i) {
            const auto hr = factory->EnumAdapters1(i, &adapter);
            if (hr == DXGI_ERROR_NOT_FOUND) throw std::runtime_error("NVIDIA hardware adapter not found");
            check(hr, "adapter enumeration"); check(adapter->GetDesc1(&desc), "adapter description");
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) break;
            adapter.Reset();
        }
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "device");
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "queue");
        check(queue->GetTimestampFrequency(&timestampFrequency), "GPU timestamp frequency");
        if (!timestampFrequency) throw std::runtime_error("zero GPU timestamp frequency");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        retiredEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!retiredEvent) throw std::runtime_error("fence event");
        WNDCLASSEXW type{}; type.cbSize = sizeof type; type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = ClassName;
        windowClass = RegisterClassExW(&type); if (!windowClass) throw std::runtime_error("window class");
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        RECT bounds{0, 0, 780, 438};
        if (!AdjustWindowRectEx(&bounds, style, FALSE, WS_EX_NOACTIVATE)) throw std::runtime_error("window bounds");
        window = CreateWindowExW(WS_EX_NOACTIVATE, ClassName, L"Native Windows 4K shader reference (not QEMU)", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, type.hInstance, this);
        if (!window) throw std::runtime_error("window");
        DXGI_SWAP_CHAIN_DESC1 chain{}; chain.Width = Width; chain.Height = Height;
        chain.Format = DXGI_FORMAT_R8G8B8A8_UNORM; chain.SampleDesc.Count = 1;
        chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; chain.BufferCount = Buffers;
        chain.Scaling = DXGI_SCALING_STRETCH; chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        chain.AlphaMode = DXGI_ALPHA_MODE_IGNORE; chain.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> created;
        check(factory->CreateSwapChainForHwnd(queue.Get(), window, &chain, nullptr, nullptr, &created), "swapchain");
        check(created.As(&swapchain), "swapchain3");
        check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES), "window association");
        check(swapchain->SetMaximumFrameLatency(1), "frame latency");
        ready = swapchain->GetFrameLatencyWaitableObject(); if (!ready) throw std::runtime_error("frame wait handle");
        D3D12_DESCRIPTOR_HEAP_DESC heap{}; heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; heap.NumDescriptors = Buffers;
        check(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptors)), "RTV heap");
        descriptorSize = device->GetDescriptorHandleIncrementSize(heap.Type);
        for (UINT i = 0; i < Buffers; ++i) {
            check(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])), "backbuffer");
            auto rtv = descriptors->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += SIZE_T(i) * descriptorSize;
            device->CreateRenderTargetView(buffers[i].Get(), nullptr, rtv);
            check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])), "allocator");
        }
        D3D12_ROOT_PARAMETER parameter{}; parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.Num32BitValues = 1; parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{}; rootDesc.NumParameters = 1; rootDesc.pParameters = &parameter;
        ComPtr<ID3DBlob> blob, errors;
        check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "serialize root");
        check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{}; pso.pRootSignature = root.Get();
        pso.VS = {TriangleVertexShader, sizeof TriangleVertexShader}; pso.PS = {TrianglePixelShader, sizeof TrianglePixelShader};
        auto& blend = pso.BlendState.RenderTarget[0]; blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO; blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.SampleMask = UINT_MAX; pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pso.RasterizerState.DepthClipEnable = TRUE;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
        pso.DepthStencilState.BackFace = pso.DepthStencilState.FrontFace;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1; pso.RTVFormats[0] = chain.Format; pso.SampleDesc.Count = 1;
        check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)), "pipeline");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), pipeline.Get(), IID_PPV_ARGS(&commands)), "commands");
        check(commands->Close(), "initial command close");
        D3D12_QUERY_HEAP_DESC query{}; query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; query.Count = (count + Warmup) * 2;
        check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries)), "queries");
        timestamps = readback(UINT64(query.Count) * sizeof(UINT64));
        const auto target = buffers[0]->GetDesc(); UINT rows = 0; UINT64 rowBytes = 0, requiredBytes = 0;
        device->GetCopyableFootprints(&target, 0, 1, 512, &footprint, &rows, &rowBytes, &requiredBytes);
        if (footprint.Offset != 512 || rows != Height || rowBytes != Width * 4 ||
            footprint.Footprint.RowPitch < rowBytes || footprint.Footprint.RowPitch > gpu_diagnostic::MaxRowPitch ||
            footprint.Footprint.Depth != 1 || footprint.Footprint.Format != target.Format ||
            footprint.Footprint.Width != Width || footprint.Footprint.Height != Height ||
            footprint.Footprint.RowPitch % 256 || requiredBytes < UINT64(Height - 1) * footprint.Footprint.RowPitch + rowBytes ||
            requiredBytes > gpu_diagnostic::MaxReadbackBytes - footprint.Offset) throw std::runtime_error("capture footprint");
        pixelBytes = footprint.Offset + requiredBytes;
        pixels = readback(pixelBytes);
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); pump();
        if (topmost && !SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            throw std::runtime_error("reference window visibility");
        std::cerr << json({{"nativeStreamStarted", true}, {"guestBridge", false}, {"width", Width}, {"height", Height},
            {"frames", count}, {"warmup", Warmup}, {"topmost", topmost}, {"vendor", desc.VendorId}, {"device", desc.DeviceId},
            {"adapterLuidLow", desc.AdapterLuid.LowPart}, {"adapterLuidHigh", desc.AdapterLuid.HighPart},
            {"processId", GetCurrentProcessId()}, {"swapchain", reinterpret_cast<std::uintptr_t>(swapchain.Get())},
            {"qpcFrequency", cpuFrequency}, {"gpuTimestampFrequency", timestampFrequency},
            {"desktop", desktop()}, {"initialVisibility", visibility()}}).dump() << '\n';
    }
    void run(UINT count) {
        for (UINT i = 0; i < Warmup; ++i) draw(i, (i / 60) % 2, false);
        retire(submitted); const auto begin = qpc();
        for (UINT i = Warmup; i < Warmup + count; ++i) draw(i, (i / 60) % 2, false);
        retire(submitted); const auto end = qpc();
        std::vector<double> milliseconds; milliseconds.reserve(count);
        void* mapped = nullptr; const SIZE_T bytes = SIZE_T(count + Warmup) * 2 * sizeof(UINT64);
        const D3D12_RANGE range{0, bytes}; check(timestamps->Map(0, &range, &mapped), "timestamp map");
        {
            struct Unmap { ID3D12Resource* resource; ~Unmap() { const D3D12_RANGE empty{}; resource->Unmap(0, &empty); } } unmap{timestamps.Get()};
            const auto* ticks = static_cast<const UINT64*>(mapped);
            for (UINT i = 0; i < count + Warmup; ++i) {
                const auto start = ticks[i * 2], finish = ticks[i * 2 + 1];
                if (!start || finish <= start || (i && start < ticks[i * 2 - 1])) throw std::runtime_error("invalid GPU timestamps");
                const auto ms = double(finish - start) * 1000. / double(timestampFrequency);
                frames[i]["gpuStartTicks"] = start; frames[i]["gpuEndTicks"] = finish;
                if (i >= Warmup) milliseconds.push_back(ms);
            }
        }
        // Full-frame reads and file I/O are outside the timed loop. The independent
        // rgba-oracle must still validate these captures before accepting pixels.
        draw(Warmup + count, 0, true); draw(Warmup + count + 1, 1, true); retire(submitted);
        std::sort(milliseconds.begin(), milliseconds.end());
        std::cout << json({{"nativeStreamCompleted", true}, {"guestBridge", false}, {"width", Width}, {"height", Height},
            {"measuredFrames", count}, {"warmupFrames", Warmup}, {"submitted", submitted}, {"retired", fence->GetCompletedValue()},
            {"gpuTimestampFrequency", timestampFrequency}, {"qpcFrequency", cpuFrequency},
            {"wallStartQpc", begin}, {"wallEndQpc", end}, {"pacedWallSeconds", double(end - begin) / double(cpuFrequency)},
            {"gpuClearDrawMedianMs", milliseconds[count / 2]}, {"gpuClearDrawP95Ms", milliseconds[(count * 95 - 1) / 100]},
            {"pixelOracleVerified", false}, {"allFramesDisplayedVerified", false}, {"nativePerformanceRatioVerified", false},
            {"frames", frames}}).dump() << '\n';
    }
};
}
int main(int argc, char** argv) {
    UINT count = 0;
    if ((argc != 3 && argc != 4) || std::string(argv[1]) != "--frames" || (argc == 4 && std::string(argv[3]) != "--topmost")) {
        std::cerr << "Usage: native-stream-probe --frames 60..3600 [--topmost] (requires fresh WDDM_BRIDGE_CAPTURE_DIRECTORY)\n";
        return 2;
    }
    const std::string argument(argv[2]); const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || count < 60 || count > MaxFrames) return 2;
    try { Stream stream; stream.initialize(count, argc == 4); stream.run(count); return 0; }
    catch (const std::exception& error) { std::cerr << "Native stream failed: " << error.what() << '\n'; return 1; }
}
