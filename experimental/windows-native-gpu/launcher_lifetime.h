// SPDX-License-Identifier: MIT
// Private inherited cancellation capability plus a held actual creator object.
#pragma once
#include "owned_qemu_process.h"
#include "third_party/nlohmann-json/json.hpp"
#include <atomic>
#include <functional>
#include <limits>
#include <thread>

namespace driver_launcher {
enum class End { None, CreatorExited, Cancelled, WaitFailed };
inline const char* reason(End why) {
    switch (why) {
    case End::CreatorExited: return "launcher-exited";
    case End::Cancelled: return "launcher-cancelled";
    case End::WaitFailed: return "wait-failed";
    default: return "none";
    }
}
inline std::uintptr_t parseHandle(const std::string& text) {
    if (text.empty()) throw std::runtime_error("Missing launcher cancellation handle");
    for (const auto c : text) if (c < '0' || c > '9') throw std::runtime_error("Invalid launcher cancellation handle");
    std::size_t used = 0; unsigned long long value = 0;
    try { value = std::stoull(text,&used,10); }
    catch (const std::invalid_argument&) { throw std::runtime_error("Invalid launcher cancellation handle"); }
    catch (const std::out_of_range&) { throw std::runtime_error("Invalid launcher cancellation handle"); }
    constexpr auto maximum = (std::numeric_limits<std::uintptr_t>::max)();
    if (used != text.size() || !value || value > maximum || value == maximum || value == maximum-1)
        throw std::runtime_error("Invalid launcher cancellation handle");
    return static_cast<std::uintptr_t>(value);
}
inline void captureActualCreator(driver_qemu::Handle& parent) {
    if (parent.value) throw std::runtime_error("Launcher creator already captured");
    const auto pid = driver_qemu::creatorPid(GetCurrentProcessId());
    if (!pid || pid == GetCurrentProcessId()) throw std::runtime_error("Invalid launcher creator");
    parent.value = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!parent.value || GetProcessId(parent.value) != pid || WaitForSingleObject(parent.value,0) != WAIT_TIMEOUT ||
        driver_qemu::processCreationTime(parent.value) > driver_qemu::processCreationTime(GetCurrentProcess()) ||
        driver_qemu::creatorPid(GetCurrentProcessId()) != pid)
        throw std::runtime_error("Launcher creator is no longer live");
}
// Take ownership of this process's inherited entry, then keep only wait access.
inline void takeInheritedCancellation(std::uintptr_t raw, driver_qemu::Handle& result) {
    const auto handle = reinterpret_cast<HANDLE>(raw);
    if (result.value || !handle || handle == INVALID_HANDLE_VALUE || handle == GetCurrentThread() ||
        handle == GetStdHandle(STD_INPUT_HANDLE) || handle == GetStdHandle(STD_OUTPUT_HANDLE) || handle == GetStdHandle(STD_ERROR_HANDLE))
        throw std::runtime_error("Invalid inherited cancellation capability");
    DWORD flags = 0;
    if (!GetHandleInformation(handle,&flags) || !(flags & HANDLE_FLAG_INHERIT))
        throw std::runtime_error("Cancellation capability was not inherited");
    driver_qemu::Handle original; original.value = handle;
    if (!DuplicateHandle(GetCurrentProcess(),handle,GetCurrentProcess(),&result.value,SYNCHRONIZE,FALSE,0) ||
        WaitForSingleObject(result.value,0) != WAIT_TIMEOUT)
        throw std::runtime_error("Launcher cancellation already requested or invalid");
}

class Lifetime {
    driver_qemu::Handle creator, request, joinEvent, finished;
    std::thread worker;
    bool started = false, closed = false;
    std::atomic<End> ended{End::None};
    std::atomic<bool> failed{false};
public:
    Lifetime(HANDLE heldCreator, HANDLE cancellation) {
        if (!DuplicateHandle(GetCurrentProcess(),heldCreator,GetCurrentProcess(),&creator.value,
                SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,FALSE,0) ||
            !DuplicateHandle(GetCurrentProcess(),cancellation,GetCurrentProcess(),&request.value,SYNCHRONIZE,FALSE,0))
            throw std::runtime_error("Cannot retain launcher lifetime capabilities");
        requireLive();
        joinEvent.value = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        finished.value = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!joinEvent.value || !finished.value) throw std::runtime_error("Cannot prepare launcher lifetime events");
    }
    Lifetime(const Lifetime&) = delete;
    Lifetime& operator=(const Lifetime&) = delete;
    DWORD creatorId() const { return GetProcessId(creator.value); }
    std::uint64_t creatorTime() const { return driver_qemu::processCreationTime(creator.value); }
    void requireLive() const {
        if (closed || cancelled() || callbackFailed() || !creatorId() || WaitForSingleObject(creator.value,0) != WAIT_TIMEOUT || WaitForSingleObject(request.value,0) != WAIT_TIMEOUT)
            throw std::runtime_error("Launcher exited or cancelled before VM readiness");
    }
    void start(std::function<void(End)> action) {
        if (started || closed || !action) throw std::runtime_error("Invalid launcher lifetime start");
        worker = std::thread([this, action = std::move(action)] {
            const HANDLE waits[]{creator.value,request.value,joinEvent.value};
            const auto result = WaitForMultipleObjects(3,waits,FALSE,INFINITE);
            if (result != WAIT_OBJECT_0+2) {
                const auto why = result == WAIT_OBJECT_0 ? End::CreatorExited : result == WAIT_OBJECT_0+1 ? End::Cancelled : End::WaitFailed;
                ended.store(why,std::memory_order_release);
                try { action(why); } catch (...) { failed.store(true,std::memory_order_release); }
            }
            SetEvent(finished.value);
        });
        started = true;
    }
    End end() const { return ended.load(std::memory_order_acquire); }
    bool cancelled() const { return end() != End::None; }
    bool callbackFailed() const { return failed.load(std::memory_order_acquire); }
    bool wait(DWORD milliseconds) const { return started && WaitForSingleObject(finished.value,milliseconds) == WAIT_OBJECT_0; }
    void cancelAndJoin() {
        closed = true;
        if (joinEvent.value) SetEvent(joinEvent.value);
        if (worker.joinable()) worker.join();
    }
    ~Lifetime() { cancelAndJoin(); }
};

inline nlohmann::json identityMessage(const driver_qemu::ProcessBinding& binding, const Lifetime& lifetime, bool qmpReady) {
    if (!binding.pid || binding.pid == GetCurrentProcessId() || binding.pid == lifetime.creatorId() ||
        !binding.creationTime || binding.creator != GetCurrentProcessId() ||
        lifetime.creatorId() == GetCurrentProcessId() || driver_qemu::creatorPid(GetCurrentProcessId()) != lifetime.creatorId() ||
        !binding.heldProcess || !binding.privateJob || !binding.pinnedImage)
        throw std::runtime_error("VM identity is not bound to this backend");
    lifetime.requireLive();
    return {{qmpReady ? "nativeLauncherVmReady" : "nativeLauncherVmBound",true},{"protocol",1},{"launchMode","backend-owned"},
        {"bridgePid",GetCurrentProcessId()},{"launcherPid",lifetime.creatorId()},
        {"launcherCreationTime100ns",std::to_string(lifetime.creatorTime())},
        {"qemuPid",binding.pid},{"qemuCreatorPid",binding.creator},{"qemuCreationTime100ns",std::to_string(binding.creationTime)},
        {"heldProcessHandle",true},{"privateJobAssigned",true},{"imageFilePinned",true},{"qmpReady",qmpReady},
        {"guestGraphicsVerified",false},{"launcherInputVerified",false},{"dailyUseAccepted",false}};
}
inline nlohmann::json ready(const driver_qemu::ProcessBinding& binding, const Lifetime& lifetime) { return identityMessage(binding,lifetime,true); }
inline nlohmann::json bound(const driver_qemu::ProcessBinding& binding, const Lifetime& lifetime) { return identityMessage(binding,lifetime,false); }
}
