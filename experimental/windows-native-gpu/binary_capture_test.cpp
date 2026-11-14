// SPDX-License-Identifier: MIT
// File lifecycle/bounds test only. No GPU rendering or driver access.
#include "binary_capture.h"
#include <fstream>
#include <vector>
#include <cstdlib>
static void require(bool ok) { if (!ok) std::abort(); }
int main() {
    constexpr unsigned rowBytes = gpu_diagnostic::Width * 4;
    constexpr unsigned pitch = (rowBytes + 255u) & ~255u;
    std::vector<std::uint8_t> mapped(512 + std::size_t(pitch) * gpu_diagnostic::Height, 0xcd);
    for (unsigned y = 0; y < gpu_diagnostic::Height; ++y)
        for (unsigned x = 0; x < rowBytes; ++x) mapped[512 + std::size_t(y) * pitch + x] = std::uint8_t(x + y);
    std::uint64_t hash = 0;
    const auto* prefix = "NATIVE_SHARED_TEXTURE_PIXEL_ROW";
    require(driver_capture::write(prefix,1,mapped.data(),511,pitch,hash) == E_INVALIDARG);
    require(driver_capture::write(prefix,1,mapped.data(),512,pitch+1,hash) == E_INVALIDARG);
    require(driver_capture::write("../escape",1,mapped.data(),512,pitch,hash) == E_INVALIDARG);
    require(driver_capture::write(prefix,0,mapped.data(),512,pitch,hash) == E_INVALIDARG);
    require(driver_capture::write(prefix,3,mapped.data(),512,pitch,hash) == E_INVALIDARG);
    require(driver_capture::write(prefix,1,mapped.data(),512,pitch,hash) == S_OK);
    const auto firstHash = hash;
    // A second attempt must fail without truncating the original evidence.
    require(FAILED(driver_capture::write(prefix,1,mapped.data(),512,pitch,hash)));
    std::ifstream file(driver_capture::directory() / "native-copy-1.rgba", std::ios::binary | std::ios::ate);
    const auto count = std::size_t(rowBytes) * gpu_diagnostic::Height;
    require(file && file.tellg() == static_cast<std::streamoff>(count));
    file.seekg(0); std::vector<std::uint8_t> dense(count);
    file.read(reinterpret_cast<char*>(dense.data()), count); require(bool(file));
    std::uint64_t expected = UINT64_C(14695981039346656037);
    for (unsigned y = 0; y < gpu_diagnostic::Height; ++y) for (unsigned x = 0; x < rowBytes; ++x) {
        const auto value = std::uint8_t(x + y);
        require(dense[std::size_t(y) * rowBytes + x] == value);
        expected = (expected ^ value) * UINT64_C(1099511628211);
    }
    require(expected == firstHash);
    // The compositor path uses dynamic extents, format metadata and native row
    // padding. Check the actual dense file against independently indexed data.
    constexpr unsigned compositorWidth=137, compositorHeight=11, compositorPitch=768;
    std::vector<std::uint8_t> compositor(compositorPitch*compositorHeight,0xee);
    for(unsigned y=0;y<compositorHeight;++y) for(unsigned x=0;x<compositorWidth*4;++x)
        compositor[y*compositorPitch+x]=static_cast<std::uint8_t>(x*3+y*7);
    require(driver_capture::writeCompositor(0,compositorWidth,compositorHeight,0x34325258u,compositor.data(),compositorPitch,hash)==E_INVALIDARG);
    require(driver_capture::writeCompositor(1,4097,1,0x34325258u,compositor.data(),compositorPitch,hash)==E_INVALIDARG);
    require(driver_capture::writeCompositor(1,compositorWidth,compositorHeight,0,compositor.data(),compositorPitch,hash)==E_INVALIDARG);
    require(driver_capture::writeCompositor(1,compositorWidth,compositorHeight,0x34325258u,compositor.data(),compositorPitch-1,hash)==E_INVALIDARG);
    require(driver_capture::writeCompositor(1,compositorWidth,compositorHeight,0x34325258u,compositor.data(),compositorPitch,hash)==S_OK);
    const auto compositorHash=hash;
    require(FAILED(driver_capture::writeCompositor(1,compositorWidth,compositorHeight,0x34325258u,compositor.data(),compositorPitch,hash)));
    std::ifstream captured(driver_capture::directory()/"compositor-copy-1.raw",std::ios::binary|std::ios::ate);
    require(captured && captured.tellg()==compositorWidth*compositorHeight*4);
    captured.seekg(0); std::vector<std::uint8_t> actual(compositorWidth*compositorHeight*4);
    captured.read(reinterpret_cast<char*>(actual.data()),actual.size()); require(bool(captured));
    expected=UINT64_C(14695981039346656037);
    for(unsigned y=0;y<compositorHeight;++y) for(unsigned x=0;x<compositorWidth*4;++x) {
        const auto value=static_cast<std::uint8_t>(x*3+y*7);
        require(actual[y*compositorWidth*4+x]==value);
        expected=(expected^value)*UINT64_C(1099511628211);
    }
    require(expected==compositorHash);
    std::cout << "PASS: capture bounds, row padding, exact bytes/hash and no overwrite\n";
}
