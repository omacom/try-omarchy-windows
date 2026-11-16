// SPDX-License-Identifier: MIT
// Isolated diagnostic: execute guest loads from driver-owned pages in the
// parent process. This is not a QEMU integration or a guest graphics runtime.
#pragma once
#include <windows.h>
#include <WinHvPlatform.h>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace whp_fence {
enum class Mode : unsigned { Read, UnmappedRead, ReadOnlyWrite };

// Runs in an owned child, with the parent alive until its partition is gone.
// Return codes identify the failed stage; no host pointers enter the report.
inline int guestRead(HANDLE source, std::uintptr_t address, std::uint64_t expected, Mode mode) {
    const auto offset = address & 4095;
    if (!source || !address || offset > 4088 || offset % 8) return 10;
    WHV_PARTITION_HANDLE partition = nullptr;
    auto memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return 11;
    bool vp = false, mapped = false;
    int result = 12;
    do {
        if (FAILED(WHvCreatePartition(&partition))) break;
        UINT32 count = 1; result = 13;
        if (FAILED(WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeProcessorCount,
                &count, sizeof count))) break;
        result = 14; if (FAILED(WHvSetupPartition(partition))) break;
        // Real-mode 64-bit value read as two 32-bit loads. The parent keeps
        // these fences idle throughout the test; this is not an atomicity test.
        const auto low = static_cast<unsigned short>(0x2000 + offset);
        const auto high = static_cast<unsigned short>(low + 4);
        const unsigned char code[]{
            0x66, 0xa1, static_cast<unsigned char>(low), static_cast<unsigned char>(low >> 8),
            0x66, 0xa3, 0, 0x30,
            0x66, 0xa1, static_cast<unsigned char>(high), static_cast<unsigned char>(high >> 8),
            0x66, 0xa3, 4, 0x30, 0xf4};
        std::memcpy(memory, code, sizeof code);
        if (mode == Mode::ReadOnlyWrite) {
            // mov dword ptr [fence], 0xa5a5a5a5; hlt. A read-only GPA mapping
            // must exit before this store reaches the actual driver page.
            const unsigned char write[]{0x66, 0xc7, 0x06,
                static_cast<unsigned char>(low), static_cast<unsigned char>(low >> 8),
                0xa5, 0xa5, 0xa5, 0xa5, 0xf4};
            std::memcpy(memory, write, sizeof write);
        }
        std::memset(memory + 4096, 0xa5, 4096);
        result = 15;
        if (FAILED(WHvMapGpaRange(partition, memory, 0x1000, 4096,
                WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute))) break;
        if (FAILED(WHvMapGpaRange(partition, memory + 4096, 0x3000, 4096,
                WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite))) break;
        if (mode != Mode::UnmappedRead) {
            result = 16;
            if (FAILED(WHvMapGpaRange2(partition, source,
                    reinterpret_cast<void*>(address - offset), 0x2000, 4096,
                    WHvMapGpaRangeFlagRead))) break;
            mapped = true;
        }
        result = 17; if (FAILED(WHvCreateVirtualProcessor(partition, 0, 0))) break;
        vp = true;
        const WHV_REGISTER_NAME names[]{WHvX64RegisterCs, WHvX64RegisterDs,
            WHvX64RegisterEs, WHvX64RegisterSs, WHvX64RegisterRip,
            WHvX64RegisterRflags, WHvX64RegisterCr0, WHvX64RegisterCr4, WHvX64RegisterEfer};
        WHV_REGISTER_VALUE values[9]{};
        values[0].Segment.Base = 0x1000; values[0].Segment.Selector = 0x100;
        values[0].Segment.Limit = 0xffff; values[0].Segment.Attributes = 0x9b;
        for (unsigned i = 1; i < 4; ++i) {
            values[i].Segment.Limit = 0xffff; values[i].Segment.Attributes = 0x93;
        }
        values[5].Reg64 = 2; values[6].Reg64 = 0x10;
        result = 18;
        if (FAILED(WHvSetVirtualProcessorRegisters(partition, 0, names, 9, values))) break;
        std::mutex guard; std::condition_variable signal; bool done = false;
        std::thread timer([&] {
            std::unique_lock<std::mutex> lock(guard);
            if (!signal.wait_for(lock, std::chrono::seconds(5), [&]{ return done; }))
                WHvCancelRunVirtualProcessor(partition, 0, 0);
        });
        WHV_RUN_VP_EXIT_CONTEXT exit{};
        const auto hr = WHvRunVirtualProcessor(partition, 0, &exit, sizeof exit);
        { std::lock_guard<std::mutex> lock(guard); done = true; }
        signal.notify_one(); timer.join();
        result = 19; if (FAILED(hr)) break;
        result = 20;
        if (mode == Mode::Read) {
            if (exit.ExitReason != WHvRunVpExitReasonX64Halt) break;
            std::uint64_t actual = 0;
            std::memcpy(&actual, memory + 4096, sizeof actual);
            result = actual == expected ? 0 : 21;
        } else {
            const auto access = mode == Mode::ReadOnlyWrite ? WHvMemoryAccessWrite : WHvMemoryAccessRead;
            if (exit.ExitReason != WHvRunVpExitReasonMemoryAccess ||
                exit.MemoryAccess.Gpa != low || exit.MemoryAccess.AccessInfo.AccessType != static_cast<UINT32>(access)) break;
            result = 0;
        }
    } while (false);
    if (partition) {
        if (vp && FAILED(WHvDeleteVirtualProcessor(partition, 0))) result = 22;
        if (mapped && FAILED(WHvUnmapGpaRange(partition, 0x2000, 4096))) result = 25;
        // On a teardown error leave code/output backing alive until this
        // child exits; never free pages a surviving partition could still use.
        if (FAILED(WHvDeletePartition(partition))) return 23;
    }
    if (!VirtualFree(memory, 0, MEM_RELEASE)) result = 24;
    return result;
}

inline bool number(const char* text, std::uint64_t& value) {
    const auto end = text + std::strlen(text);
    const auto parsed = std::from_chars(text, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

inline int childMain(int argc, char** argv) {
    std::uint64_t handle = 0, address = 0, expected = 0, mode = 0;
    if (argc != 6 || !number(argv[2], handle) || !number(argv[3], address) ||
        !number(argv[4], expected) || !number(argv[5], mode) || mode > 2) return 2;
    const auto source = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(handle));
    const auto result = guestRead(source, static_cast<std::uintptr_t>(address), expected,
        static_cast<Mode>(mode));
    return CloseHandle(source) ? result : 26;
}

inline DWORD runChild(volatile UINT64* fence, Mode mode) {
    if (!fence) return 30;
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return 31;
    HANDLE source = nullptr;
    // These are the documented WHvMapGpaRange2 process rights. Only this
    // handle is inherited, and it refers to this probe's own parent process.
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &source,
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, TRUE, 0)) return 32;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
    if (!attributes) { CloseHandle(source); return 37; }
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        HeapFree(GetProcessHeap(), 0, attributes); CloseHandle(source); return 38;
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &source, sizeof source, nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes); HeapFree(GetProcessHeap(), 0, attributes);
        CloseHandle(source); return 39;
    }
    const auto expected = *fence;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --whp-fence-child " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(source)) + L" " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(fence)) + L" " +
        std::to_wstring(expected) + L" " + std::to_wstring(static_cast<unsigned>(mode));
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup;
    startup.lpAttributeList = attributes; PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
    DeleteProcThreadAttributeList(attributes); HeapFree(GetProcessHeap(), 0, attributes);
    CloseHandle(source);
    if (!started) return 33;
    CloseHandle(child.hThread);
    DWORD result = 34;
    if (WaitForSingleObject(child.hProcess, 10000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 35);
        // Parent driver objects must remain live while a child could map them.
        if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) ExitProcess(36);
    }
    GetExitCodeProcess(child.hProcess, &result); CloseHandle(child.hProcess);
    MemoryBarrier();
    return *fence == expected ? result : 40;
}
} // namespace whp_fence
