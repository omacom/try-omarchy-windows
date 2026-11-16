// SPDX-License-Identifier: MIT
// Owned QEMU child, driver objects retained until the process has exited.
#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace qemu_fence {
struct Paths { const char *qemu, *firmware, *kernel, *initramfs, *log; };

inline std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? 2 * slashes + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(2 * slashes, L'\\'); result += L'"';
    return result;
}

inline DWORD run(volatile UINT64 *paging, volatile UINT64 *queue, const Paths& paths) {
    if (!paging || !queue || !*paging || *queue) return 50;
    const auto executable = std::filesystem::absolute(paths.qemu).wstring();
    HANDLE source = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &source,
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, TRUE, 0)) return 51;
    SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
    const auto logfile = std::filesystem::absolute(paths.log);
    HANDLE log = CreateFileW(logfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { CloseHandle(source); return 52; }
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE) { CloseHandle(log); CloseHandle(source); return 53; }
    HANDLE inherited[]{source, log, input};
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes));
    bool initialized = attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &bytes);
    bool ready = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof inherited, nullptr, nullptr);
    std::vector<std::wstring> args{executable, L"-machine", L"q35,accel=whpx", L"-cpu", L"host",
        L"-m", L"512", L"-smp", L"1", L"-display", L"none", L"-monitor", L"none",
        L"-serial", L"stdio", L"-nodefaults", L"-no-reboot", L"-L",
        std::filesystem::absolute(paths.firmware).wstring(), L"-kernel",
        std::filesystem::absolute(paths.kernel).wstring(), L"-initrd",
        std::filesystem::absolute(paths.initramfs).wstring(), L"-append",
        L"console=ttyS0 rdinit=/init panic=1"};
    const auto device = [&](volatile UINT64 *fence) {
        return L"wddm-fence-lab,source-process=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(source)) + L",source-address=" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(fence)) + L",expected=" + std::to_wstring(*fence);
    };
    args.insert(args.end(), {L"-device", device(paging), L"-device", device(queue)});
    std::wstring command;
    for (const auto& arg : args) { if (!command.empty()) command += L' '; command += quote(arg); }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input; startup.StartupInfo.hStdOutput = log;
    startup.StartupInfo.hStdError = log; startup.lpAttributeList = attributes;
    PROCESS_INFORMATION child{};
    const bool started = ready && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
    if (initialized) DeleteProcThreadAttributeList(attributes);
    if (attributes) HeapFree(GetProcessHeap(), 0, attributes);
    CloseHandle(source); CloseHandle(input); CloseHandle(log);
    if (!started) return 54;
    CloseHandle(child.hThread);
    DWORD result = 55;
    if (WaitForSingleObject(child.hProcess, 60000) != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 56);
        if (WaitForSingleObject(child.hProcess, 5000) != WAIT_OBJECT_0) ExitProcess(57);
    }
    GetExitCodeProcess(child.hProcess, &result); CloseHandle(child.hProcess);
    if (result) return result;
    std::error_code error;
    const auto size = std::filesystem::file_size(logfile, error);
    if (error || size > 16 * 1024 * 1024) return 58;
    std::ifstream stream(logfile, std::ios::binary);
    std::string output(static_cast<size_t>(size), '\0');
    if (!stream.read(output.data(), static_cast<std::streamsize>(output.size()))) return 59;
    const std::string marker = "FENCE_READ direct=true match=true";
    const auto first = output.find(marker);
    if (output.find("FENCE_GUEST_EXIT=0 devices=2 nonzero=1") == std::string::npos ||
        first == std::string::npos || output.find(marker, first + marker.size()) == std::string::npos) return 60;
    return 0;
}
} // namespace qemu_fence
