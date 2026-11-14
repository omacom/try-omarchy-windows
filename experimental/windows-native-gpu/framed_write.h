// SPDX-License-Identifier: MIT
// One logical length-prefixed reply, with scatter/gather and partial-write safety.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace driver_transport {
// Winsock may return a boolean option in one byte even when given a DWORD.
// The untouched bytes start at zero; validate the returned extent and value.
inline bool tcpNoDelayEnabled(std::uint32_t value,int returnedBytes) noexcept {
    return returnedBytes>=1 && returnedBytes<=static_cast<int>(sizeof value) && value!=0;
}
struct WriteBuffer { const char* bytes;std::size_t size; };
template<class Send>
void writeFrame(const void* payload,std::size_t bytes,Send&& send) {
    if(!payload || !bytes || bytes>std::numeric_limits<std::uint32_t>::max() ||
       bytes>std::numeric_limits<std::size_t>::max()-sizeof(std::uint32_t))
        throw std::invalid_argument("Invalid framed reply size");
    // Preserve the existing native little-endian wire prefix without a copy
    // of the reply payload. Sender completion must be synchronous.
    const auto length=static_cast<std::uint32_t>(bytes);
    std::array<WriteBuffer,2> buffers{{{reinterpret_cast<const char*>(&length),sizeof length},
                                     {static_cast<const char*>(payload),bytes}}};
    std::size_t first=0,remaining=bytes+sizeof length;
    while(remaining) {
        auto written=send(buffers.data()+first,buffers.size()-first);
        if(!written || written>remaining) throw std::runtime_error("Incomplete framed reply write");
        remaining-=written;
        while(written) {
            auto& buffer=buffers[first];
            if(written<buffer.size) { buffer.bytes+=written;buffer.size-=written;break; }
            written-=buffer.size;++first;
        }
    }
}
}
