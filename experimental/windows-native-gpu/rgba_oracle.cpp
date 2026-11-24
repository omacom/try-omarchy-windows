// SPDX-License-Identifier: MIT
// Independent integer rasterizer for dense RGBA captures. No GPU or vendor ABI.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc, char** argv) {
    if (argc != 3) { std::cerr << "Usage: rgba-oracle frame1.rgba frame2.rgba\n"; return 2; }
    constexpr std::int64_t w = 3840, h = 2160, area = 36 * w * h;
    constexpr std::size_t count = static_cast<std::size_t>(w * h * 4);
    std::vector<std::uint8_t> bytes(count);
    std::uint64_t previous = 0;
    for (unsigned round = 1; round <= 2; ++round) {
        std::ifstream file(argv[round], std::ios::binary | std::ios::ate);
        if (!file || file.tellg() != static_cast<std::streamoff>(count)) return 1;
        file.seekg(0); file.read(reinterpret_cast<char*>(bytes.data()), count); if (!file) return 1;
        std::uint64_t hash = UINT64_C(14695981039346656037), referenceHash = hash;
        unsigned triangle = 0, maximum = 0;
        for (std::int64_t y = 0; y < h; ++y) for (std::int64_t x = 0; x < w; ++x) {
            const auto px = 8 * x + 4, py = 8 * y + 4;
            const auto a = -6 * h * (px - 7 * w) + 3 * w * (py - 7 * h);
            const auto b = -6 * w * (py - 7 * h);
            const auto c = area - a - b;
            if (!a || !b || !c) return 1; // Reject ambiguous edge samples.
            const bool inside = a > 0 && b > 0 && c > 0; triangle += inside;
            unsigned expected[4]{0,0,round == 1 ? 255u : 0u,255};
            if (inside) {
                const std::int64_t weights[3]{a,b,c};
                for (unsigned v = 0; v < 3; ++v)
                    expected[(v + round - 1) % 3] = static_cast<unsigned>((255 * weights[v] + area / 2) / area);
            }
            for (unsigned ch = 0; ch < 4; ++ch) {
                const auto actual = bytes[static_cast<std::size_t>((y * w + x) * 4) + ch];
                const auto error = static_cast<unsigned>(std::abs(int(actual) - int(expected[ch])));
                maximum = (std::max)(maximum, error);
                if (error > (inside && ch < 3 ? 2u : 0u)) return 1;
                hash = (hash ^ actual) * UINT64_C(1099511628211);
                referenceHash = (referenceHash ^ expected[ch]) * UINT64_C(1099511628211);
            }
        }
        if (triangle != 2332800 || (round == 2 && hash == previous)) return 1;
        previous = hash;
        std::cout << "{\"round\":" << round << ",\"width\":3840,\"height\":2160,\"verifiedPixels\":8294400"
            << ",\"trianglePixels\":" << triangle << ",\"maximumChannelError\":" << maximum
            << ",\"fnv1a\":" << hash << ",\"referenceFnv1a\":" << referenceHash << "}\n";
    }
}
