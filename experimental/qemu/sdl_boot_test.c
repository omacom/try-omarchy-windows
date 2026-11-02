/* SPDX-License-Identifier: MIT */
/* Real production ownership helper, with CPU-only QEMU callback stand-ins. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

typedef uint32_t console_ch_t;
typedef struct GraphicHwOps {
    int (*get_flags)(void *);
    void (*invalidate)(void *);
    void (*gfx_update)(void *);
    bool gfx_update_async;
    void (*text_update)(void *, console_ch_t *);
    void (*ui_info)(void *, uint32_t, void *);
    void (*gl_block)(void *, bool);
} GraphicHwOps;
typedef struct QemuConsole {
    const GraphicHwOps *ops;
    void *opaque;
} QemuConsole;
static const GraphicHwOps *graphic_console_get_hwops(QemuConsole *con, void **opaque)
{
    *opaque = con->opaque;
    return con->ops;
}
static void graphic_console_set_hwops(QemuConsole *con, const GraphicHwOps *ops, void *opaque)
{
    con->ops = ops;
    con->opaque = opaque;
}
static void graphic_hw_invalidate(QemuConsole *con)
{
    if (con->ops->invalidate) {
        con->ops->invalidate(con->opaque);
    }
}
static void graphic_hw_update(QemuConsole *con)
{
    if (con->ops->gfx_update) {
        con->ops->gfx_update(con->opaque);
    }
}
#include "wddm-sdl-boot.h"

typedef struct Counters { unsigned updates, invalidations, text; } Counters;
static void update(void *opaque) { ++((Counters *)opaque)->updates; }
static void invalidate(void *opaque) { ++((Counters *)opaque)->invalidations; }
static void text(void *opaque, console_ch_t *data)
{
    assert(data && *data == 0x1234);
    ++((Counters *)opaque)->text;
}
static int gl_flags(void *opaque) { (void)opaque; return 1; }
static void ui_info(void *opaque, uint32_t head, void *info)
{
    (void)opaque; (void)head; (void)info;
}
static void gl_block(void *opaque, bool block) { (void)opaque; (void)block; }
static void proxy_update(void *opaque) { wddm_sdl_boot_update(opaque); }
static void proxy_invalidate(void *opaque) { wddm_sdl_boot_invalidate(opaque); }
static void proxy_text(void *opaque, console_ch_t *data) { wddm_sdl_boot_text(opaque, data); }

int main(void)
{
    const GraphicHwOps original = { .gfx_update = update, .invalidate = invalidate,
        .text_update = text };
    const GraphicHwOps proxy = { .get_flags = gl_flags, .gfx_update = proxy_update,
        .invalidate = proxy_invalidate, .text_update = proxy_text };
    Counters counters = {0};
    QemuConsole console = { &original, &counters };
    WddmSdlBoot boot = {0}, other = {0};
    console_ch_t character = 0x1234;
    assert(!wddm_sdl_boot_bind(&boot, NULL, &proxy, &boot));
    assert(!wddm_sdl_boot_bind(&boot, &console, NULL, &boot));
    assert(!wddm_sdl_boot_bind(&boot, &console, &proxy, NULL));
    assert(!wddm_sdl_boot_enter(&boot) && !wddm_sdl_boot_leave(&boot) && !wddm_sdl_boot_restore(&boot));
    GraphicHwOps invalid[] = { original, original, original, original };
    invalid[0].get_flags = gl_flags;
    invalid[1].gfx_update_async = true;
    invalid[2].ui_info = ui_info;
    invalid[3].gl_block = gl_block;
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        console.ops = &invalid[i];
        assert(!wddm_sdl_boot_bind(&boot, &console, &proxy, &boot));
        assert(!boot.console && console.ops == &invalid[i] && console.opaque == &counters);
    }
    console.ops = &original;
    assert(wddm_sdl_boot_bind(&boot, &console, &proxy, &boot));
    assert(wddm_sdl_boot_owned(&boot) && !boot.native);
    assert(!wddm_sdl_boot_bind(&boot, &console, &proxy, &boot));
    assert(!wddm_sdl_boot_bind(&other, &console, &proxy, &other));
    graphic_hw_update(&console);
    graphic_hw_invalidate(&console);
    console.ops->text_update(console.opaque, &character);
    assert(counters.updates == 1 && counters.invalidations == 1 && counters.text == 1);
    assert(wddm_sdl_boot_enter(&boot) && boot.native);
    for (unsigned i = 0; i < 10000; ++i) {
        graphic_hw_update(&console);
        graphic_hw_invalidate(&console);
        console.ops->text_update(console.opaque, &character);
    }
    /* A failed GPU operation deliberately does not call leave: VGA must remain
     * suppressed until the caller has retired/disabled native scanout.
     */
    assert(counters.updates == 1 && counters.invalidations == 1 && counters.text == 1);
    assert(wddm_sdl_boot_leave(&boot) && !boot.native);
    assert(counters.updates == 2 && counters.invalidations == 2);
    console.ops->text_update(console.opaque, &character);
    assert(counters.text == 2 && console.ops == &proxy && console.opaque == &boot);
    /* Preserve an unexpected successor, including a changed opaque identity. */
    console.opaque = &other;
    assert(!wddm_sdl_boot_owned(&boot));
    assert(!wddm_sdl_boot_enter(&boot) && !wddm_sdl_boot_leave(&boot) && !wddm_sdl_boot_restore(&boot));
    assert(console.ops == &proxy && console.opaque == &other);
    console.opaque = &boot;
    console.ops = &original;
    assert(!wddm_sdl_boot_restore(&boot) && console.ops == &original);
    console.ops = &proxy;
    assert(wddm_sdl_boot_restore(&boot));
    assert(!boot.console && console.ops == &original && console.opaque == &counters);
    graphic_hw_update(&console);
    assert(counters.updates == 3);
    puts("SDL_BOOT_OWNERSHIP_PASS bootCallbacks=true nativeSuppression=10000 successorPreserved=true gpu=false");
    return 0;
}
