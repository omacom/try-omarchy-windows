// SPDX-License-Identifier: MIT
// Only known, fully unwound memory refusals become a recoverable allocation
// result. Missing budget observations and unexpected exceptions remain errors.
#pragma once
#include <cstdint>
#include <exception>
#include <new>
#include <stdexcept>
#include <vector>

namespace driver_metadata {
enum class BudgetDecision { Accepted, QuotaRefused, Unavailable };
enum class MemoryFailure { None, BeforeTemplate, AfterTemplate, NativeMemory, HostMemory };
class MemoryUnavailable : public std::exception {
public:
    const MemoryFailure failure;
    const std::int32_t nativeCode;
    explicit MemoryUnavailable(MemoryFailure reason, std::int32_t code = 0) noexcept : failure(reason), nativeCode(code) {}
    const char* what() const noexcept override { return "Native metadata memory unavailable"; }
};
inline void requireMetadataBudget(BudgetDecision decision, MemoryFailure stage) {
    if (stage != MemoryFailure::BeforeTemplate && stage != MemoryFailure::AfterTemplate)
        throw std::logic_error("Invalid metadata budget stage");
    if (decision == BudgetDecision::Accepted) return;
    if (decision == BudgetDecision::QuotaRefused) throw MemoryUnavailable(stage);
    throw std::runtime_error("Native metadata budget observation unavailable");
}
inline bool isMetadataMemoryCode(std::int32_t code) {
    return code == static_cast<std::int32_t>(0x8007000eu) || code == static_cast<std::int32_t>(0xc0000017u);
}
inline void checkMetadataMemoryCode(std::int32_t code) {
    // Documented HRESULT E_OUTOFMEMORY and NTSTATUS STATUS_NO_MEMORY. Do not
    // reinterpret device removal, access errors, or unknown native failures.
    if (isMetadataMemoryCode(code)) throw MemoryUnavailable(MemoryFailure::NativeMemory, code);
}
struct MetadataAttempt {
    std::vector<std::uint8_t> bytes;
    MemoryFailure failure;
    std::int32_t nativeCode;
};
template<class Generate>
inline MetadataAttempt attemptBufferMetadata(Generate&& generate) {
    try { return {generate(), MemoryFailure::None, 0}; }
    // Generate's native ownership scopes must finish unwinding before these
    // failures are returned. Uncertain native cleanup remains fail-stop.
    catch (const MemoryUnavailable& error) {
        const bool known = error.failure == MemoryFailure::NativeMemory ? isMetadataMemoryCode(error.nativeCode) :
            (error.failure == MemoryFailure::BeforeTemplate || error.failure == MemoryFailure::AfterTemplate ||
             error.failure == MemoryFailure::HostMemory) && !error.nativeCode;
        if (!known) throw;
        return {{}, error.failure, error.nativeCode};
    }
    catch (const std::bad_alloc&) { return {{}, MemoryFailure::HostMemory, 0}; }
}
inline const char* memoryFailureName(MemoryFailure failure) {
    switch (failure) {
    case MemoryFailure::BeforeTemplate: return "before-template";
    case MemoryFailure::AfterTemplate: return "after-template";
    case MemoryFailure::NativeMemory: return "native-memory";
    case MemoryFailure::HostMemory: return "host-memory";
    default: return "none";
    }
}
}
