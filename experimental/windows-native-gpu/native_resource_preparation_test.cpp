// SPDX-License-Identifier: MIT
#include "native_resource_preparation.h"
#include <iostream>
#include <stdexcept>
using namespace driver_bridge;
static unsigned checks=0;
static void require(bool good) { ++checks; if (!good) throw std::runtime_error("Native resource preparation regression"); }
int main() {
    try {
        const NativeTextureDesc texture{640,360,87,1};
        const std::map<std::uint32_t,std::uint32_t> devices{{20,1},{30,1}};
        NativeResourcePreparations p;
        require(!p.stage(0,texture,devices) && !p.stage(1,texture,{}) && p.empty());
        require(!p.stage(1,{0,360,87,1},devices) && !p.stage(1,texture,{{0,1}}) && !p.stage(1,texture,{{20,0}}));
        // The renderer's device need not be the first allocator device.
        require(p.stage(1,texture,devices));
        require(!p.stage(1,{1280,720,28,1},devices));
        auto result=p.take(1,30,devices);
        require(result && result->width==640 && result->height==360 && result->format==87 && p.empty());
        require(!p.take(1,30,devices));
        // Another thread cannot consume the declaration or cancel its owner.
        require(p.stage(1,texture,devices)); p.cancel(2);
        require(!p.take(2,30,devices) && p.contains(1));
        require(p.stage(2,{1280,720,28,1},devices));
        result=p.take(2,20,devices); require(result && result->width==1280 && p.contains(1));
        p.cancel(1); require(p.empty());
        // Foreign and subsequently created devices fail and consume the attempt.
        require(p.stage(1,texture,devices)); require(!p.take(1,99,devices) && p.empty());
        require(p.stage(1,texture,devices)); auto added=devices; added.emplace(40,1);
        require(!p.take(1,40,added) && p.empty());
        // A destroyed device, reused numeric ID, or changed adapter is excluded.
        require(p.stage(1,texture,devices)); p.destroyDevice(30);
        require(!p.take(1,30,devices) && p.empty());
        require(p.stage(1,texture,devices)); auto missing=devices; missing.erase(30);
        require(!p.take(1,30,missing) && p.empty());
        require(p.stage(1,texture,devices)); auto moved=devices; moved[30]=2;
        require(!p.take(1,30,moved) && p.empty());
        // Releasing one candidate leaves a different live device usable.
        require(p.stage(1,texture,devices)); p.destroyDevice(20);
        result=p.take(1,30,devices); require(result && p.empty());
        require(p.stage(1,texture,{{20,1}})); p.destroyDevice(20); require(p.empty());
        for(std::uint64_t thread=1;thread<=16;++thread) require(p.stage(thread,texture,devices));
        require(!p.stage(17,texture,devices)); p.cancel(8);
        require(p.stage(17,texture,devices)); p.destroyDevice(20); p.destroyDevice(30); require(p.empty());
        std::map<std::uint32_t,std::uint32_t> oversized;
        for(std::uint32_t n=1;n<=513;++n) oversized.emplace(n,1);
        require(!p.stage(1,texture,oversized) && p.empty());
        std::cout<<"PASS: "<<checks<<" native resource preparation checks; multiple owned devices, thread binding, snapshot lifetime, one-shot failures and quota\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
