// SPDX-License-Identifier: MIT
// Real hidden helper children only: no QEMU, GPU, windows or host input.
#include "owned_qemu_process.h"
#include <filesystem>
#include <iostream>
using namespace driver_qemu;
static unsigned checks = 0;
static void require(bool value, const char* why) { ++checks; if (!value) throw std::runtime_error(why); }
static std::wstring self() {
    std::vector<wchar_t> text(32768);
    const auto count = GetModuleFileNameW(nullptr,text.data(),static_cast<DWORD>(text.size()));
    if (!count || count >= text.size()) throw std::runtime_error("Cannot find helper executable");
    return std::wstring(text.data(),count);
}
struct Child {
    OwnedProcess guard;
    Handle thread, started;
    DWORD pid = 0;
    explicit Child(bool natural = false) {
        SECURITY_ATTRIBUTES security{sizeof security,nullptr,TRUE};
        started.value = CreateEventW(&security,TRUE,FALSE,nullptr);
        if (!started.value) throw std::runtime_error("Cannot create helper readiness event");
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
        auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(),0,size));
        const bool initialized = attributes && InitializeProcThreadAttributeList(attributes,1,0,&size);
        HANDLE handles[]{started.value};
        const bool ready = initialized && UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof handles,nullptr,nullptr);
        const auto executable = self();
        std::wstring command = L"\"" + executable + L"\" --owned-child " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(started.value)) + (natural ? L" natural" : L" wait");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        const bool launched = ready && CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&startup.StartupInfo,&child);
        if (initialized) DeleteProcThreadAttributeList(attributes);
        if (attributes) HeapFree(GetProcessHeap(),0,attributes);
        if (!launched) throw std::runtime_error("Cannot create hidden helper child");
        guard.handle.value = child.hProcess; thread.value = child.hThread; pid = child.dwProcessId;
    }
    ProcessClaim claim() const { return {pid,processCreationTime(guard.handle.value)}; }
    void resume() {
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1) || WaitForSingleObject(started.value,5000) != WAIT_OBJECT_0)
            throw std::runtime_error("Helper did not acknowledge startup");
    }
    void originalHandleTransferred() {
        const auto original = guard.handle.value; guard.handle.value = nullptr; CloseHandle(original);
    }
};
template<class Call> static void rejectedWithoutMutation(Child& child, OwnedProcess& owner, Call&& call) {
    bool rejected = false;
    try { call(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected,"Invalid claim was accepted");
    require(!owner.handle.value && !owner.reaped && !owner.forced,"Rejected claim changed owner state");
    require(WaitForSingleObject(child.guard.handle.value,0) == WAIT_TIMEOUT,"Rejected claim stopped candidate");
}
int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--owned-child") {
        const auto event = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(argv[2])));
        if (!SetEvent(event)) return 2;
        if (std::string(argv[3]) == "wait") Sleep(30000); // self-bounded even if an external test runner dies.
        return 0;
    }
    try {
        require(argc == 1,"Unexpected ownership-test arguments");
        FILE_ID_INFO first{}, other{};
        require(!sameImageIdentity(first,other),"Unavailable file IDs compared equal");
        first.VolumeSerialNumber = 1; first.FileId.Identifier[0] = 1; other = first;
        require(sameImageIdentity(first,other),"Known file identity did not match");
        other.VolumeSerialNumber = 2;
        require(!sameImageIdentity(first,other),"Different volumes compared equal");
        other = first; other.FileId.Identifier[0] = 2;
        require(!sameImageIdentity(first,other),"Different file IDs compared equal");
        other = {};
        require(!sameImageIdentity(first,other),"Unavailable peer identity accepted");
        Child child;
        const auto claim = child.claim();
        const auto image = self();
        OwnedProcess owner;
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),{0,claim.creationTime},image);});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),{claim.pid,0},image);});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),{claim.pid+1,claim.creationTime},image);});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),{claim.pid,claim.creationTime+1},image);});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,child.guard.handle.value,claim,image);});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(nullptr,GetCurrentProcess(),claim,image);});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),claim,std::filesystem::absolute(__FILE__).wstring());});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),claim,image + std::wstring(L"\0suffix",7));});
        rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(GetCurrentProcess(),child.guard.handle.value,
            {GetCurrentProcessId(),processCreationTime(GetCurrentProcess())},image);});
        {
            Child deadCreator(true); deadCreator.resume(); deadCreator.guard.stop(5000);
            require(deadCreator.guard.reaped && !deadCreator.guard.forced && deadCreator.guard.exit == 0,"Natural control did not exit");
            rejectedWithoutMutation(child,owner,[&]{owner.adoptVerified(child.guard.handle.value,deadCreator.guard.handle.value,claim,image);});
        }
        {
            OwnedProcess cancelled; cancelled.stop(0);
            rejectedWithoutMutation(child,cancelled,[&]{cancelled.adoptVerified(child.guard.handle.value,GetCurrentProcess(),claim,image);});
        }
        owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),claim,image);
        require(owner.handle.value && GetProcessId(owner.handle.value) == claim.pid,"Verified process object not retained");
        DWORD flags = 0;
        require(GetHandleInformation(owner.handle.value,&flags) && !(flags & HANDLE_FLAG_INHERIT),"Owned process handle leaked inheritance");
        bool duplicate = false;
        try { owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),claim,image); } catch (const std::runtime_error&) { duplicate = true; }
        require(duplicate && !owner.reaped && !owner.forced,"Duplicate attach changed live ownership");
        child.originalHandleTransferred();
        require(!owner.exited(),"Independent held process object was lost");
        child.resume();
        owner.stop(0);
        require(owner.reaped && owner.forced && owner.exit == 90 && owner.exited(),"Forced own-child stop was not reaped");
        owner.stop(0);
        require(owner.reaped && owner.forced && owner.exit == 90,"Idempotent stop changed its result");
        {
            Child natural(true); OwnedProcess owned;
            const auto expected = natural.claim();
            owned.adoptVerified(natural.guard.handle.value,GetCurrentProcess(),expected,image);
            natural.originalHandleTransferred(); natural.resume(); owned.stop(5000);
            require(owned.reaped && !owned.forced && owned.exit == 0 && owned.exited(),"Adopted natural child did not exit cleanly");
        }
        {
            Child dead(true); dead.resume(); dead.guard.stop(5000);
            OwnedProcess empty;
            bool rejected = false;
            try { empty.adoptVerified(dead.guard.handle.value,GetCurrentProcess(),dead.claim(),image); } catch (const std::runtime_error&) { rejected = true; }
            require(rejected && !empty.handle.value && !empty.reaped,"Exited candidate was accepted");
        }
        std::cout << "OWNED_QEMU_PROCESS_PASS checks=" << checks
            << " rejectedCandidatesUntouched=true heldIdentity=true naturalExit=true forcedExit=true"
               " duplicateAttachRejected=true cancelledAttachRejected=true qemu=false gpu=false input=false windows=0\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "OWNED_QEMU_PROCESS_FAIL " << e.what() << '\n'; return 1; }
}
