// SPDX-License-Identifier: MIT
// Bounded diagnostic presentation; not the Omarchy display integration.
#pragma once
#include <dwmapi.h>
#include <functional>
#include <memory>
#include "diagnostic_profile.h"
#include "stream_profile.h"
#include "bounded_input_channel.h"
namespace driver_shared {
// An admission observation, not a physical allocation quota. The owner checks
// process-reported local + nonlocal usage, including its KMT and D3D12 devices.
using GpuUsageGuard=std::function<bool(const char*,std::uint64_t)>;
class TexturePresenter {
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> buffers;
    Microsoft::WRL::ComPtr<ID3D12Fence> retirement;
    HWND window = nullptr;
    HANDLE frameLatency = nullptr;
    ATOM windowClass = 0;
    bool opened = false;
    bool compositorOutput = false, closeRequested = false, acceptedCompositor = false;
    UINT outputWidth = 0, outputHeight = 0;
    const wchar_t* registeredClass = nullptr;
    UINT index = 0;
    std::uint64_t preparedFrames = 0;
    std::uint64_t queuedPresentationFence = 0;
    std::shared_ptr<driver_input::Channel> windowInput;
    bool inputDiagnosticIsolated=false;
    unsigned heldButtons=0;
    int wheelVertical=0,wheelHorizontal=0;
    static constexpr const wchar_t* ClassName = L"7WdevGuestShaderPresentationLab";
    static constexpr const wchar_t* CompositorClassName = L"7WdevHyprlandPresentationLab";
    void resetInput(HWND hwnd) {
        heldButtons=0;wheelVertical=wheelHorizontal=0;
        if(windowInput) windowInput->post({driver_input::Kind::Reset});
        if(GetCapture()==hwnd) ReleaseCapture();
    }
    bool pointerPosition(HWND hwnd,LPARAM position) {
        RECT client{};driver_input::Event event{};
        return GetClientRect(hwnd,&client) && driver_input::absolute(static_cast<short>(LOWORD(position)),
            static_cast<short>(HIWORD(position)),client.right,client.bottom,event) && windowInput->post(event);
    }
    bool handleInput(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) {
        if(!windowInput || inputDiagnosticIsolated) return false;
        if(message==WM_KILLFOCUS || message==WM_CANCELMODE || message==WM_CLOSE) { resetInput(hwnd);return false; }
        if(message==WM_CAPTURECHANGED && heldButtons && reinterpret_cast<HWND>(lparam)!=hwnd) { resetInput(hwnd);return false; }
        if(message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYDOWN || message==WM_SYSKEYUP) {
            if(GetFocus()!=hwnd) return false;
            const bool down=message==WM_KEYDOWN || message==WM_SYSKEYDOWN;
            const unsigned scan=wparam==VK_PAUSE?0xffu:static_cast<unsigned>((lparam>>16)&0x7f)|((lparam&(1ll<<24))?0x80u:0u);
            if(driver_input::key(scan) && (!down || !(lparam&(1ll<<30)))) windowInput->post({driver_input::Kind::Key,scan,0,0,down});
            return true;
        }
        if(message==WM_CHAR || message==WM_SYSCHAR) return GetFocus()==hwnd;
        const bool down=message==WM_LBUTTONDOWN || message==WM_MBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_XBUTTONDOWN;
        const bool up=message==WM_LBUTTONUP || message==WM_MBUTTONUP || message==WM_RBUTTONUP || message==WM_XBUTTONUP;
        if(down || up) {
            if(down) SetFocus(hwnd);
            if(GetFocus()!=hwnd) return false;
            const unsigned code=(message==WM_LBUTTONDOWN || message==WM_LBUTTONUP)?0u:
                (message==WM_MBUTTONDOWN || message==WM_MBUTTONUP)?1u:
                (message==WM_RBUTTONDOWN || message==WM_RBUTTONUP)?2u:HIWORD(wparam)==XBUTTON1?3u:4u;
            if(down) { heldButtons|=1u<<code;SetCapture(hwnd); }
            pointerPosition(hwnd,lparam);
            windowInput->post({driver_input::Kind::Button,code,0,0,down});
            if(up) { heldButtons&=~(1u<<code);if(!heldButtons && GetCapture()==hwnd) ReleaseCapture(); }
            return true;
        }
        if(message==WM_MOUSEMOVE && GetFocus()==hwnd) { pointerPosition(hwnd,lparam);return true; }
        if((message==WM_MOUSEWHEEL || message==WM_MOUSEHWHEEL) && GetFocus()==hwnd) {
            auto& remainder=message==WM_MOUSEWHEEL?wheelVertical:wheelHorizontal;
            remainder+=static_cast<short>(HIWORD(wparam));
            while(remainder>=WHEEL_DELTA || remainder<=-WHEEL_DELTA) {
                const bool positive=remainder>0;
                const unsigned code=message==WM_MOUSEWHEEL?(positive?5u:6u):(positive?8u:7u);
                if(!windowInput->post({driver_input::Kind::Wheel,code})) break;
                remainder+=positive?-WHEEL_DELTA:WHEEL_DELTA;
            }
            return true;
        }
        return false;
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        const auto owner = reinterpret_cast<TexturePresenter*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if(owner && owner->handleInput(hwnd,message,wparam,lparam)) return message==WM_XBUTTONDOWN || message==WM_XBUTTONUP ? TRUE : 0;
        // Retire submitted work before destroying the compositor window.
        if (message == WM_CLOSE) {
            if (owner && owner->compositorOutput) owner->closeRequested = true;
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    void pump() {
        MSG message{};
        while (PeekMessageW(&message, compositorOutput ? nullptr : window, 0, 0, PM_REMOVE)) {
            if (compositorOutput && message.message == WM_QUIT) { closeRequested = true; continue; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    HRESULT waitForFrameSlot() {
        if (!frameLatency) return S_OK;
        const auto started = GetTickCount64(), deadline = started + 5000;
        for (;;) {
            pump(); if (closeRequested) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
            const auto now = GetTickCount64();
            if (now >= deadline) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            const auto result = MsgWaitForMultipleObjectsEx(1, &frameLatency, static_cast<DWORD>(deadline - now),
                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (result == WAIT_OBJECT_0) {
                DXGI_FRAME_STATISTICS stats{};
                const auto status = swapchain->GetFrameStatistics(&stats);
                std::cerr << "{\"nativeCompositorFrameSlotReady\":true,\"frame\":" << ++preparedFrames
                    << ",\"maximumFrameLatency\":1,\"waitMilliseconds\":" << GetTickCount64() - started
                    << ",\"statisticsHresult\":" << status << ",\"statisticsPresentCount\":" << stats.PresentCount
                    << ",\"syncQpc\":" << stats.SyncQPCTime.QuadPart
                    << ",\"presentProcessId\":" << GetCurrentProcessId()
                    << ",\"presentSwapChain\":" << reinterpret_cast<std::uintptr_t>(swapchain.Get()) << "}\n";
                return S_OK;
            }
            if (result == WAIT_OBJECT_0 + 1) continue;
            return HRESULT_FROM_WIN32(result == WAIT_TIMEOUT ? WAIT_TIMEOUT : result == WAIT_FAILED ? GetLastError() : ERROR_INVALID_DATA);
        }
    }
public:
    bool serviceMessages() { pump(); return !closeRequested && (!windowInput || windowInput->healthy()); }
    HRESULT presentQueued(ID3D12CommandQueue* queue, std::uint64_t frame) {
        if (!opened || !compositorOutput || !frame || frame == UINT64_MAX) return E_INVALIDARG;
        pump(); if (closeRequested) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        LARGE_INTEGER before{}, after{}, frequency{};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before)) return E_FAIL;
        const auto presented = swapchain->Present(1, 0);
        if (!QueryPerformanceCounter(&after)) ExitProcess(3);
        const auto signaled = queue->Signal(retirement.Get(), frame);
        if (FAILED(signaled)) ExitProcess(3);
        queuedPresentationFence = frame;
        UINT count = 0; const auto counted = swapchain->GetLastPresentCount(&count);
        std::cerr << "{\"nativeCompositorWindowSubmitted\":true,\"frame\":" << frame << ",\"presentHresult\":" << presented
            << ",\"countHresult\":" << counted << ",\"lastPresentCount\":" << count
            << ",\"presentProcessId\":" << GetCurrentProcessId()
            << ",\"presentSwapChain\":" << reinterpret_cast<std::uintptr_t>(swapchain.Get())
            << ",\"presentCallQpcBefore\":" << before.QuadPart << ",\"presentCallQpcAfter\":" << after.QuadPart
            << ",\"qpcFrequency\":" << frequency.QuadPart << ",\"cpuUpload\":false,\"displayVerified\":false}\n";
        return presented == S_OK && counted == S_OK && count == static_cast<UINT>(frame) ? S_OK : E_FAIL;
    }
    void drainQueued(std::uint64_t frame = 0) {
        if (!frame) frame = queuedPresentationFence;
        if (!frame) return;
        const auto deadline = GetTickCount64() + 5000;
        UINT64 observed = 0;
        while ((observed = retirement->GetCompletedValue()) < frame && GetTickCount64() < deadline) { pump(); Sleep(1); }
        if (observed == UINT64_MAX || observed < frame) ExitProcess(3);
    }
    void reportQueuedDisplay() {
        DXGI_FRAME_STATISTICS stats{}; const auto status = swapchain->GetFrameStatistics(&stats);
        std::cerr << "{\"nativeCompositorRingDisplayObservation\":true,\"statisticsHresult\":" << status
            << ",\"statisticsPresentCount\":" << stats.PresentCount << ",\"syncQpc\":" << stats.SyncQPCTime.QuadPart
            << ",\"presentProcessId\":" << GetCurrentProcessId()
            << ",\"presentSwapChain\":" << reinterpret_cast<std::uintptr_t>(swapchain.Get())
            << ",\"windowVisible\":" << (!closeRequested && IsWindowVisible(window) && !IsIconic(window) ? "true" : "false") << "}\n";
    }
    ~TexturePresenter() {
        if(windowInput && window) resetInput(window);
        // Only this bounded capture diagnostic holds its last output at EOF.
        // There is no per-frame hold in the compositor handoff.
        if (acceptedCompositor && !closeRequested) {
            const auto until = GetTickCount64() + 1000;
            do { pump(); Sleep(1); } while (!closeRequested && GetTickCount64() < until);
            DXGI_FRAME_STATISTICS stats{};
            const auto status = swapchain->GetFrameStatistics(&stats);
            std::cerr << "{\"nativeCompositorShutdownStatistics\":true,\"statisticsHresult\":" << status
                << ",\"statisticsPresentCount\":" << stats.PresentCount << ",\"syncQpc\":" << stats.SyncQPCTime.QuadPart
                << ",\"presentProcessId\":" << GetCurrentProcessId()
                << ",\"presentSwapChain\":" << reinterpret_cast<std::uintptr_t>(swapchain.Get())
                << ",\"windowVisible\":" << (!closeRequested && IsWindowVisible(window) && !IsIconic(window) ? "true" : "false")
                << ",\"diagnosticShutdownHoldMilliseconds\":1000}\n";
        }
        const bool latencyClosed = !frameLatency || CloseHandle(frameLatency) != FALSE;
        frameLatency = nullptr;
        for (auto& buffer : buffers) buffer.Reset();
        swapchain.Reset(); retirement.Reset();
        const bool destroyed = !window || DestroyWindow(window) != FALSE;
        const bool unregistered = !windowClass || UnregisterClassW(registeredClass, GetModuleHandleW(nullptr)) != FALSE;
        if (opened) std::cerr << "{\"nativeSharedTexturePresenterReleased\":true,\"gpuWorkRetired\":true,\"windowDestroyed\":"
            << (destroyed ? "true" : "false") << ",\"classUnregistered\":" << (unregistered ? "true" : "false")
            << (compositorOutput ? (latencyClosed ? ",\"frameLatencyHandleClosed\":true" : ",\"frameLatencyHandleClosed\":false") : "") << "}\n";
        if (!destroyed || !unregistered || !latencyClosed) ExitProcess(3);
    }
    HRESULT initialize(IDXGIFactory4* factory, ID3D12Device* device, ID3D12CommandQueue* queue,
                       UINT width = gpu_diagnostic::Width, UINT height = gpu_diagnostic::Height,
                       DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM, bool compositor = false,
                       const GpuUsageGuard& budget = {},std::shared_ptr<driver_input::Channel> input = {},bool isolateInputDiagnostic = false) {
        if (!factory || !device || !queue || windowClass || !width || !height || width > 4096 || height > 4096 ||
            (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM)) return E_INVALIDARG;
        if(budget) {
            D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture.Width=width;texture.Height=height;texture.DepthOrArraySize=texture.MipLevels=1;
            texture.Format=format;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            const auto allocation=device->GetResourceAllocationInfo(0,1,&texture);
            if(!allocation.SizeInBytes || allocation.SizeInBytes>UINT64_MAX/2 ||
                !budget("swapchain-before",allocation.SizeInBytes*2)) return E_OUTOFMEMORY;
        }
        outputWidth = width; outputHeight = height; compositorOutput = compositor;windowInput=std::move(input);
        inputDiagnosticIsolated=isolateInputDiagnostic;
        if(windowInput && !compositor) return E_INVALIDARG;
        registeredClass = compositor ? CompositorClassName : ClassName;
        WNDCLASSEXW type{}; type.cbSize = sizeof type; type.lpfnWndProc = procedure;
        type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = registeredClass;
        if(windowInput) type.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); // Standard arrow, including non-UNICODE callers.
        windowClass = RegisterClassExW(&type);
        if (!windowClass) return HRESULT_FROM_WIN32(GetLastError());
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        const DWORD extended = windowInput && !inputDiagnosticIsolated?0:WS_EX_NOACTIVATE;
        UINT clientWidth = compositor ? (std::min)(width, 1280u) : 780u;
        UINT clientHeight = compositor ? (std::max)(1u, height * clientWidth / width) : 438u;
        if (compositor && clientHeight > 720) { clientHeight = 720; clientWidth = (std::max)(1u, width * clientHeight / height); }
        RECT bounds{0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight)};
        if (!AdjustWindowRectEx(&bounds, style, FALSE, extended)) return HRESULT_FROM_WIN32(GetLastError());
        window = CreateWindowExW(extended, registeredClass,
            compositor ? L"Try Omarchy - NVIDIA Hyprland output (experimental)" : L"NVIDIA guest shader - QEMU Windows GPU bridge lab", style,
            CW_USEDEFAULT, CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, type.hInstance, this);
        if (!window) return HRESULT_FROM_WIN32(GetLastError());
        DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = width; desc.Height = height;
        desc.Format = format; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = compositorOutput ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (compositorOutput) desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
        auto hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &created);
        if (FAILED(hr)) return hr;
        hr = created.As(&swapchain); if (FAILED(hr)) return hr;
        if (compositorOutput) {
            hr = swapchain->SetMaximumFrameLatency(1); if (FAILED(hr)) return hr;
            frameLatency = swapchain->GetFrameLatencyWaitableObject();
            if (!frameLatency || frameLatency == INVALID_HANDLE_VALUE) { frameLatency = nullptr; return E_FAIL; }
        }
        hr = factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        if (FAILED(hr)) return hr;
        for (UINT i = 0; i < buffers.size(); ++i) {
            hr = swapchain->GetBuffer(i, IID_PPV_ARGS(&buffers[i])); if (FAILED(hr)) return hr;
        }
        if(budget && !budget("swapchain-after",0)) return E_OUTOFMEMORY;
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retirement)); if (FAILED(hr)) return hr;
        UINT previous = UINT_MAX;
        hr = swapchain->GetLastPresentCount(&previous); if (hr != S_OK || previous != 0) return E_FAIL;
        ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window); pump();
        RECT client{};
        if (!IsWindowVisible(window) || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return E_FAIL;
        opened = true;
        std::cerr << "{\"nativeSharedTexturePresenterOpened\":true,\"width\":" << outputWidth
            << ",\"height\":" << outputHeight << ",\"bufferCount\":2"
            << (compositorOutput ? ",\"compositorOutput\":true,\"flipDiscard\":true,\"frameLatencyWaitable\":true,\"maximumFrameLatency\":1" : "")
            << ",\"flipModel\":true,\"clientWidth\":" << client.right << ",\"clientHeight\":" << client.bottom
            << ",\"windowInputEnabled\":"<<(windowInput && !inputDiagnosticIsolated?"true":"false")
            << ",\"inputSelfTestIsolated\":"<<(inputDiagnosticIsolated?"true":"false")<<",\"windowHandle\":"<<reinterpret_cast<std::uintptr_t>(window)
            << ",\"windowVisible\":true,\"sameAdapter\":true,\"cpuUpload\":false}\n";
        return S_OK;
    }
    HRESULT copy(ID3D12GraphicsCommandList* commands, ID3D12Resource* source,
                 ID3D12Resource* readback, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
        if (!opened) return E_FAIL;
        pump(); if (closeRequested) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        const auto ready = waitForFrameSlot(); if (FAILED(ready)) return ready;
        index = swapchain->GetCurrentBackBufferIndex(); if (index >= buffers.size()) return E_FAIL;
        const auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = buffers[index].Get(); b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before; b.Transition.StateAfter = after; commands->ResourceBarrier(1, &b);
        };
        barrier(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        commands->CopyResource(buffers[index].Get(), source);
        if (!readback) {
            barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            return S_OK;
        }
        barrier(D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = buffers[index].Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        return S_OK;
    }
    // The queued compositor audits the current backbuffer before Present.
    HRESULT copyAuditTile(ID3D12GraphicsCommandList* commands,ID3D12Resource* readback,
                          D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint,UINT top,UINT rows) {
        if(!opened || !commands || !readback || index>=buffers.size() || !rows || top>=outputHeight || rows>outputHeight-top ||
            footprint.Offset || footprint.Footprint.Width!=outputWidth || footprint.Footprint.Height!=rows ||
            footprint.Footprint.Depth!=1 || footprint.Footprint.Format!=buffers[index]->GetDesc().Format ||
            footprint.Footprint.RowPitch<outputWidth*4 || footprint.Footprint.RowPitch>16384 || footprint.Footprint.RowPitch%256 ||
            readback->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER ||
            readback->GetDesc().Width<std::uint64_t(rows)*footprint.Footprint.RowPitch) return E_INVALIDARG;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=buffers[index].Get();barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        commands->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION from{};from.pResource=buffers[index].Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};to.pResource=readback;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        const D3D12_BOX region{0,top,0,outputWidth,top+rows,1};
        commands->CopyTextureRegion(&to,0,0,0,&from,&region);
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;
        commands->ResourceBarrier(1,&barrier);return S_OK;
    }
    // Stream acceptance is separate from the strict two-frame display proof.
    // A retired queue and Present(S_OK) alone do not prove scanout.
    HRESULT presentStreaming(ID3D12CommandQueue* queue, UINT64 frame) {
        if (!opened || !gpu_diagnostic::Streaming || !frame || frame > driver_bridge::GuestStreamProfile.frames) return E_INVALIDARG;
        pump(); LARGE_INTEGER before{}, after{}, frequency{};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before)) return E_FAIL;
        const auto presented = swapchain->Present(1, 0);
        if (!QueryPerformanceCounter(&after)) ExitProcess(3);
        auto hr = queue->Signal(retirement.Get(), frame);
        const auto deadline = GetTickCount64() + 5000; UINT64 observed = 0;
        while (SUCCEEDED(hr) && (observed = retirement->GetCompletedValue()) < frame && GetTickCount64() < deadline) { pump(); Sleep(1); }
        if (FAILED(hr) || observed == UINT64_MAX || observed < frame) ExitProcess(3);
        UINT count = 0; const auto counted = swapchain->GetLastPresentCount(&count);
        DXGI_FRAME_STATISTICS stats{}; const auto statistics = swapchain->GetFrameStatistics(&stats);
        std::cerr << "{\"guestStreamPresent\":true,\"frame\":" << frame << ",\"presentHresult\":" << presented
            << ",\"presentCountHresult\":" << counted << ",\"lastPresentCount\":" << count
            << ",\"presentQueueFenceObserved\":" << observed << ",\"presentCallQpcBefore\":" << before.QuadPart
            << ",\"presentCallQpcAfter\":" << after.QuadPart << ",\"qpcFrequency\":" << frequency.QuadPart
            << ",\"presentProcessId\":" << GetCurrentProcessId() << ",\"presentSwapChain\":" << reinterpret_cast<std::uintptr_t>(swapchain.Get())
            << ",\"statisticsHresult\":" << statistics << ",\"statisticsPresentCount\":" << stats.PresentCount
            << ",\"allFramesDisplayedVerified\":false,\"cpuUpload\":false}\n";
        return presented == S_OK && counted == S_OK && count == frame ? S_OK : E_FAIL;
    }
    HRESULT present(ID3D12CommandQueue* queue, UINT64 round, UINT64 hash, bool requireDisplayProof = true) {
        if (!opened || !round || round > 2) return E_INVALIDARG;
        pump();
        LARGE_INTEGER before{}, after{}, frequency{};
        if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&before)) return E_FAIL;
        const auto presented = swapchain->Present(1, 0);
        if (!QueryPerformanceCounter(&after)) ExitProcess(3); // Submitted work must not unwind.

        // Retire the queue even if Present reports an error or occlusion.
        auto hr = queue->Signal(retirement.Get(), round);
        const auto deadline = GetTickCount64() + 5000; UINT64 observed = 0;
        while (SUCCEEDED(hr) && (observed = retirement->GetCompletedValue()) < round && GetTickCount64() < deadline) { pump(); Sleep(1); }
        if (FAILED(hr) || observed == UINT64_MAX || observed < round) {
            std::cerr << "FAIL: Presentation queue did not retire; backing retained until process teardown\n" << std::flush;
            ExitProcess(3);
        }
        if (presented != S_OK) return E_FAIL;
        UINT count = 0; hr = swapchain->GetLastPresentCount(&count); if (hr != S_OK || count != round) return E_FAIL;
        HRESULT flushed=S_OK; unsigned flushes=0;
        DXGI_FRAME_STATISTICS stats{};
        const auto statsDeadline = GetTickCount64() + 2500;
        if (requireDisplayProof) do {
            // Hardware Flip Queue systems can require a DWM flush before
            // statistics update. Flush each observation, not just the first.
            pump(); flushed=DwmFlush(); ++flushes;
            if (FAILED(flushed)) break;
            hr = swapchain->GetFrameStatistics(&stats);
            if (hr == S_OK && stats.PresentCount == count && stats.SyncQPCTime.QuadPart > 0) break;
            if (FAILED(hr) && hr != DXGI_ERROR_FRAME_STATISTICS_DISJOINT) break;
            Sleep(10);
        } while (GetTickCount64() < statsDeadline);
        else hr = swapchain->GetFrameStatistics(&stats);
        const bool visible = IsWindowVisible(window) != FALSE && !IsIconic(window);
        const bool complete = hr == S_OK && SUCCEEDED(flushed) && stats.PresentCount == count && stats.SyncQPCTime.QuadPart > 0 && visible;
        std::cerr << "{\"nativeSharedTexturePresented\":" << (complete ? "true" : "false") << ",\"round\":" << round
            << ",\"width\":" << outputWidth << ",\"height\":" << outputHeight << ",\"backBufferIndex\":" << index << ",\"backBufferFnv1a\":" << hash
            << (compositorOutput ? ",\"compositorOutput\":true" : "")
            << (compositorOutput ? (requireDisplayProof ? ",\"presentSubmissionAccepted\":true,\"displayProofRequired\":true" :
                ",\"presentSubmissionAccepted\":true,\"displayProofRequired\":false") : "")
            << ",\"presentProcessId\":" << GetCurrentProcessId()
            << ",\"presentSwapChain\":" << reinterpret_cast<std::uintptr_t>(swapchain.Get())
            << ",\"presentCallQpcBefore\":" << before.QuadPart << ",\"presentCallQpcAfter\":" << after.QuadPart
            << ",\"qpcFrequency\":" << frequency.QuadPart
            << ",\"dxgiStatisticsComplete\":" << (complete ? "true" : "false")
            << ",\"presentationResult\":\"" << (complete ? "dxgi-statistics-verified" : "statistics-unavailable") << "\""
            << ",\"presentHresult\":" << presented << ",\"lastPresentCount\":" << count << ",\"statisticsHresult\":" << hr
            << ",\"statisticsPresentCount\":" << stats.PresentCount << ",\"syncQpc\":" << stats.SyncQPCTime.QuadPart
            << ",\"dwmFlushHresult\":" << flushed << ",\"windowVisible\":" << (visible ? "true" : "false")
            << ",\"dwmStatisticsFlushes\":" << flushes
            << ",\"presentQueueFenceTarget\":" << round << ",\"presentQueueFenceObserved\":" << observed
            << ",\"backBufferState\":\"PRESENT\",\"cpuUpload\":false}\n";
        // Leave the verified frame visible briefly, pumping its window messages.
        if (complete && requireDisplayProof) { const auto hold = GetTickCount64() + 3000; do { pump(); Sleep(10); } while (GetTickCount64() < hold); }
        // Statistics can be stale even when ETW records the frame as displayed.
        // Preserve strict diagnostic failure; an offline ETW verifier needs exact
        // PID/swapchain/QPC correlation, never Present(S_OK) or a retired GPU fence alone.
        if (compositorOutput && !requireDisplayProof) acceptedCompositor = true;
        return complete || !requireDisplayProof ? S_OK : E_FAIL;
    }
};
}
