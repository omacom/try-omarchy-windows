/* SPDX-License-Identifier: MIT */
#include "wddm-sdl-command.h"
#include <assert.h>
#include <stddef.h>

int main(void)
{
    WddmSdlCommand command;
    assert(wddm_sdl_parse("attach:1", &command) &&
           command.action == WDDM_SDL_ATTACH && command.generation == 1);
    assert(wddm_sdl_parse("audit:19:281474976710655", &command) &&
           command.action == WDDM_SDL_AUDIT &&
           command.frame == WDDM_SDL_MAX_COUNTER);
    assert(wddm_sdl_parse("present:19:2", &command) &&
           command.action == WDDM_SDL_PRESENT && command.frame == 2);
    assert(wddm_sdl_parse("detach:19", &command) &&
           command.action == WDDM_SDL_DETACH && !command.frame);
    const char *invalid[] = {
        "", "attach", "attach:0", "attach:01", "attach:+1", "attach:-1",
        "attach:1 ", "attach:1:2", "present:1", "present:1:0", "audit:1:01",
        "present:1:2:3", "present:1:281474976710656", "detach:18446744073709551616",
        "map:1:4096", "attach:0x1", "attach:1\n"
    };
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        assert(!wddm_sdl_parse(invalid[i], &command));
    }
    assert(!wddm_sdl_parse(NULL, &command));
    assert(wddm_sdl_hex("00112233445566778899aabbccddeeff", 16));
    assert(wddm_sdl_hex("1122334455667788", 8));
    assert(!wddm_sdl_hex("00112233445566778899AABBCCDDEEFF", 16));
    assert(!wddm_sdl_hex("00112233445566778899aabbccddeeff-x", 16));
    assert(!wddm_sdl_hex(NULL, 8));
    return 0;
}
