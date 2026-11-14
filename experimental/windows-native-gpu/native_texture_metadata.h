// SPDX-License-Identifier: MIT
// Experimental native runtime metadata generation for a declared texture.
// Opaque bytes stay on the Windows host. No private layout is decoded here.
#pragma once
#include "driver_wire.h"
#include <array>
#include <iostream>
#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>

namespace driver_metadata {
struct Lease {
    HANDLE shared = nullptr;
    D3DKMT_HANDLE adapter = 0, device = 0, resource = 0;
    NTSTATUS resourceStatus = 0, deviceStatus = 0, adapterStatus = 0;
    bool handleClosed = false, released = false;
    void release() noexcept {
        if (released) return;
        if (resource) {
            D3DKMT_DESTROYALLOCATION2 a{}; a.hDevice = device; a.hResource = resource;
            a.Flags.SynchronousDestroy = 1; resourceStatus = D3DKMTDestroyAllocation2(&a);
        }
        if (device) { D3DKMT_DESTROYDEVICE a{}; a.hDevice = device; deviceStatus = D3DKMTDestroyDevice(&a); }
        if (adapter) { D3DKMT_CLOSEADAPTER a{}; a.hAdapter = adapter; adapterStatus = D3DKMTCloseAdapter(&a); }
        if (shared) handleClosed = CloseHandle(shared) != FALSE;
        released = true;
        // Do not continue GPU experiments if native ownership is uncertain.
        if (resourceStatus || deviceStatus || adapterStatus || (shared && !handleClosed)) ExitProcess(3);
    }
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() { release(); }
};

inline std::vector<std::uint8_t> generate(LUID luid, driver_bridge::NativeTextureDesc profile, bool interop=false) {
    if (interop ? !driver_bridge::validNativeInteropTexture(profile) : !driver_bridge::validNativeTexture(profile))
        throw std::runtime_error("Unsupported native texture profile");
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device; ComPtr<ID3D12Resource> prototype;
    auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) throw std::runtime_error("Cannot create native metadata generator device");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = profile.width; desc.Height = profile.height; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = static_cast<DXGI_FORMAT>(profile.format); desc.SampleDesc.Count = 1;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(profile.flags);
    const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    if ((!gpu_diagnostic::HighResolution && info.SizeInBytes != 65536) ||
        !info.SizeInBytes || info.SizeInBytes > gpu_diagnostic::MaxMappingBytes || info.Alignment != 65536) throw std::runtime_error("Unexpected native template allocation requirements");
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&prototype));
    if (FAILED(hr)) throw std::runtime_error("Cannot create native metadata template");
    Lease owned;
    hr = device->CreateSharedHandle(prototype.Get(), nullptr, GENERIC_ALL, nullptr, &owned.shared);
    if (FAILED(hr) || !owned.shared) throw std::runtime_error("Cannot share native metadata template");
    D3DKMT_OPENADAPTERFROMLUID kmtAdapter{}; kmtAdapter.AdapterLuid = luid;
    const auto adapterStatus = D3DKMTOpenAdapterFromLuid(&kmtAdapter); owned.adapter = kmtAdapter.hAdapter;
    D3DKMT_CREATEDEVICE kmtDevice{}; kmtDevice.hAdapter = owned.adapter;
    const auto deviceStatus = adapterStatus == 0 && owned.adapter ? D3DKMTCreateDevice(&kmtDevice) : static_cast<NTSTATUS>(0xc0000001u);
    owned.device = kmtDevice.hDevice;
    D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE size{}; size.hDevice = owned.device; size.hNtHandle = owned.shared;
    const auto queryStatus = deviceStatus == 0 && owned.device ? D3DKMTQueryResourceInfoFromNtHandle(&size) : static_cast<NTSTATUS>(0xc0000001u);
    std::array<std::uint8_t, 1024> runtime{};
    std::array<std::uint8_t, 4096> resourceData{}, allocationData{};
    D3DDDI_OPENALLOCATIONINFO2 allocation{}; D3DKMT_OPENRESOURCEFROMNTHANDLE opened{};
    NTSTATUS openStatus = static_cast<NTSTATUS>(0xc0000001u);
    if (queryStatus == 0 && size.NumAllocations == 1 && size.PrivateRuntimeDataSize && size.PrivateRuntimeDataSize <= runtime.size() &&
        size.TotalPrivateDriverDataSize && size.TotalPrivateDriverDataSize <= allocationData.size() && size.ResourcePrivateDriverDataSize <= resourceData.size()) {
        opened.hDevice = owned.device; opened.hNtHandle = owned.shared; opened.NumAllocations = 1; opened.pOpenAllocationInfo2 = &allocation;
        opened.PrivateRuntimeDataSize = size.PrivateRuntimeDataSize; opened.pPrivateRuntimeData = runtime.data();
        opened.ResourcePrivateDriverDataSize = size.ResourcePrivateDriverDataSize;
        opened.pResourcePrivateDriverData = size.ResourcePrivateDriverDataSize ? resourceData.data() : nullptr;
        opened.TotalPrivateDriverDataBufferSize = size.TotalPrivateDriverDataSize; opened.pTotalPrivateDriverDataBuffer = allocationData.data();
        openStatus = D3DKMTOpenResourceFromNtHandle(&opened); owned.resource = opened.hResource;
    }
    if (opened.hKeyedMutex || opened.hSyncObject) ExitProcess(3);
    const bool success = openStatus == 0 && owned.resource && allocation.hAllocation &&
        opened.PrivateRuntimeDataSize == size.PrivateRuntimeDataSize && opened.PrivateRuntimeDataSize <= runtime.size();
    std::vector<std::uint8_t> result;
    if (success) result.assign(runtime.begin(), runtime.begin() + opened.PrivateRuntimeDataSize);
    owned.release();
    prototype.Reset(); device.Reset(); adapter.Reset(); factory.Reset();
    std::cerr << "{\"nativeTextureMetadataGenerated\":" << (success ? "true" : "false")
        << ",\"width\":" << profile.width << ",\"height\":" << profile.height << ",\"format\":" << profile.format
        << ",\"flags\":" << profile.flags << ",\"allocationBytes\":" << info.SizeInBytes
        << ",\"allocationAlignment\":" << info.Alignment << ",\"runtimeBytes\":" << result.size()
        << ",\"adapterNtstatus\":" << adapterStatus << ",\"deviceNtstatus\":" << deviceStatus
        << ",\"queryNtstatus\":" << queryStatus << ",\"openNtstatus\":" << openStatus
        << ",\"resourceDestroyedNtstatus\":" << owned.resourceStatus << ",\"deviceDestroyedNtstatus\":" << owned.deviceStatus
        << ",\"adapterClosedNtstatus\":" << owned.adapterStatus << ",\"sharedHandleClosed\":" << (owned.handleClosed ? "true" : "false")
        << ",\"prototypeReleased\":true,\"privateBytesExported\":false}\n";
    if (!success) throw std::runtime_error("Native Windows metadata extraction failed");
    return result;
}
} // namespace driver_metadata
