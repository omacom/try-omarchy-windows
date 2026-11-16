// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstddef>
namespace gpu_pressure {
constexpr unsigned HeapCount=28,PassCount=2,Blocks=3;
constexpr std::uint64_t HeapBytes=48ull*1024*1024;
constexpr std::size_t BlockBytes=4096,TransferBytes=Blocks*BlockBytes;
constexpr std::uint64_t offsets[Blocks]={0,HeapBytes/2,HeapBytes-BlockBytes};
inline unsigned char byte(unsigned pass,unsigned heap,unsigned block,std::size_t position) {
    return static_cast<unsigned char>(heap*73u+pass*151u+block*29u+((position*17u)^(position>>4)));
}
inline std::uint64_t hash(const unsigned char* bytes,std::size_t count) {
    std::uint64_t result=14695981039346656037ull;
    for(std::size_t n=0;n<count;++n) result=(result^bytes[n])*1099511628211ull;
    return result;
}
}
