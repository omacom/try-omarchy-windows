// SPDX-License-Identifier: MIT
// Link against the actual patched Aquamarine library. No GPU/seat is opened.
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Session.hpp>
#include <cstdio>
#include <cstdlib>

int main() {
    if (setenv("LIBSEAT_BACKEND", "wddm-lifetime-test-invalid-seat", 1))
        return 1;
    unsigned logCalls = 0;
    Aquamarine::SBackendOptions options;
    options.logFunction = [&](Aquamarine::eBackendLogLevel, std::string) { ++logCalls; };
    Aquamarine::SBackendImplementationOptions implementation;
    implementation.backendType = Aquamarine::AQ_BACKEND_HEADLESS;
    implementation.backendRequestMode = Aquamarine::AQ_BACKEND_REQUEST_MANDATORY;
    auto backend = Aquamarine::CBackend::create({implementation}, options);
    if (!backend)
        return 2;
    Hyprutils::Memory::CWeakPointer<Aquamarine::CBackend> observer = backend;
    if (Aquamarine::CSession::attempt(backend) || !logCalls)
        return 3;
    backend.reset();
    if (observer.lock()) {
        std::fputs("AQ_LOGGING_LIFETIME retainedBackend=true\n", stderr);
        return 4;
    }
    std::puts("AQ_LOGGING_LIFETIME rejectedSeat=true callbacksObserved=true retainedBackend=false");
    return 0;
}
