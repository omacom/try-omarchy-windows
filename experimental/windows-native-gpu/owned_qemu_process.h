// SPDX-License-Identifier: MIT
// A held process object, rather than a bare PID, owns every VM lifetime.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace driver_qemu {
struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline std::uint64_t processCreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process,&created,&exited,&kernel,&user))
        throw std::runtime_error("Cannot query process creation time");
    return (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}
inline DWORD creatorPid(DWORD pid) {
    Handle snapshot; snapshot.value = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot snapshot process identity");
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof entry;
    if (!Process32FirstW(snapshot.value,&entry)) throw std::runtime_error("Cannot read process identity");
    do { if (entry.th32ProcessID == pid) return entry.th32ParentProcessID; }
    while (Process32NextW(snapshot.value,&entry));
    throw std::runtime_error("Process identity disappeared");
}
inline std::wstring processImage(HANDLE process) {
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!QueryFullProcessImageNameW(process,0,buffer.data(),&size) || !size || size >= buffer.size())
        throw std::runtime_error("Cannot query process image");
    return std::wstring(buffer.data(),size);
}
inline void openImage(Handle& file, const std::wstring& name) {
    if (name.empty() || name.find(L'\0') != std::wstring::npos)
        throw std::runtime_error("Invalid process image path");
    file.value = CreateFileW(name.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot pin process image file");
}
inline FILE_ID_INFO imageIdentity(HANDLE file) {
    FILE_ID_INFO result{};
    if (!GetFileInformationByHandleEx(file,FileIdInfo,&result,sizeof result))
        throw std::runtime_error("Cannot query process image file identity");
    return result;
}
inline bool sameImageIdentity(const FILE_ID_INFO& a, const FILE_ID_INFO& b) {
    bool aKnown = false, bKnown = false;
    for (const auto value : a.FileId.Identifier) aKnown = aKnown || value != 0;
    for (const auto value : b.FileId.Identifier) bKnown = bKnown || value != 0;
    return aKnown && bKnown && a.VolumeSerialNumber == b.VolumeSerialNumber &&
        !std::memcmp(a.FileId.Identifier,b.FileId.Identifier,sizeof a.FileId.Identifier);
}
struct ProcessClaim { DWORD pid; std::uint64_t creationTime; };
struct ProcessBinding {
    DWORD pid, creator;
    std::uint64_t creationTime;
    bool heldProcess, privateJob, pinnedImage;
};

class OwnedProcess {
    // Closing this job is a final owner-death fallback. Explicit stop/reap
    // remains mandatory before releasing native allocations and mapped pages.
    Handle job, pinnedImage;
    mutable std::mutex stopGuard;
    bool closed = false;
    ProcessClaim bound{};
    DWORD boundCreator = 0;
public:
    Handle handle;
    DWORD exit = STILL_ACTIVE;
    bool forced = false, reaped = false;
    bool exited() const { return handle.value && WaitForSingleObject(handle.value,0) == WAIT_OBJECT_0; }

    // The caller must already authenticate its launcher/session and hold the
    // creator process. This checks object identity and job/lifetime ownership;
    // parent PIDs alone are not authentication or a process handoff protocol.
    // No process/job mutation occurs until every identity check succeeds.
    void adoptVerified(HANDLE candidate, HANDLE creator, ProcessClaim claim, const std::wstring& expectedImage) {
        std::lock_guard<std::mutex> lock(stopGuard);
        if (closed || handle.value || job.value || reaped || !claim.pid || !claim.creationTime || expectedImage.empty())
            throw std::runtime_error("Invalid or duplicate process adoption");
        Handle owned, image, actualImage, ownedJob;
        constexpr DWORD rights = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_SET_QUOTA;
        if (!DuplicateHandle(GetCurrentProcess(),candidate,GetCurrentProcess(),&owned.value,rights,FALSE,0))
            throw std::runtime_error("Cannot hold candidate process");
        const auto pid = GetProcessId(owned.value), parent = GetProcessId(creator);
        if (!pid || pid != claim.pid || pid == parent || pid == GetCurrentProcessId() || !parent ||
            WaitForSingleObject(owned.value,0) != WAIT_TIMEOUT || WaitForSingleObject(creator,0) != WAIT_TIMEOUT ||
            processCreationTime(owned.value) != claim.creationTime || processCreationTime(creator) > claim.creationTime ||
            creatorPid(pid) != parent)
            throw std::runtime_error("Candidate process identity does not match live creator");
        openImage(image,expectedImage); openImage(actualImage,processImage(owned.value));
        const auto expected = imageIdentity(image.value), actual = imageIdentity(actualImage.value);
        if (!sameImageIdentity(expected,actual))
            throw std::runtime_error("Candidate process executable differs");
        if (WaitForSingleObject(owned.value,0) != WAIT_TIMEOUT || WaitForSingleObject(creator,0) != WAIT_TIMEOUT)
            throw std::runtime_error("Candidate or creator exited before attachment");
        ownedJob.value = CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!ownedJob.value || !SetInformationJobObject(ownedJob.value,JobObjectExtendedLimitInformation,&limit,sizeof limit))
            throw std::runtime_error("Cannot prepare process owner job");
        if (!AssignProcessToJobObject(ownedJob.value,owned.value))
            throw std::runtime_error("Cannot attach verified process to owner job");
        // This is the ownership commit. Keep the non-inheritable kernel object
        // and pinned executable until explicit stop or this owner is destroyed.
        handle.value = owned.value; owned.value = nullptr;
        job.value = ownedJob.value; ownedJob.value = nullptr;
        pinnedImage.value = image.value; image.value = nullptr;
        bound = claim; boundCreator = parent;
    }
    ProcessBinding binding() const {
        std::lock_guard<std::mutex> lock(stopGuard);
        return {bound.pid,boundCreator,bound.creationTime,handle.value != nullptr,job.value != nullptr,pinnedImage.value != nullptr};
    }
    void stop(DWORD grace = 5000) noexcept {
        std::lock_guard<std::mutex> lock(stopGuard);
        closed = true;
        if (!handle.value || reaped) return;
        auto state = WaitForSingleObject(handle.value,grace);
        if (state != WAIT_OBJECT_0) {
            forced = true;
            TerminateProcess(handle.value,90);
            state = WaitForSingleObject(handle.value,10000);
            while (state != WAIT_OBJECT_0) {
                // Do not return ownership while the VM could access pages.
                std::fprintf(stderr,"QEMU_OWNER_STOP_PENDING nativeObjectsRetained=true\n");
                if (state == WAIT_FAILED) Sleep(100);
                state = WaitForSingleObject(handle.value,INFINITE);
            }
        }
        DWORD code = STILL_ACTIVE;
        while (!GetExitCodeProcess(handle.value,&code)) {
            std::fprintf(stderr,"QEMU_OWNER_EXIT_QUERY_PENDING nativeObjectsRetained=true\n");
            Sleep(100);
        }
        exit = code; reaped = true;
    }
    ~OwnedProcess() { stop(0); }
};
}
