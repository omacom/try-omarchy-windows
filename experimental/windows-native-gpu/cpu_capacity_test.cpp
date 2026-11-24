// SPDX-License-Identifier: MIT
// Reproduce virtual-slot exhaustion with small UMD views plus a full 4K map.
#include "cpu_aperture.h"
#include <iostream>
#include <vector>
static void require(bool value) { if (!value) throw std::runtime_error("CPU virtual capacity regression"); }
int main() {
    using namespace driver_bridge;
    static_assert(gpu_diagnostic::HighResolution, "This regression needs the explicit 4K profile");
    static_assert(ExpandedVendorCpuBytes == 96u * 1024 * 1024, "Do not raise the physical byte budget");
    static_assert(MaxVendorCpuMappingBytes == 64u * 1024 * 1024, "Keep the per-view bound");
    constexpr std::uint32_t imageBytes = 3840u * 2160 * 4;
    unsigned stops = 0;
    auto stop = [&] { ++stops; };
    auto map = [](std::uint32_t, std::uint64_t, std::uint32_t, std::uint64_t) {};
    auto unmap = [](std::uint32_t, std::uint64_t) {};
    for (const auto slots : {128u, 256u}) {
        driver_cpu::Aperture aperture(slots);
        std::vector<driver_cpu::Lease> small;
        for (unsigned n = 0; n < 100; ++n) small.push_back(aperture.map(4096, map, stop));
        require(aperture.bytes() == 100 * 4096 && aperture.chunks() == 100);
        require(vendorCpuBudgetFits(aperture.bytes(), imageBytes, ExpandedVendorCpuCapability));
        require(aperture.canMap(imageBytes) == (slots == 256));
        if (slots == 256) {
            const auto image = aperture.map(imageBytes, map, stop);
            require(image.slot == 100 && aperture.chunks() == 132);
            require(aperture.bytes() == imageBytes + 100 * 4096);
            require(validVendorCpuReply(image.offset, {image.bytes, 0, image.generation}));
            aperture.release(image, unmap, stop);
            const auto reused = aperture.map(imageBytes, map, stop);
            require(reused.slot == image.slot && reused.generation > image.generation);
            aperture.release(reused, unmap, stop);
        }
        for (const auto view : small) aperture.release(view, unmap, stop);
        require(!aperture.bytes() && !aperture.groups() && aperture.chunkMaps() == aperture.chunkUnmaps());
    }
    require(!vendorCpuBudgetFits(ExpandedVendorCpuBytes - 4096, 8192, ExpandedVendorCpuCapability));
    require(!validVendorCpuSlots(255) && !validVendorCpuSlots(257) && !validVendorCpuSlots(512));
    // Reproduce the measured live-4K failure: 226 occupied slots leave fewer
    // than the 32 required by the next output-sized native allocation.
    for(const auto slots:{256u,512u}) {
        driver_cpu::Aperture aperture(slots);std::vector<driver_cpu::Lease> held;
        for(unsigned n=0;n<226;++n) held.push_back(aperture.map(4096,map,stop));
        require(aperture.chunks()==226 && aperture.canMap(imageBytes)==(slots==512));
        if(slots==512) {
            const auto image=aperture.map(imageBytes,map,stop);require(image.slot==226 && aperture.chunks()==258);
            require(validVendorCpuReply(image.offset,{image.bytes,0,image.generation},true));
            require(!validVendorCpuReply(image.offset,{image.bytes,0,image.generation}));
            aperture.release(image,unmap,stop);
            // Exercise the last sparse slot and deny stale lease retirement.
            for(unsigned n=226;n<512;++n) held.push_back(aperture.map(4096,map,stop));
            require(held.back().slot==511 && !aperture.canMap(4096));
            const auto last=held.back();aperture.release(last,unmap,stop);held.pop_back();
            const auto reused=aperture.map(4096,map,stop);require(reused.slot==511 && reused.generation>last.generation);
            bool stale=false;try { aperture.release(last,unmap,stop); } catch(const std::runtime_error&) { stale=true; }
            require(stale && aperture.groups()==512);aperture.release(reused,unmap,stop);
        }
        for(const auto view:held) aperture.release(view,unmap,stop);
        require(!aperture.bytes() && !aperture.groups() && aperture.chunkMaps()==aperture.chunkUnmaps());
    }
    // Glyph uploads can fill virtual slots with small views without reaching
    // the mapped-byte limit. Only the separately authorized layout admits more.
    require(!validVendorCpuSlots(1024,true) && !validVendorCpuSlots(1024,false,true) && validVendorCpuSlots(1024,true,true));
    bool unnegotiated=false;
    try { driver_cpu::Aperture denied(1024); } catch(const std::runtime_error&) { unnegotiated=true; }
    require(unnegotiated);
    for(const auto slots:{512u,1024u}) {
        driver_cpu::Aperture aperture(slots,slots==1024);std::vector<driver_cpu::Lease> held;
        for(unsigned n=0;n<512;++n) held.push_back(aperture.map(4096,map,stop));
        require(aperture.bytes()==2u*1024*1024 && aperture.canMap(imageBytes)==(slots==1024));
        if(slots==1024) {
            const auto image=aperture.map(imageBytes,map,stop);
            require(image.slot==512 && validVendorCpuReply(image.offset,{image.bytes,0,image.generation},true,true));
            require(!validVendorCpuReply(image.offset,{image.bytes,0,image.generation},true));
            aperture.release(image,unmap,stop);
            for(unsigned n=512;n<1024;++n) held.push_back(aperture.map(4096,map,stop));
            const auto last=held.back();require(last.slot==1023 && !aperture.canMap(4096));
            require(validVendorCpuReply(last.offset,{4096,0,last.generation},true,true));
            require(!validVendorCpuReply(1024ull*VendorCpuSlotBytes,{4096,0,last.generation},true,true));
            aperture.release(last,unmap,stop);held.pop_back();
            const auto reused=aperture.map(4096,map,stop);require(reused.slot==1023 && reused.generation>last.generation);
            bool stale=false;try { aperture.release(last,unmap,stop); } catch(const std::runtime_error&) { stale=true; }
            require(stale && aperture.groups()==1024);aperture.release(reused,unmap,stop);
        }
        for(const auto view:held) aperture.release(view,unmap,stop);
        require(!aperture.groups() && !aperture.bytes() && aperture.chunkMaps()==aperture.chunkUnmaps());
    }
    // Reproduce run 141's aggregate CPU admission failure. The separate profile
    // admits another bounded view without changing any older capacity receipt.
    require(!vendorCpuBudgetFits(201293824,65536,ExpandedVendorCpuCapability,true));
    require(vendorCpuBudgetFits(201293824,65536,ExpandedVendorCpuCapability,true,true));
    require(vendorCpuByteLimit(ExpandedVendorCpuCapability,true)==192u*1024*1024);
    require(vendorCpuByteLimit(ExpandedVendorCpuCapability,true,true)==384u*1024*1024);
    require(vendorCpuByteLimit(ExpandedVendorCpuCapability,false,true)==ExpandedVendorCpuBytes);
    require(vendorCpuByteLimit(0,true,true)==MaxAllocatedBytes);
    require(vendorCpuBudgetFits(OpenGlMultiappCpuBytes-4096,4096,ExpandedVendorCpuCapability,true,true));
    require(!vendorCpuBudgetFits(OpenGlMultiappCpuBytes-4096,8192,ExpandedVendorCpuCapability,true,true));
    require(!vendorCpuBudgetFits(UINT32_MAX,4096,ExpandedVendorCpuCapability,true,true));
    require(!vendorCpuBudgetFits(4096,UINT32_MAX,ExpandedVendorCpuCapability,true,true));
    require(validOpenGlMultiappCapacity(GuestOpenGlMultiappCapacity));
    require(validOpenGlMultiappProfile(GuestOpenGlMultiappProfile));
    auto mixed=GuestOpenGlMultiappCapacity;mixed.mapping.aperture.cpuByteLimit=OpenGlDesktop4kCpuBytes;
    require(!validCoordinatedPresentationCapacity(mixed));
    mixed=GuestOpenGlDesktopReferenceCapacity;mixed.mapping.aperture.cpuByteLimit=OpenGlMultiappCpuBytes;
    require(!validCoordinatedPresentationCapacity(mixed));
    for(unsigned word=0;word<12;++word) {
        auto changed=GuestOpenGlMultiappCapacity;std::uint32_t words[12]{};
        std::memcpy(words,&changed,sizeof words);++words[word];std::memcpy(&changed,words,sizeof words);
        require(!validOpenGlMultiappCapacity(changed));
    }
    require(!stops);
    std::cout << "PASS: 256 virtual slots fit full 4K map beside small views; unchanged legacy 96 MiB CPU budget; explicit512-slot boundary, reuse and stale-owner controls\n";
}
