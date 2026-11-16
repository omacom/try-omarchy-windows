// SPDX-License-Identifier: MIT
// Native API/lifetime acceptance for the guest's forthcoming shared-fence FD
// path. No rendering, desktop or guest sharing is established by this probe.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include "shared_sync_handle.h"
using Microsoft::WRL::ComPtr;

struct OwnedKmt {
    D3DKMT_HANDLE adapter = 0, sourceDevice = 0, recipientDevice = 0, source = 0, recipient = 0;
    bool cleanup() {
        bool good = true;
        for (auto* object : {&recipient, &source}) if (*object) {
            D3DKMT_DESTROYSYNCHRONIZATIONOBJECT a{}; a.hSyncObject = *object;
            const auto status = D3DKMTDestroySynchronizationObject(&a);
            std::printf("NATIVE_SYNC_CLEANUP kind=fence status=%08lx\n", static_cast<unsigned long>(status));
            good &= status == 0; if (!status) *object = 0;
        }
        for (auto* object : {&recipientDevice, &sourceDevice}) if (*object) {
            D3DKMT_DESTROYDEVICE a{}; a.hDevice = *object;
            const auto status = D3DKMTDestroyDevice(&a);
            std::printf("NATIVE_SYNC_CLEANUP kind=device status=%08lx\n", static_cast<unsigned long>(status));
            good &= status == 0; if (!status) *object = 0;
        }
        if (adapter) {
            D3DKMT_CLOSEADAPTER a{}; a.hAdapter = adapter;
            const auto status = D3DKMTCloseAdapter(&a);
            std::printf("NATIVE_SYNC_CLEANUP kind=adapter status=%08lx\n", static_cast<unsigned long>(status));
            good &= status == 0; if (!status) adapter = 0;
        }
        return good;
    }
    ~OwnedKmt() { cleanup(); }
};
static void nt(NTSTATUS status, const char* stage) {
    std::printf("NATIVE_SYNC_NT stage=%s status=%08lx\n", stage, static_cast<unsigned long>(status));
    if (status != 0) throw std::runtime_error(stage);
}
static void hr(HRESULT status, const char* stage) {
    std::printf("NATIVE_SYNC_HR stage=%s status=%08lx\n", stage, static_cast<unsigned long>(status));
    if (status != S_OK) throw std::runtime_error(stage);
}
static void wait(volatile UINT64* original, volatile UINT64* imported, ID3D12Fence* fence, UINT64 target) {
    const auto deadline = GetTickCount64() + 5000;
    UINT64 a = 0, b = 0, c = 0;
    do {
        MemoryBarrier(); a = original ? *original : target; b = *imported; c = fence->GetCompletedValue();
        if (a == UINT64_MAX || b == UINT64_MAX || c == UINT64_MAX) throw std::runtime_error("DeviceRemoved");
        if (a >= target && b >= target && c >= target) {
            std::printf("NATIVE_SYNC_GPU_RETIRED target=%llu source=%llu recipient=%llu d3d12=%llu sourceAlive=%u\n",
                target, a, b, c, original ? 1u : 0u);
            return;
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
    throw std::runtime_error("RetirementTimeout");
}
int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try {
        ComPtr<IDXGIFactory4> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Factory");
        ComPtr<IDXGIAdapter1> selected;
        DXGI_ADAPTER_DESC1 description{};
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const auto status = factory->EnumAdapters1(index, &candidate);
            if (status == DXGI_ERROR_NOT_FOUND) break;
            hr(status, "Enumerate"); hr(candidate->GetDesc1(&description), "Description");
            if (description.VendorId == 0x10de && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                selected = candidate; break;
            }
        }
        if (!selected) throw std::runtime_error("NvidiaMissing");
        std::printf("NATIVE_SYNC_BEGIN vendor=%u device=%u cpuWritesFence=false guest=false desktop=false\n",
            description.VendorId, description.DeviceId);
        OwnedKmt owned;
        D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid = description.AdapterLuid;
        nt(D3DKMTOpenAdapterFromLuid(&adapter), "OpenAdapter"); owned.adapter = adapter.hAdapter;
        for (auto* id : {&owned.sourceDevice, &owned.recipientDevice}) {
            D3DKMT_CREATEDEVICE device{}; device.hAdapter = owned.adapter; device.Flags.RequestVSync = 1;
            nt(D3DKMTCreateDevice(&device), "CreateDevice"); *id = device.hDevice;
        }
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 source{}; source.hDevice = owned.sourceDevice;
        source.Info.Type = D3DDDI_MONITORED_FENCE;
        source.Info.Flags.Shared = source.Info.Flags.NtSecuritySharing = 1;
        source.Info.MonitoredFence.InitialFenceValue = 0;
        source.Info.MonitoredFence.EngineAffinity = 1;
        nt(D3DKMTCreateSynchronizationObject2(&source), "CreateSource"); owned.source = source.hSyncObject;
        if (!owned.source || source.Info.SharedHandle || !source.Info.MonitoredFence.FenceValueCPUVirtualAddress ||
            !source.Info.MonitoredFence.FenceValueGPUVirtualAddress) throw std::runtime_error("SourceLayout");
        driver_shared::SyncHandle exported;
        nt(exported.share(owned.source), "ExportNt");
        driver_shared::SyncHandle alias;
        nt(alias.duplicate(exported.get()),"DuplicateNt");
        if(alias.get()==exported.get() || !exported.close()) throw std::runtime_error("AliasOwnership");
        puts("NATIVE_SYNC_ALIAS independentNtReference=true originalNtClosedBeforeImport=true inheritable=false");
        D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 imported{};
        nt(alias.open(owned.recipientDevice, 0, 1, imported), "ImportKmt"); owned.recipient = imported.hSyncObject;
        if (!owned.recipient || owned.recipient == owned.source || !imported.MonitoredFence.FenceValueCPUVirtualAddress ||
            !imported.MonitoredFence.FenceValueGPUVirtualAddress) throw std::runtime_error("RecipientLayout");
        ComPtr<ID3D12Device> device; hr(D3D12CreateDevice(selected.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), "D3D12Device");
        ComPtr<ID3D12Fence> fence; hr(device->OpenSharedHandle(alias.get(), IID_PPV_ARGS(&fence)), "ImportD3D12");
        D3D12_COMMAND_QUEUE_DESC queueDesc{}; queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue; hr(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Queue");
        auto sourceCpu = static_cast<volatile UINT64*>(source.Info.MonitoredFence.FenceValueCPUVirtualAddress);
        auto recipientCpu = static_cast<volatile UINT64*>(imported.MonitoredFence.FenceValueCPUVirtualAddress);
        for (UINT64 target = 1; target <= 8; ++target) {
            hr(queue->Signal(fence.Get(), target), "QueueSignal"); wait(sourceCpu, recipientCpu, fence.Get(), target);
        }
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy{}; destroy.hSyncObject = owned.source;
        nt(D3DKMTDestroySynchronizationObject(&destroy), "DestroySource"); owned.source = 0; sourceCpu = nullptr;
        if (!alias.close()) throw std::runtime_error("CloseAlias");
        puts("NATIVE_SYNC_SOURCE_RELEASED originalDestroyed=true ntHandleClosed=true aliasesLive=true");
        for (UINT64 target = 9; target <= 16; ++target) {
            hr(queue->Signal(fence.Get(), target), "QueueSignalAfterClose"); wait(nullptr, recipientCpu, fence.Get(), target);
        }
        driver_shared::SyncHandle reexported;
        nt(reexported.share(owned.recipient), "ReexportRecipient");
        ComPtr<ID3D12Fence> reopened; hr(device->OpenSharedHandle(reexported.get(), IID_PPV_ARGS(&reopened)), "ReimportD3D12");
        if (reopened->GetCompletedValue() != 16 || !reexported.close()) throw std::runtime_error("ReexportIdentity");
        reopened.Reset(); queue.Reset(); fence.Reset(); device.Reset();
        if (!owned.cleanup()) throw std::runtime_error("Cleanup");
        puts("NATIVE_SYNC_COMPLETE gpuSignals=16 originalReleasedAfter=8 ntClosedBeforeLast=8 aliasesReleased=true cleanup=true guest=false desktop=false");
        return 0;
    } catch (const std::exception& error) {
        std::printf("NATIVE_SYNC_FAILURE stage=%s\n", error.what());
        return 1;
    }
}
