// SPDX-License-Identifier: MIT
// Native-only diagnostic using one local captured allocation input.
// No guest runtime, private data output, command submission or host pointers in JSON.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "whp_fence_test.h"
#include "qmp_protocol.h"
#include "whp_allocation_test.h"
using Microsoft::WRL::ComPtr;
using Json = driver_qmp::Json;
int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "--whp-fence-child")) return whp_fence::childMain(argc, argv);
    if (argc >= 2 && !std::strcmp(argv[1], "--whp-allocation-child")) return whp_allocation::childMain(argc, argv);
    if (argc != 2) return 2;
    FILE* file = nullptr;
    if (fopen_s(&file, argv[1], "rb") || !file) return 2;
    std::uint32_t header[8]{};
    bool valid = std::fread(header, 1, sizeof header, file) == sizeof header && header[0] == 0x31434c41 &&
        !header[1] && header[2] == 4 && header[3] >= 0x28000000 && header[3] <= 0x78ffffff &&
        !header[4] && header[5] && header[5] <= 4000 && !header[6] && !header[7];
    std::vector<unsigned char> payload(valid ? header[5] : 0);
    valid = valid && std::fread(payload.data(), 1, payload.size(), file) == payload.size() && std::fgetc(file) == EOF;
    std::fclose(file); if (!valid) return 2;
    Json report{{"schema", 1}, {"scope", "Native Lock2 and WHP foreign-page reads/writes using one local captured private input; no QEMU or live UMD"},
                {"capturedPrivateFixtureUsed", true}, {"privateBytes", payload.size()}, {"success", false},
                {"guestWritesTested", false}, {"hostWriteVisibilityTested", false}, {"originalWordRestoredAfterChildExit", false},
                {"firstAndLastPagesWriteVerified", false}, {"guestWritableMappingBytes", whp_allocation::Bytes}};
    D3DKMT_OPENADAPTERFROMLUID adapter{};
    D3DKMT_CREATEDEVICE device{};
    D3DKMT_CREATEPAGINGQUEUE paging{};
    D3DDDI_ALLOCATIONINFO2 info{};
    D3DKMT_LOCK2 lock{};
    bool opened = false, createdDevice = false, createdPaging = false, allocated = false, locked = false, ok = false;
    D3DKMT_HANDLE allocationResource = 0;
    ComPtr<IDXGIAdapter3> budgetAdapter;
    std::uint64_t peakReportedUsage = 0;
    const auto budget = [&]() {
        if (!budgetAdapter) return false;
        std::uint64_t usage = 0;
        for (const auto group : {DXGI_MEMORY_SEGMENT_GROUP_LOCAL, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL}) {
            DXGI_QUERY_VIDEO_MEMORY_INFO query{};
            if (FAILED(budgetAdapter->QueryVideoMemoryInfo(0, group, &query)) || query.CurrentUsage > UINT64_MAX - usage) return false;
            usage += query.CurrentUsage;
        }
        peakReportedUsage = (std::max)(peakReportedUsage, usage);
        return usage <= 64ull * 1024 * 1024;
    };
    const auto record = [&](const char* name, NTSTATUS status) { report[name] = static_cast<std::int32_t>(status); return status >= 0; };
    do {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) break;
        bool found = false;
        for (UINT n = 0;; ++n) {
            ComPtr<IDXGIAdapter1> item;
            const auto result = factory->EnumAdapters1(n, &item);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(result) || FAILED(item->GetDesc1(&desc))) break;
            if (desc.VendorId == 0x10de && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                adapter.AdapterLuid = desc.AdapterLuid;
                report["vendorId"] = desc.VendorId; report["deviceId"] = desc.DeviceId;
                found = SUCCEEDED(item.As(&budgetAdapter)); break;
            }
        }
        if (!found || !budget()) break;
        opened = record("openAdapterStatus", D3DKMTOpenAdapterFromLuid(&adapter)); if (!opened) break;
        device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
        createdDevice = record("createDeviceStatus", D3DKMTCreateDevice(&device)); if (!createdDevice) break;
        paging.hDevice = device.hDevice; paging.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        createdPaging = record("createPagingStatus", D3DKMTCreatePagingQueue(&paging)); if (!createdPaging) break;
        const auto wait = [&](std::uint64_t target) {
            const auto fence = static_cast<volatile std::uint64_t*>(paging.FenceValueCPUVirtualAddress);
            if (!fence) return false;
            const auto deadline = GetTickCount64() + 5000;
            while (*fence < target && GetTickCount64() < deadline) Sleep(1);
            MemoryBarrier(); return *fence >= target;
        };
        info.Flags.Value = header[2]; info.Priority = header[3];
        info.pPrivateDriverData = payload.data(); info.PrivateDriverDataSize = header[5];
        D3DKMT_CREATEALLOCATION allocation{}; allocation.hDevice = device.hDevice;
        allocation.NumAllocations = 1; allocation.pAllocationInfo2 = &info;
        allocated = record("createAllocationStatus", D3DKMTCreateAllocation2(&allocation)); if (!allocated) break;
        allocationResource = allocation.hResource;
        if (!info.hAllocation || allocation.hResource || allocation.hGlobalShare || !budget()) break;
        D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue = paging.hPagingQueue; map.hAllocation = info.hAllocation;
        map.MinimumAddress = 67108864; map.MaximumAddress = 1099511627776; map.SizeInPages = 16; map.Protection.Value = 1;
        if (!record("mapGpuVaStatus", D3DKMTMapGpuVirtualAddress(&map)) || !wait(map.PagingFenceValue)) break;
        report["mappedPages"] = map.SizeInPages;
        D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue = paging.hPagingQueue; resident.NumAllocations = 1;
        resident.AllocationList = &info.hAllocation; resident.PriorityList = &info.Priority; resident.Flags.CantTrimFurther = 1;
        if (!record("makeResidentStatus", D3DKMTMakeResident(&resident)) || !wait(resident.PagingFenceValue) || !budget()) break;
        lock.hDevice = device.hDevice; lock.hAllocation = info.hAllocation;
        locked = record("lock2Status", D3DKMTLock2(&lock)); if (!locked) break;
        MEMORY_BASIC_INFORMATION region{};
        const bool queried = lock.pData && VirtualQuery(lock.pData, &region, sizeof region) == sizeof region;
        report["virtualQuerySucceeded"] = queried;
        if (!queried) break;
        const auto address = reinterpret_cast<std::uintptr_t>(lock.pData);
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        report["cpuPageOffset"] = address & 4095;
        report["regionBytes"] = region.RegionSize;
        report["regionState"] = region.State; report["regionType"] = region.Type; report["regionProtection"] = region.Protect;
        report["atRegionBase"] = address == start;
        report["atAllocationBase"] = lock.pData == region.AllocationBase;
        report["allocationProtection"] = region.AllocationProtect;
        const auto protection = region.Protect & 255;
        if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD) ||
            protection != PAGE_READWRITE || region.Type != MEM_PRIVATE || lock.pData != region.AllocationBase || address != start ||
            region.RegionSize != 16 * 4096 || address < start ||
            address - start > region.RegionSize || sizeof(UINT64) > region.RegionSize - (address - start)) break;
        report["whpReadChildExit"] = whp_fence::runChild(static_cast<volatile UINT64*>(lock.pData), whp_fence::Mode::Read);
        report["whpReadOnlyWriteControlChildExit"] = whp_fence::runChild(static_cast<volatile UINT64*>(lock.pData), whp_fence::Mode::ReadOnlyWrite);
        report["whpWriteChildExit"] = whp_allocation::runChild(lock.pData);
        report["guestWritesTested"] = true;
        report["hostWriteVisibilityTested"] = report["whpWriteChildExit"] == 0;
        report["originalWordRestoredAfterChildExit"] = report["whpWriteChildExit"] == 0;
        report["firstAndLastPagesWriteVerified"] = report["whpWriteChildExit"] == 0;
        ok = report["whpReadChildExit"] == 0 && report["whpReadOnlyWriteControlChildExit"] == 0 && report["whpWriteChildExit"] == 0 && budget();
    } while (false);
    if (locked) {
        D3DKMT_UNLOCK2 unlock{}; unlock.hDevice = device.hDevice; unlock.hAllocation = info.hAllocation;
        ok = record("unlock2Status", D3DKMTUnlock2(&unlock)) && ok;
    }
    if (allocated) {
        D3DKMT_DESTROYALLOCATION2 destroy{}; destroy.hDevice = device.hDevice;
        destroy.hResource = allocationResource; destroy.Flags.SynchronousDestroy = 1;
        if (!allocationResource) { destroy.phAllocationList = &info.hAllocation; destroy.AllocationCount = 1; }
        ok = record("destroyAllocationStatus", D3DKMTDestroyAllocation2(&destroy)) && ok;
    }
    if (createdPaging) {
        D3DDDI_DESTROYPAGINGQUEUE destroy{}; destroy.hPagingQueue = paging.hPagingQueue;
        ok = record("destroyPagingStatus", D3DKMTDestroyPagingQueue(&destroy)) && ok;
    }
    if (createdDevice) {
        D3DKMT_DESTROYDEVICE destroy{}; destroy.hDevice = device.hDevice;
        ok = record("destroyDeviceStatus", D3DKMTDestroyDevice(&destroy)) && ok;
    }
    if (opened) {
        D3DKMT_CLOSEADAPTER close{}; close.hAdapter = adapter.hAdapter;
        ok = record("closeAdapterStatus", D3DKMTCloseAdapter(&close)) && ok;
    }
    report["peakReportedGpuUsageBytes"] = peakReportedUsage;
    report["success"] = ok;
    std::puts(report.dump(2).c_str()); return ok ? 0 : 1;
}
