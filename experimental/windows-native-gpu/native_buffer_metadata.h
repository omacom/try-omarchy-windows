// SPDX-License-Identifier: MIT
// Generate opaque Windows runtime metadata from a declared public D3D12 buffer.
// The Linux runtime bytes remain independently owned by the guest resource.
#pragma once
#include "native_texture_metadata.h"
#include "native_metadata_attempt.h"

namespace driver_metadata {
template<class Admit>
inline std::vector<std::uint8_t> generateBuffer(LUID luid, driver_bridge::NativeGpuBufferDesc profile, Admit&& admit) {
    if (!driver_bridge::validNativeGpuBuffer(profile)) throw std::runtime_error("Unsupported native buffer profile");
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory4> factory; ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device; ComPtr<ID3D12Resource> prototype;
    auto hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
    if (SUCCEEDED(hr)) hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    checkMetadataMemoryCode(hr);
    if (FAILED(hr)) throw std::runtime_error("Cannot create native buffer metadata device");
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = profile.bytes; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(profile.flags);
    const auto info = device->GetResourceAllocationInfo(0, 1, &desc);
    if (!info.SizeInBytes || info.SizeInBytes > driver_bridge::NativeGpuBufferProfile.maxBytes ||
        info.Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
        throw std::runtime_error("Unexpected native buffer template allocation requirements");
    requireMetadataBudget(admit(info.SizeInBytes), MemoryFailure::BeforeTemplate);
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&prototype));
    checkMetadataMemoryCode(hr);
    if (FAILED(hr)) throw std::runtime_error("Cannot create native buffer metadata template");
    requireMetadataBudget(admit(0), MemoryFailure::AfterTemplate);
    Lease owned;
    hr = device->CreateSharedHandle(prototype.Get(), nullptr, GENERIC_ALL, nullptr, &owned.shared);
    checkMetadataMemoryCode(hr);
    if (FAILED(hr) || !owned.shared) throw std::runtime_error("Cannot share native buffer metadata template");
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
    owned.release(); prototype.Reset(); device.Reset(); adapter.Reset(); factory.Reset();
    std::cerr << "{\"nativeGpuBufferMetadataGenerated\":" << (success ? "true" : "false")
        << ",\"bytes\":" << profile.bytes << ",\"flags\":" << profile.flags << ",\"allocationBytes\":" << info.SizeInBytes
        << ",\"allocationAlignment\":" << info.Alignment << ",\"runtimeBytes\":" << result.size()
        << ",\"adapterNtstatus\":" << adapterStatus << ",\"deviceNtstatus\":" << deviceStatus
        << ",\"queryNtstatus\":" << queryStatus << ",\"openNtstatus\":" << openStatus
        << ",\"resourceDestroyedNtstatus\":" << owned.resourceStatus << ",\"deviceDestroyedNtstatus\":" << owned.deviceStatus
        << ",\"adapterClosedNtstatus\":" << owned.adapterStatus << ",\"sharedHandleClosed\":" << (owned.handleClosed ? "true" : "false")
        << ",\"prototypeReleased\":true,\"reportedUsageChecked\":true,\"privateBytesExported\":false}\n";
    if (!success) {
        for (const auto code : {adapterStatus, deviceStatus, queryStatus, openStatus}) checkMetadataMemoryCode(code);
        throw std::runtime_error("Native Windows buffer metadata extraction failed");
    }
    return result;
}
}
