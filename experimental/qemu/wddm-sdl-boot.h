/* SPDX-License-Identifier: MIT */
/* Serialized QEMU main-loop ownership of an optional boot VGA console.
 * Include after ui/console.h. No SDL/input or guest rendering implementation.
 */
#ifndef WDDM_SDL_BOOT_H
#define WDDM_SDL_BOOT_H

typedef struct WddmSdlBoot {
    QemuConsole *console;
    const GraphicHwOps *original, *proxy;
    void *original_opaque, *proxy_opaque;
    bool native;
} WddmSdlBoot;

static inline bool wddm_sdl_boot_owned(const WddmSdlBoot *boot)
{
    void *opaque = NULL;
    return boot->console &&
           graphic_console_get_hwops(boot->console, &opaque) == boot->proxy &&
           opaque == boot->proxy_opaque;
}

static inline bool wddm_sdl_boot_bind(WddmSdlBoot *boot, QemuConsole *console,
                                      const GraphicHwOps *proxy, void *opaque)
{
    void *original_opaque = NULL;
    const GraphicHwOps *original;
    if (boot->console || !console || !proxy || !opaque) {
        return false;
    }
    original = graphic_console_get_hwops(console, &original_opaque);
    /* Only the synchronous, non-GL VGA path is supported. In particular a
     * second relay cannot steal the first relay's GL console proxy.
     */
    if (!original || original == proxy || original->gfx_update_async ||
        original->ui_info || original->gl_block ||
        (original->get_flags && original->get_flags(original_opaque))) {
        return false;
    }
    *boot = (WddmSdlBoot){ .console = console, .original = original,
        .original_opaque = original_opaque, .proxy = proxy,
        .proxy_opaque = opaque };
    graphic_console_set_hwops(console, proxy, opaque);
    return true;
}

static inline void wddm_sdl_boot_update(WddmSdlBoot *boot)
{
    if (boot->console && !boot->native && boot->original->gfx_update) {
        boot->original->gfx_update(boot->original_opaque);
    }
}

static inline void wddm_sdl_boot_invalidate(WddmSdlBoot *boot)
{
    if (boot->console && !boot->native && boot->original->invalidate) {
        boot->original->invalidate(boot->original_opaque);
    }
}

static inline void wddm_sdl_boot_text(WddmSdlBoot *boot, console_ch_t *text)
{
    if (boot->console && !boot->native && boot->original->text_update) {
        boot->original->text_update(boot->original_opaque, text);
    }
}

static inline bool wddm_sdl_boot_enter(WddmSdlBoot *boot)
{
    if (!wddm_sdl_boot_owned(boot)) {
        return false;
    }
    boot->native = true;
    return true;
}

/* Caller must retire and disable the native scanout BEFORE returning to VGA. */
static inline bool wddm_sdl_boot_leave(WddmSdlBoot *boot)
{
    if (!wddm_sdl_boot_owned(boot)) {
        return false;
    }
    boot->native = false;
    graphic_hw_invalidate(boot->console);
    graphic_hw_update(boot->console);
    return true;
}

/* Never close a borrowed console, or overwrite another owner's callbacks. */
static inline bool wddm_sdl_boot_restore(WddmSdlBoot *boot)
{
    if (!wddm_sdl_boot_owned(boot)) {
        return false;
    }
    graphic_console_set_hwops(boot->console, boot->original, boot->original_opaque);
    *boot = (WddmSdlBoot){0};
    return true;
}

#endif
