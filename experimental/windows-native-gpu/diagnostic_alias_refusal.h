// SPDX-License-Identifier: MIT
// Test-only state: one refusal after a real accepted-frame threshold.
#pragma once
#include <cstdint>
#include <cerrno>
#include <stdexcept>
#include <system_error>

namespace guest_broker {
class DiagnosticAliasRefusalError final : public std::system_error {
public:
    DiagnosticAliasRefusalError():std::system_error(ENOMEM,std::generic_category(),"Diagnostic pre-native alias capacity refusal") {}
};
class DiagnosticAliasRefusal {
    const bool primary;
    const std::uint64_t threshold;
    std::uint64_t completed=0;
    bool used=false;
public:
    DiagnosticAliasRefusal(bool presentationOwner,std::uint64_t frame):primary(presentationOwner),threshold(frame) {
        if(!frame || frame>1700) throw std::invalid_argument("Diagnostic alias refusal requires a threshold in 1..1700");
    }
    bool acceptedNativeFrame(std::uint64_t frame) {
        if(!primary) return false;
        if(!frame || frame>1800 || frame!=completed+1) throw std::runtime_error("Diagnostic native frame acknowledgements are not sequential");
        completed=frame;
        return frame==threshold;
    }
    bool refuseResourceAlias(bool resource) {
        if(!primary || !resource || used || completed<threshold) return false;
        used=true;return true;
    }
    std::uint64_t completedFrames() const { return completed; }
};
}
