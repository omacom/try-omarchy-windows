// SPDX-License-Identifier: MIT
#include "native_buffer_admission_reply.h"
#include <cstdio>
#include <cstdlib>
using namespace driver_bridge;
static unsigned checks = 0;
static void require(bool value) { ++checks; if (!value) std::abort(); }
static std::vector<std::uint8_t> packet(std::int32_t transport, std::int32_t native = 0) {
    const Header header{static_cast<std::uint32_t>(Op::QueryNativeGpuBufferAdmission), 0, transport, 0};
    std::vector<std::uint8_t> out(sizeof header); std::memcpy(out.data(), &header, sizeof header);
    if (!transport) { const Reply value{native, 0, 0}; out.resize(sizeof header + sizeof value); std::memcpy(out.data()+sizeof header, &value, sizeof value); }
    return out;
}
int main() {
    for (const auto status : {0, static_cast<std::int32_t>(0xc0000017u), static_cast<std::int32_t>(0xc0000001u)}) {
        const auto result = parseNativeBufferAdmissionReply(packet(0,status));
        require(result.supported && !result.transportStatus && result.nativeStatus == status);
    }
    for (const auto transport : {-95, -38}) require(!parseNativeBufferAdmissionReply(packet(transport)).supported);
    require(!parseNativeBufferAdmissionReply(packet(0,static_cast<std::int32_t>(0xc00000bbu))).supported);
    require(parseNativeBufferAdmissionReply(packet(-12)).transportStatus == -12);
    for (unsigned n = 0; n < 10; ++n) {
        auto bad = packet(n == 9 ? -95 : 0);
        if (n == 0) bad.resize(sizeof(Header)-1);
        if (n == 1) bad.push_back(0);
        if (n == 2) bad.pop_back();
        if (n == 3) bad[0] ^= 1;
        if (n == 4 || n == 9) bad[4] = 1; // A malformed unsupported reply cannot enable fallback.
        if (n == 5) bad[12] = 1;
        if (n == 6) bad[sizeof(Header)+4] = 1;
        if (n == 7) bad[sizeof(Header)+8] = 1;
        if (n == 8) bad[sizeof(Header)] = 1;
        bool rejected = false;
        try { parseNativeBufferAdmissionReply(bad); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected);
    }
    std::printf("NATIVE_BUFFER_ADMISSION_REPLY_PASS checks=%u malformedFallbackRejected=true nativeMemoryStatusPreserved=true gpu=false\n",checks);
}
