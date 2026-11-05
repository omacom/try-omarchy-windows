// SPDX-License-Identifier: MIT
// Windows host experiment. This is not an NVIDIA Linux RM implementation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#include <array>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "wire.h"
#include "checked_fence_completion.h"
using Microsoft::WRL::ComPtr;
using namespace native_gpu;
constexpr UINT Width = 640, Height = 360;
static void check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        std::ostringstream s; s << operation << ": HRESULT 0x" << std::hex
                                << static_cast<unsigned long>(result);
        throw std::runtime_error(s.str());
    }
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default; Handle(const Handle&) = delete; Handle& operator=(const Handle&) = delete;
};
struct Window {
    HWND value = nullptr;
    static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b) {
        return DefWindowProcW(w, m, a, b);
    }
    Window() {
        WNDCLASSW cls{}; cls.lpfnWndProc = proc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"TryOmarchyNativeGpuLab";
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("RegisterClassW failed");
        RECT rect{0, 0, Width, Height}; AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        value = CreateWindowW(cls.lpszClassName,
            L"Try Omarchy native NVIDIA GPU experiment (host only)", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, cls.hInstance, nullptr);
        if (!value) throw std::runtime_error("CreateWindowW failed");
    }
    ~Window() { if (IsWindow(value)) DestroyWindow(value); }
    void show() { ShowWindow(value, SW_SHOWNOACTIVATE); }
    void pump() {
        MSG msg{}; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (!IsWindow(value)) throw std::runtime_error("Test window was closed");
    }
};
static D3D12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{}; p.Type = type; p.CreationNodeMask = 1; p.VisibleNodeMask = 1;
    return p;
}
static void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}
static std::string utf8(const wchar_t* text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Adapter name conversion failed");
    std::string s(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, s.data(), size, nullptr, nullptr);
    s.pop_back(); return s;
}
static std::string jsonString(const std::string& s) {
    std::ostringstream out; out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"'; return out.str();
}
// Read-only driver-level adapter check through documented WDDM KMT entry points.
// These calls say nothing about compatibility with Linux NV_ESC_* requests.
static std::string kmtFacts(LUID luid) {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return "unavailable: gdi32 not loaded";
    auto open = reinterpret_cast<PFND3DKMT_OPENADAPTERFROMLUID>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
    auto query = reinterpret_cast<PFND3DKMT_QUERYADAPTERINFO>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
    auto close = reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
    if (!open || !query || !close) return "unavailable: missing KMT entry points";
    D3DKMT_OPENADAPTERFROMLUID o{}; o.AdapterLuid = luid;
    NTSTATUS status = open(&o);
    if (status < 0) { std::ostringstream s; s << "open failed: NTSTATUS 0x" << std::hex << status; return s.str(); }
    D3DKMT_DRIVERVERSION version{};
    D3DKMT_QUERYADAPTERINFO q{}; q.hAdapter = o.hAdapter; q.Type = KMTQAITYPE_DRIVERVERSION;
    q.pPrivateDriverData = &version; q.PrivateDriverDataSize = sizeof version;
    status = query(&q);
    D3DKMT_CLOSEADAPTER c{}; c.hAdapter = o.hAdapter; const auto closeStatus = close(&c);
    std::ostringstream s; s << "open=0; query=0x" << std::hex << status
                            << "; close=0x" << closeStatus;
    if (status >= 0) s << "; WDDM enum=" << std::dec << static_cast<unsigned>(version);
    return s.str();
}
class NativeGpu : public Renderer {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device, consumer;
    ComPtr<ID3D12CommandQueue> queue, consumerQueue;
    ComPtr<ID3D12CommandAllocator> allocator, consumerAllocator;
    ComPtr<ID3D12GraphicsCommandList> commands, consumerCommands;
    ComPtr<ID3D12Fence> fence, importedFence;
    ComPtr<ID3D12Resource> image, importedImage, readback, backBuffer;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<IDXGISwapChain3> swap;
    Handle resourceHandle, fenceHandle, event;
    std::uint64_t fenceValue = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0; UINT64 rowSize = 0, totalSize = 0;
    Color lastColor{};
    DXGI_ADAPTER_DESC1 description{};
    std::string kmt;
    std::uint32_t presented = 0;
    bool ready = false, workSubmitted = false;
    [[noreturn]] static void retainAndExit(const char* reason) noexcept {
        std::fputs(reason, stderr);
        std::fflush(stderr);
        // Bypass C++/COM unwinding when retirement cannot be established.
        // This is process termination, not a GPU reset or verified recovery.
        ExitProcess(3);
    }
    void wait(std::uint64_t value) {
        const auto result = checkedFenceCompletion(value,
            [&] { return fence->GetCompletedValue(); },
            [&] { return SUCCEEDED(fence->SetEventOnCompletion(value, event.value)); },
            [&] { return WaitForSingleObject(event.value, 10000) == WAIT_OBJECT_0; });
        switch (result) {
        case FenceCompletion::Complete:
            workSubmitted = false;
            return;
        case FenceCompletion::DeviceRemoved: throw std::runtime_error("GPU device removed");
        case FenceCompletion::ArmFailed: throw std::runtime_error("Cannot arm GPU completion event");
        case FenceCompletion::WaitFailed: throw std::runtime_error("GPU fence wait failed or timed out");
        case FenceCompletion::Incomplete: throw std::runtime_error("GPU fence woke before target completed");
        }
        throw std::runtime_error("Invalid GPU completion result");
    }
    std::uint64_t beginSubmission() {
        if (fenceValue >= UINT64_MAX - 1) throw std::runtime_error("GPU fence value exhausted");
        workSubmitted = true;
        return ++fenceValue;
    }
public:
    NativeGpu() {
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            auto hr = factory->EnumAdapters1(index, &candidate);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            check(hr, "EnumAdapters1");
            DXGI_ADAPTER_DESC1 desc{}; check(candidate->GetDesc1(&desc), "GetDesc1");
            if (desc.VendorId != 0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            ComPtr<ID3D12Device> found;
            hr = D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&found));
            check(hr, "D3D12CreateDevice on NVIDIA adapter");
            adapter = candidate; device = found; description = desc; break;
        }
        if (!device) throw std::runtime_error("No hardware NVIDIA D3D12 adapter; software fallback is refused");
        kmt = kmtFacts(description.AdapterLuid);
    }
    ~NativeGpu() override {
        if (!workSubmitted) return;
        try { finish(); }
        catch (...) { retainAndExit("FAIL: unretired diagnostic GPU work; process teardown retains backing\n"); }
    }
    Capabilities capabilities() const override { return {1, ready ? 3u : 0u, description.VendorId, description.DeviceId}; }
    void initializeWindow(HWND window) {
        D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "CreateCommandQueue");
        DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width = Width; sd.Height = Height;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> base;
        check(factory->CreateSwapChainForHwnd(queue.Get(), window, &sd, nullptr, nullptr, &base), "CreateSwapChainForHwnd");
        check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
        check(base.As(&swap), "IDXGISwapChain3");
        check(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)), "CreateCommandList");
        check(commands->Close(), "Close initial command list");
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = Width; rd.Height = Height; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = sd.Format; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        auto properties = heapProperties(D3D12_HEAP_TYPE_DEFAULT);
        check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_SHARED, &rd,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&image)), "Create shared texture");
        check(device->CreateSharedHandle(image.Get(), nullptr, GENERIC_ALL, nullptr, &resourceHandle.value), "CreateSharedHandle(texture)");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)), "Create shared fence");
        check(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle.value), "CreateSharedHandle(fence)");
        event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event.value) throw std::runtime_error("CreateEventW failed");
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1;
        check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)), "Create RTV heap");
        device->CreateRenderTargetView(image.Get(), nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
        // A second device consumes the same resource and fence. This is host
        // interop only: it must not be reported as a guest BAR/dma-buf mapping.
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&consumer)), "Create consumer device");
        check(consumer->OpenSharedHandle(resourceHandle.value, IID_PPV_ARGS(&importedImage)), "OpenSharedHandle(texture)");
        check(consumer->OpenSharedHandle(fenceHandle.value, IID_PPV_ARGS(&importedFence)), "OpenSharedHandle(fence)");
        check(consumer->CreateCommandQueue(&q, IID_PPV_ARGS(&consumerQueue)), "Create consumer queue");
        check(consumer->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&consumerAllocator)), "Create consumer allocator");
        check(consumer->CreateCommandList(0, q.Type, consumerAllocator.Get(), nullptr, IID_PPV_ARGS(&consumerCommands)), "Create consumer commands");
        check(consumerCommands->Close(), "Close consumer commands");
        consumer->GetCopyableFootprints(&rd, 0, 1, 0, &footprint, &rows, &rowSize, &totalSize);
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = totalSize; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto readHeap = heapProperties(D3D12_HEAP_TYPE_READBACK);
        check(consumer->CreateCommittedResource(&readHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "Create readback buffer");
        ready = true;
    }
    bool clearPresent(Color color) override {
        if (!ready) return false;
        wait(fenceValue);
        check(allocator->Reset(), "Reset allocator"); check(commands->Reset(allocator.Get(), nullptr), "Reset commands");
        // Keep the submitted backbuffer in the owner, including exception paths.
        check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)), "GetBuffer");
        transition(commands.Get(), image.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float rgba[4]; for (UINT i = 0; i < 4; ++i) rgba[i] = color.rgba[i] / 255.0f;
        commands->ClearRenderTargetView(rtvHeap->GetCPUDescriptorHandleForHeapStart(), rgba, 0, nullptr);
        transition(commands.Get(), image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(commands.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(backBuffer.Get(), image.Get());
        transition(commands.Get(), backBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        transition(commands.Get(), image.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        check(commands->Close(), "Close commands"); ID3D12CommandList* lists[]{commands.Get()};
        const auto target = beginSubmission();
        queue->ExecuteCommandLists(1, lists);
        if (FAILED(queue->Signal(fence.Get(), target)))
            retainAndExit("FAIL: cannot signal submitted producer work; process teardown retains backing\n");
        lastColor = color;
        auto hr = swap->Present(1, 0); check(hr, "Present");
        if (hr == S_OK) ++presented;
        return hr == S_OK; // occlusion must not count as visible presentation
    }
    void verifySharedPixels() {
        if (!ready || !presented) throw std::runtime_error("No frame to verify");
        check(consumerQueue->Wait(importedFence.Get(), fenceValue), "Wait imported fence");
        check(consumerAllocator->Reset(), "Reset consumer allocator");
        check(consumerCommands->Reset(consumerAllocator.Get(), nullptr), "Reset consumer commands");
        transition(consumerCommands.Get(), importedImage.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = importedImage.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        consumerCommands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        transition(consumerCommands.Get(), importedImage.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        check(consumerCommands->Close(), "Close consumer commands");
        ID3D12CommandList* lists[]{consumerCommands.Get()};
        const auto target = beginSubmission();
        consumerQueue->ExecuteCommandLists(1, lists);
        if (FAILED(consumerQueue->Signal(importedFence.Get(), target)))
            retainAndExit("FAIL: cannot signal submitted consumer work; process teardown retains backing\n");
        wait(fenceValue);
        const D3D12_RANGE range{0, static_cast<SIZE_T>(totalSize)}; std::uint8_t* pixels = nullptr;
        check(readback->Map(0, &range, reinterpret_cast<void**>(&pixels)), "Map readback");
        bool correct = true;
        for (UINT y = 0; y < Height && correct; ++y)
            for (UINT x = 0; x < Width && correct; ++x)
                for (UINT c = 0; c < 4; ++c) {
                    int actual = pixels[footprint.Offset + y * footprint.Footprint.RowPitch + x * 4 + c];
                    int expected = lastColor.rgba[c];
                    if (actual < expected - 1 || actual > expected + 1) { correct = false; break; }
                }
        const D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
        if (!correct) throw std::runtime_error("Shared texture pixel verification failed");
    }
    std::string report(bool verified) const {
        std::ostringstream s;
        s << "{\n  \"schema\": 1,\n  \"adapter\": " << jsonString(utf8(description.Description))
          << ",\n  \"vendorId\": " << description.VendorId << ",\n  \"deviceId\": " << description.DeviceId
          << ",\n  \"softwareFallback\": false,\n  \"kmt\": " << jsonString(kmt)
          << ",\n  \"d3d12Device\": true,\n  \"presentedFrames\": " << presented
          << ",\n  \"sharedTextureReadbackVerified\": " << (verified ? "true" : "false")
          << ",\n  \"linuxNvidiaAbiCompatible\": false,\n  \"virtioTransportImplemented\": false,"
          << "\n  \"guestDesktopAcceleratedByThisBackend\": false\n}\n";
        return s.str();
    }
    void finish() { if (fence) wait(fenceValue); }
};
static void expectSuccess(const std::vector<std::uint8_t>& reply) {
    if (reply.size() < sizeof(Header)) throw std::runtime_error("Missing dispatch reply");
    Header h{}; std::memcpy(&h, reply.data(), sizeof h);
    if (h.status) throw std::runtime_error("Native operation failed: " + std::to_string(h.status));
}
int main(int argc, char** argv) {
    std::string reportPath; bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--self-test") selfTest = true;
        else if (arg == "--probe") selfTest = false;
        else if (arg == "--report" && i + 1 < argc) reportPath = argv[++i];
        else { std::cerr << "Usage: native-gpu.exe --probe|--self-test [--report path.json]\n"; return 2; }
    }
    try {
        // GPU objects are destroyed before the window.
        Window window; NativeGpu gpu;
        if (selfTest) {
            gpu.initializeWindow(window.value); window.show();
            Session session(gpu);
            expectSuccess(session.dispatch(request(Operation::Capabilities, ProtocolVersion)));
            const std::array<Color, 3> colors{{{{32, 128, 224, 255}}, {{224, 32, 128, 255}}, {{128, 224, 32, 255}}}};
            for (const auto color : colors) {
                for (int frame = 0; frame < 30; ++frame) {
                    window.pump(); expectSuccess(session.dispatch(request(Operation::ClearPresent, color)));
                }
                gpu.verifySharedPixels();
            }
            gpu.finish();
        }
        auto report = gpu.report(selfTest);
        if (!reportPath.empty()) {
            std::ofstream file(reportPath, std::ios::binary); file << report;
            if (!file) throw std::runtime_error("Cannot write report");
        }
        std::cout << report; return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
