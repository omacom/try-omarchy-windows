// SPDX-License-Identifier: MIT
// Reuse the hidden child fixture; the original ownership test runs separately.
#define main owned_qemu_fixture_main
#include "owned_qemu_process_test.cpp"
#undef main
#include "launcher_lifetime.h"
using driver_launcher::End;
using driver_launcher::Lifetime;

static void event(Handle& result, bool inherited = false, bool signalled = false) {
    SECURITY_ATTRIBUTES security{sizeof security,nullptr,inherited ? TRUE : FALSE};
    result.value = CreateEventW(&security,TRUE,signalled ? TRUE : FALSE,nullptr);
    if (!result.value) throw std::runtime_error("Cannot create private test cancellation event");
}
static void attach(Child& child, OwnedProcess& owner) {
    owner.adoptVerified(child.guard.handle.value,GetCurrentProcess(),child.claim(),self());
    child.originalHandleTransferred(); child.resume();
}
template<class Call> static void invalid(Call&& call) {
    bool failed = false; try { call(); } catch (const std::exception&) { failed = true; }
    require(failed,"Malformed lifetime operation was accepted");
}
int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--owned-child") return owned_qemu_fixture_main(argc,argv);
    try {
        require(argc == 1,"Unexpected lifetime-test arguments");
        for (const auto text : {"", "0", "-1", "+1", "0x10", "1x", "18446744073709551615", "18446744073709551614", "18446744073709551616"})
            invalid([&]{driver_launcher::parseHandle(text);});
        require(driver_launcher::parseHandle("1234") == 1234,"Valid capability integer changed");
        {
            Handle token, copy, signal;
            event(token,true);
            require(DuplicateHandle(GetCurrentProcess(),token.value,GetCurrentProcess(),&signal.value,0,FALSE,DUPLICATE_SAME_ACCESS) != FALSE,"Cannot retain signal authority");
            const auto raw = token.value; token.value = nullptr;
            driver_launcher::takeInheritedCancellation(reinterpret_cast<std::uintptr_t>(raw),copy);
            DWORD flags = HANDLE_FLAG_INHERIT;
            require(GetHandleInformation(copy.value,&flags) && !(flags & HANDLE_FLAG_INHERIT),"Retained capability remained inheritable");
            require(!SetEvent(copy.value) && WaitForSingleObject(copy.value,0) == WAIT_TIMEOUT,"Wait capability retained signal authority");
            require(SetEvent(signal.value) && WaitForSingleObject(copy.value,5000) == WAIT_OBJECT_0,"Creator lost cancellation authority");
        }
        {
            Handle token, copy; event(token);
            invalid([&]{driver_launcher::takeInheritedCancellation(reinterpret_cast<std::uintptr_t>(token.value),copy);});
            require(WaitForSingleObject(token.value,0) == WAIT_TIMEOUT,"Rejected non-inherited entry changed its source");
        }
        {
            Handle token; event(token,false,true);
            invalid([&]{Lifetime alreadyCancelled(GetCurrentProcess(),token.value);});
        }
        {
            Child target; OwnedProcess owner; attach(target,owner);
            Handle token; event(token);
            std::atomic<unsigned> callbacks{0};
            Lifetime lifetime(GetCurrentProcess(),token.value);
            lifetime.start([&](End why) { if (why != End::Cancelled) throw std::runtime_error("Wrong cancellation reason"); owner.stop(0); ++callbacks; });
            invalid([&]{lifetime.start([](End){});});
            require(SetEvent(token.value) && lifetime.wait(5000),"Cancellation callback did not complete");
            require(lifetime.end() == End::Cancelled && !lifetime.callbackFailed() && owner.reaped && owner.forced && owner.exit == 90,"Cancellation did not reap its held child");
            lifetime.cancelAndJoin(); lifetime.cancelAndJoin();
            require(callbacks == 1,"Cancellation callback ran more than once");
            invalid([&]{lifetime.requireLive();});
        }
        {
            Child creator(true), target; OwnedProcess owner; attach(target,owner);
            Handle token; event(token);
            Lifetime lifetime(creator.guard.handle.value,token.value);
            lifetime.start([&](End why) { if (why != End::CreatorExited) throw std::runtime_error("Wrong creator exit reason"); owner.stop(0); });
            creator.resume();
            require(lifetime.wait(5000) && lifetime.end() == End::CreatorExited && !lifetime.callbackFailed(),"Held creator exit was not observed");
            require(owner.reaped && owner.forced && owner.exit == 90,"Creator exit did not reap owned target");
            invalid([&]{lifetime.requireLive();});
        }
        {
            Child target; OwnedProcess owner; attach(target,owner);
            Handle token; event(token);
            std::atomic<unsigned> callbacks{0};
            Lifetime lifetime(GetCurrentProcess(),token.value);
            lifetime.start([&](End){ ++callbacks; }); lifetime.cancelAndJoin();
            require(callbacks == 0 && !lifetime.cancelled() && !owner.exited(),"Joining monitor changed live guest state");
            invalid([&]{lifetime.start([](End){});});
            owner.stop(0);
        }
        {
            Handle token; event(token);
            Lifetime lifetime(GetCurrentProcess(),token.value);
            lifetime.start([](End){throw std::runtime_error("Controlled callback failure");});
            require(SetEvent(token.value) && lifetime.wait(5000) && lifetime.callbackFailed(),"Callback failure was hidden");
            invalid([&]{lifetime.requireLive();});
        }
        {
            Handle creator, token; driver_launcher::captureActualCreator(creator); event(token);
            require(GetProcessId(creator.value) == creatorPid(GetCurrentProcessId()),"Actual creator capture differs");
            Lifetime lifetime(creator.value,token.value);
            Child target; OwnedProcess owner; attach(target,owner);
            const auto binding = owner.binding();
            const auto message = driver_launcher::ready(binding,lifetime);
            require(message["qemuPid"] == binding.pid && message["bridgePid"] == GetCurrentProcessId() &&
                message["launcherPid"] == GetProcessId(creator.value) && message["qemuCreationTime100ns"].is_string() &&
                message["qemuCreationTime100ns"] == std::to_string(binding.creationTime),"Ready message lost kernel identity or precision");
            require(message["dailyUseAccepted"] == false && message["guestGraphicsVerified"] == false && message["launcherInputVerified"] == false,"Ready message promoted product acceptance");
            auto bad = binding; bad.privateJob = false;
            invalid([&]{driver_launcher::ready(bad,lifetime);});
            bad = binding; bad.pid = GetCurrentProcessId(); invalid([&]{driver_launcher::ready(bad,lifetime);});
            require(SetEvent(token.value) != FALSE,"Cannot set pre-ready cancellation");
            invalid([&]{driver_launcher::ready(binding,lifetime);});
            owner.stop(0);
        }
        std::cout << "LAUNCHER_LIFETIME_PASS checks=" << checks << " cancelledChildReaped=true creatorExitWait=true"
            " waitOnlyCapability=true joinPreservesGuest=true readyIdentityExact=true callbackFailureReported=true"
            " qemu=false gpu=false input=false windows=0\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "LAUNCHER_LIFETIME_FAIL " << e.what() << '\n'; return 1; }
}
