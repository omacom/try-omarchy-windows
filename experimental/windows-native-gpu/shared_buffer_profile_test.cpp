// SPDX-License-Identifier: MIT
#include "shared_buffer_profile.h"
#include <cstdlib>
#include <iostream>
#include <limits>
static void require(bool value) { if (!value) std::abort(); }
int main() {
    using namespace driver_shared;
    const BufferDescription valid{1, 0, 65536, 1, 1, 1, 0, 1, 0, 1, 0};
    require(validBufferDescription(valid));
    auto d = valid; d.alignment = 65536; d.flags = 4; d.width = MaxSharedBufferBytes;
    require(validBufferDescription(d));
    d.width = 1; require(validBufferDescription(d));
    unsigned rejected = 0;
    for (unsigned test = 0; test < 14; ++test) {
        d = valid;
        switch (test) {
        case 0: d.dimension = 3; break;
        case 1: d.alignment = 4096; break;
        case 2: d.width = 0; break;
        case 3: d.width = MaxSharedBufferBytes + 1; break;
        case 4: d.width = std::numeric_limits<std::uint64_t>::max(); break;
        case 5: d.height = 2; break;
        case 6: d.layers = 2; break;
        case 7: d.levels = 2; break;
        case 8: d.format = 28; break;
        case 9: d.samples = 2; break;
        case 10: d.quality = 1; break;
        case 11: d.layout = 0; break;
        case 12: d.flags = 1; break;
        case 13: d.flags = 16; break;
        }
        require(!validBufferDescription(d)); ++rejected;
    }
    std::cout << "SHARED_BUFFER_PROFILE valid=true rejected=" << rejected << " maximumBytes="
              << MaxSharedBufferBytes << " textureProfileChanged=false gpu=false\n";
}
