/* SPDX-License-Identifier: MIT */
#include "wddm-fence-map.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    WddmFenceCommand command;
    const char *bad[] = { "", "map", "map:", "map:0:4096", "map:0:4096:1:2",
        "map:-1:4096:1", "map:+1:4096:1", "map:512:4096:1", "map:0:0:1",
        "map:0:4097:1", "map:0:4096:0", "map:0:18446744073709551616:1",
        "map:0:4096:18446744073709551616", "unmap:0", "unmap:0:0",
        "unmap:512:1", "unmap:0:1:", "unmap:0:1 ", "MAP:0:4096:1" };
    for (size_t n = 0; n < sizeof bad / sizeof bad[0]; ++n) {
        assert(!wddm_fence_command(bad[n], &command));
    }
    char long_input[130]; memset(long_input, '1', sizeof long_input); long_input[129] = 0;
    assert(!wddm_fence_command(long_input, &command));
    assert(!wddm_fence_command(NULL, &command));
    assert(wddm_fence_command("map:63:8184:10", &command));
    assert(command.map && command.slot == 63 && command.source == 8184 && command.generation == 10);
    assert(wddm_fence_transition(&command, 9, 0));
    assert(!wddm_fence_transition(&command, 10, 0));
    assert(!wddm_fence_transition(&command, 9, 7));
    assert(wddm_fence_command("unmap:63:10", &command));
    assert(wddm_fence_transition(&command, 10, 10));
    assert(!wddm_fence_transition(&command, 10, 9));
    assert(!wddm_fence_transition(&command, 10, 0));
    assert(wddm_fence_command("map:0:4096:11", &command));
    assert(wddm_fence_transition(&command, 10, 0));
    assert(wddm_fence_command("map:0:18446744073709551608:18446744073709551615", &command));
    for (uint32_t slots = 0; slots <= 1024; ++slots)
        assert(wddm_fence_slots_valid(slots) == (slots == 64 || slots == 128 || slots == 256 || slots == 512));
    assert(wddm_fence_command_for_slots("map:63:4096:1", 64, &command));
    assert(!wddm_fence_command_for_slots("map:64:4096:1", 64, &command));
    assert(!wddm_fence_command_for_slots("unmap:64:1", 64, &command));
    assert(wddm_fence_command_for_slots("map:127:4096:1", 128, &command));
    assert(wddm_fence_command_for_slots("unmap:127:1", 128, &command));
    assert(!wddm_fence_command_for_slots("map:128:4096:1", 128, &command));
    assert(!wddm_fence_command_for_slots("unmap:128:1", 128, &command));
    assert(wddm_fence_command_for_slots("map:255:4096:1", 256, &command));
    assert(wddm_fence_command_for_slots("unmap:255:1", 256, &command));
    assert(!wddm_fence_command_for_slots("map:256:4096:1", 256, &command));
    assert(!wddm_fence_command_for_slots("unmap:256:1", 256, &command));
    assert(wddm_fence_command_for_slots("map:511:4096:1", 512, &command));
    assert(command.slot==511 && command.source==4096 && command.generation==1);
    assert(wddm_fence_transition(&command,0,0));
    assert(!wddm_fence_transition(&command,1,0));
    assert(wddm_fence_command_for_slots("unmap:511:1", 512, &command));
    assert(wddm_fence_transition(&command,1,1));
    assert(!wddm_fence_transition(&command,1,2));
    assert(!wddm_fence_command_for_slots("map:512:4096:1", 512, &command));
    assert(!wddm_fence_command_for_slots("unmap:512:1", 512, &command));
    assert(!wddm_fence_command_for_slots("map:127:4096:1", 65, &command));
    puts("PASS: bounded host fence commands and generation ownership; explicit 512-page layout, legacy boundaries preserved");
    return 0;
}
