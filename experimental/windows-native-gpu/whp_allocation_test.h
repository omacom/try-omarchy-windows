// SPDX-License-Identifier: MIT
// Direct guest writes to an owned, locked allocation. Parent restores its words
// only after the child partition exits; no memory contents enter public output.
#pragma once
#include "whp_fence_test.h"
namespace whp_allocation {
constexpr std::uint64_t Marker = 0x9b40e26581fa37cdull;
constexpr std::uint64_t LastMarker = 0xc61d43f50872eba9ull;
constexpr std::size_t Bytes = 65536;
inline int guestWrite(HANDLE source, std::uintptr_t address) {
    if (!source || !address || address % 4096) return 10;
    WHV_PARTITION_HANDLE partition = nullptr;
    auto memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 11;
    bool vp = false, mapped = false; int result = 12;
    do {
        if (FAILED(WHvCreatePartition(&partition))) break;
        UINT32 count = 1; result = 13;
        if (FAILED(WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeProcessorCount, &count, sizeof count))) break;
        result = 14; if (FAILED(WHvSetupPartition(partition))) break;
        // DS addresses the owned 64 KiB allocation, ES the result page.
        // Write and read back both ends, with SFENCE before the guest loads.
        const unsigned char code[]{
            0x66,0xc7,0x06,0,0,0xcd,0x37,0xfa,0x81,
            0x66,0xc7,0x06,4,0,0x65,0xe2,0x40,0x9b,
            0x66,0xc7,0x06,0xf8,0xff,0xa9,0xeb,0x72,0x08,
            0x66,0xc7,0x06,0xfc,0xff,0xf5,0x43,0x1d,0xc6,
            0x0f,0xae,0xf8,
            0x66,0xa1,0,0,0x26,0x66,0xa3,0,0x30,
            0x66,0xa1,4,0,0x26,0x66,0xa3,4,0x30,
            0x66,0xa1,0xf8,0xff,0x26,0x66,0xa3,8,0x30,
            0x66,0xa1,0xfc,0xff,0x26,0x66,0xa3,12,0x30,0xf4};
        std::memcpy(memory, code, sizeof code); std::memset(memory + 4096, 0, 4096);
        result = 15;
        if (FAILED(WHvMapGpaRange(partition, memory, 0x1000, 4096, WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute)) ||
            FAILED(WHvMapGpaRange(partition, memory + 4096, 0x3000, 4096, WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite))) break;
        result = 16;
        if (FAILED(WHvMapGpaRange2(partition, source, reinterpret_cast<void*>(address), 0x10000, Bytes,
                                 WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite))) break;
        mapped = true; result = 17;
        if (FAILED(WHvCreateVirtualProcessor(partition, 0, 0))) break; vp = true;
        const WHV_REGISTER_NAME names[]{WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs, WHvX64RegisterSs,
            WHvX64RegisterRip, WHvX64RegisterRflags, WHvX64RegisterCr0, WHvX64RegisterCr4, WHvX64RegisterEfer};
        WHV_REGISTER_VALUE values[9]{};
        values[0].Segment.Base = 0x1000; values[0].Segment.Selector = 0x100;
        values[0].Segment.Limit = 0xffff; values[0].Segment.Attributes = 0x9b;
        for (unsigned n = 1; n < 4; ++n) { values[n].Segment.Limit = 0xffff; values[n].Segment.Attributes = 0x93; }
        values[1].Segment.Base = 0x10000; values[1].Segment.Selector = 0x1000;
        values[5].Reg64 = 2; values[6].Reg64 = 0x10;
        result = 18; if (FAILED(WHvSetVirtualProcessorRegisters(partition, 0, names, 9, values))) break;
        std::mutex guard; std::condition_variable signal; bool done = false;
        std::thread timer([&] { std::unique_lock<std::mutex> hold(guard);
            if (!signal.wait_for(hold, std::chrono::seconds(5), [&]{ return done; })) WHvCancelRunVirtualProcessor(partition, 0, 0); });
        WHV_RUN_VP_EXIT_CONTEXT exit{};
        const auto hr = WHvRunVirtualProcessor(partition, 0, &exit, sizeof exit);
        { std::lock_guard<std::mutex> hold(guard); done = true; }
        signal.notify_one(); timer.join();
        result = 19; if (FAILED(hr)) break;
        result = 20; if (exit.ExitReason != WHvRunVpExitReasonX64Halt) break;
        std::uint64_t actual[2]{}; std::memcpy(actual, memory + 4096, sizeof actual);
        result = actual[0] == Marker && actual[1] == LastMarker ? 0 : 21;
    } while (false);
    if (partition) {
        if (vp && FAILED(WHvDeleteVirtualProcessor(partition, 0))) result = 22;
        if (mapped && FAILED(WHvUnmapGpaRange(partition, 0x10000, Bytes))) result = 25;
        if (FAILED(WHvDeletePartition(partition))) return 23;
    }
    if (!VirtualFree(memory, 0, MEM_RELEASE)) result = 24;
    return result;
}
inline int childMain(int argc, char** argv) {
    std::uint64_t handle = 0, address = 0;
    if (argc != 4 || !whp_fence::number(argv[2], handle) || !whp_fence::number(argv[3], address)) return 2;
    const auto source = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle));
    const auto result = guestWrite(source, static_cast<std::uintptr_t>(address));
    return CloseHandle(source) ? result : 26;
}
inline DWORD runChild(void* data) {
    if (!data || reinterpret_cast<std::uintptr_t>(data) % 4096) return 30;
    auto last = static_cast<unsigned char*>(data) + Bytes - sizeof(std::uint64_t);
    std::uint64_t original = 0, originalLast = 0;
    std::memcpy(&original, data, sizeof original); std::memcpy(&originalLast, last, sizeof originalLast);
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 31;
    HANDLE source = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &source,
                         PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, TRUE, 0)) return 32;
    SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!attributes) { CloseHandle(source); return 37; }
    const bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &size) != FALSE;
    const bool ready = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                               &source, sizeof source, nullptr, nullptr);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --whp-allocation-child " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(source)) + L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(data));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    const bool started = ready && CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
    if (initialized) DeleteProcThreadAttributeList(attributes); HeapFree(GetProcessHeap(), 0, attributes); CloseHandle(source);
    if (!started) return 33;
    CloseHandle(child.hThread);
    DWORD result = 34;
    if (WaitForSingleObject(child.hProcess, 10000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 35);
        if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) {
            // Keep the locked allocation alive while a surviving child could
            // still access it. A failed stop cannot justify parent teardown.
            std::fputs("WHP_ALLOCATION_STOP_PENDING nativeAllocationRetained=true\n", stderr);
            WaitForSingleObject(child.hProcess, INFINITE);
        }
    }
    GetExitCodeProcess(child.hProcess, &result); CloseHandle(child.hProcess);
    MemoryBarrier(); std::uint64_t actual = 0, actualLast = 0;
    std::memcpy(&actual, data, sizeof actual); std::memcpy(&actualLast, last, sizeof actualLast);
    const bool visible = actual == Marker && actualLast == LastMarker;
    std::memcpy(data, &original, sizeof original); std::memcpy(last, &originalLast, sizeof originalLast); MemoryBarrier();
    std::uint64_t restored = 0, restoredLast = 0;
    std::memcpy(&restored, data, sizeof restored); std::memcpy(&restoredLast, last, sizeof restoredLast);
    if (restored != original || restoredLast != originalLast) return 41;
    return visible ? result : 40;
}
} // namespace whp_allocation
