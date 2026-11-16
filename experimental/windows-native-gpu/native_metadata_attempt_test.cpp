// SPDX-License-Identifier: MIT
#include "native_metadata_attempt.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace driver_metadata;
static unsigned checks = 0;
static void require(bool value) { ++checks; if (!value) std::abort(); }
struct Owned {
    unsigned& live;
    explicit Owned(unsigned& value) : live(value) { ++live; }
    ~Owned() { --live; }
};
int main() {
    unsigned live = 0;
    for (const auto failure : {MemoryFailure::BeforeTemplate, MemoryFailure::AfterTemplate}) {
        const auto attempt = attemptBufferMetadata([&]() -> std::vector<std::uint8_t> {
            Owned device(live);
            if (failure == MemoryFailure::BeforeTemplate) requireMetadataBudget(BudgetDecision::QuotaRefused, failure);
            Owned prototype(live);
            requireMetadataBudget(BudgetDecision::QuotaRefused, failure);
            std::abort();
        });
        require(attempt.failure == failure && attempt.bytes.empty() && !attempt.nativeCode && !live);
    }
    for (const auto code : {static_cast<std::int32_t>(0x8007000eu), static_cast<std::int32_t>(0xc0000017u)}) {
        const auto attempt = attemptBufferMetadata([&]() -> std::vector<std::uint8_t> {
            Owned device(live), prototype(live), resource(live);
            checkMetadataMemoryCode(code); std::abort();
        });
        require(attempt.failure == MemoryFailure::NativeMemory && attempt.bytes.empty() && attempt.nativeCode == code && !live);
    }
    auto attempt = attemptBufferMetadata([&]() -> std::vector<std::uint8_t> {
        Owned device(live), prototype(live), resource(live); throw std::bad_alloc();
    });
    require(attempt.failure == MemoryFailure::HostMemory && attempt.bytes.empty() && !live);
    for (const auto stage : {MemoryFailure::BeforeTemplate, MemoryFailure::AfterTemplate}) {
        bool escaped = false;
        try {
            attemptBufferMetadata([&]() -> std::vector<std::uint8_t> {
                Owned device(live); requireMetadataBudget(BudgetDecision::Unavailable, stage); return {};
            });
        } catch (const std::runtime_error&) { escaped = true; }
        require(escaped && !live);
    }
    bool escaped = false;
    try { attemptBufferMetadata([&]() -> std::vector<std::uint8_t> { Owned object(live); throw std::logic_error("Unexpected metadata failure"); }); }
    catch (const std::logic_error&) { escaped = true; }
    require(escaped && !live);
    escaped = false;
    try { requireMetadataBudget(BudgetDecision::QuotaRefused, MemoryFailure::None); }
    catch (const std::logic_error&) { escaped = true; }
    require(escaped);
    for (const auto reason : {MemoryFailure::None, MemoryFailure::NativeMemory, MemoryFailure::AfterTemplate}) {
        escaped = false;
        try {
            attemptBufferMetadata([&]() -> std::vector<std::uint8_t> {
                Owned object(live); throw MemoryUnavailable(reason, static_cast<std::int32_t>(0xc00002b6u));
            });
        } catch (const MemoryUnavailable&) { escaped = true; }
        require(escaped && !live);
    }
    for (const auto code : {0, static_cast<std::int32_t>(0x80004002u), static_cast<std::int32_t>(0xc00002b6u), std::numeric_limits<std::int32_t>::min()})
        checkMetadataMemoryCode(code); // Other errors must stay with their native caller.
    attempt = attemptBufferMetadata([&]() {
        Owned device(live), prototype(live), resource(live);
        requireMetadataBudget(BudgetDecision::Accepted, MemoryFailure::BeforeTemplate);
        requireMetadataBudget(BudgetDecision::Accepted, MemoryFailure::AfterTemplate);
        return std::vector<std::uint8_t>{1, 2, 3, 4};
    });
    require(attempt.failure == MemoryFailure::None && !attempt.nativeCode && attempt.bytes == std::vector<std::uint8_t>({1,2,3,4}) && !live);
    std::printf("NATIVE_METADATA_ATTEMPT_PASS checks=%u memoryOnly=true missingBudgetFatal=true ownershipUnwoundBeforeReturn=true retry=true gpu=false\n", checks);
}
