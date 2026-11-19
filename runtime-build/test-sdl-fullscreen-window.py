#!/usr/bin/env python3
"""Leave a full screen start for a window that fits, against SDL's windowed size rules."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
root = parser.parse_args().source
source = (root / 'ui/sdl2.c').read_text(encoding='utf-8')
header = (root / 'include/ui/sdl2.h').read_text(encoding='utf-8')


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


assert 'bool fullscreen_start;' in section(header, 'struct sdl2_console {', '};')
create = section(source, 'void sdl2_window_create(', 'void sdl2_window_destroy(')
# The flags are settled before SDL creates the window.
assert create.index('scon->fullscreen_start = gui_fullscreen;') < create.index('SDL_CreateWindow(')
# A guest size the window had while full screen is remembered as SDL
# reports the window resized (the RESIZED handler), and sdl2_window_resize
# does not follow the guest back to it after the window left full screen.
resize = section(source, 'static bool sdl2_full_screen_size(', 'static void sdl2_redraw(')
resized = section(source, '            if (SDL_GetWindowFlags(scon->real_window) & SDL_WINDOW_FULLSCREEN) {\n'
                  '                sdl2_full_screen_size(', '            dpy_set_ui_info(')
toggle = section(source, 'static void sdl2_window_size_after_full_screen(', 'static int get_mod_state(')
# Ctrl+Alt+F still goes through toggle_full_screen.
keys = section(source, 'case SDL_SCANCODE_F:', 'break;')
assert 'toggle_full_screen(scon);' in keys
# Ctrl+Alt+0, the user asking for the guest's size.
zoom = section(source, '        case SDL_SCANCODE_0:\n', '            break;\n').split('\n', 1)[1]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SDL_WINDOW_FULLSCREEN 0x1
#define SDL_WINDOW_RESIZABLE 0x20
#define SDL_WINDOW_FULLSCREEN_DESKTOP (SDL_WINDOW_FULLSCREEN | 0x1000)
typedef struct { int x, y, w, h; } SDL_Rect;
/* SDL 2.32: a window's current rectangle, the windowed rectangle it gets
 * back when it leaves full screen, and the monitor it covers. */
typedef struct {
    int flags;
    SDL_Rect now, windowed;
    int display;
} SDL_Window;
#define SDL_WINDOW_HIDDEN 0x8
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
typedef struct { int w, h; } DisplaySurface;
typedef struct { int unused; } QemuConsole;
typedef struct { QemuConsole *con; } DisplayChangeListener;
typedef struct { unsigned width, height; } QemuUIInfo;
/* The size the window last asked the guest for (dpy_set_ui_info). */
static QemuUIInfo asked;
__attribute__((unused)) static const QemuUIInfo *dpy_get_ui_info(QemuConsole *con) { (void)con; return &asked; }
/* Whether the display device takes sizes from the window (ui-info). */
static bool ui_info = true;
__attribute__((unused)) static bool dpy_ui_info_supported(QemuConsole *con) { (void)con; return ui_info; }
enum { DISPLAY_GL_MODE_ES = 2 };
typedef struct { int gl; } DisplayOptions;
struct sdl2_console {
    DisplayChangeListener dcl;
    DisplayOptions *opts;
    SDL_Window *real_window;
    void *real_renderer, *winctx;
    DisplaySurface *surface;
    bool hidden, opengl, gui_keysym;
    bool fullscreen_start;
    int full_screen_size[4][2];
    int full_screen_sizes;
    bool after_full_screen;
    bool cloaked;
};
static SDL_Rect monitors[2] = { {0, 0, 1920, 1080}, {1920, 0, 2560, 1440} };
static SDL_Rect usable[2] = { {0, 0, 1920, 1040}, {1920, 0, 2560, 1400} };
static bool usable_fails;
static int gui_fullscreen, gui_grab, gui_saved_grab, size_calls;
static int surface_width(DisplaySurface *s) { return s->w; }
static int surface_height(DisplaySurface *s) { return s->h; }
static unsigned SDL_GetWindowFlags(SDL_Window *w) { return (unsigned)w->flags; }
static int SDL_GetWindowDisplayIndex(SDL_Window *w) { return w->display; }
static int SDL_GetDisplayUsableBounds(int display, SDL_Rect *r)
{
    if (usable_fails) return -1;
    *r = usable[display];
    return 0;
}
static void SDL_SetWindowSize(SDL_Window *w, int width, int height)
{
    size_calls++;
    w->windowed.w = width;
    w->windowed.h = height;
    if (!(w->flags & SDL_WINDOW_FULLSCREEN)) {
        w->now.w = width;
        w->now.h = height;
    }
}
__attribute__((unused)) static void SDL_GetWindowSize(SDL_Window *w, int *width, int *height)
{
    *width = w->now.w;
    *height = w->now.h;
}
static void SDL_SetWindowPosition(SDL_Window *w, int x, int y)
{
    w->windowed.x = x;
    w->windowed.y = y;
    if (!(w->flags & SDL_WINDOW_FULLSCREEN)) {
        w->now.x = x;
        w->now.y = y;
    }
}
static int SDL_SetWindowFullscreen(SDL_Window *w, unsigned flags)
{
    w->flags = (w->flags & ~SDL_WINDOW_FULLSCREEN_DESKTOP) | (int)flags;
    w->now = flags ? monitors[w->display] : w->windowed;
    return 0;
}
static void sdl_grab_start(struct sdl2_console *scon) { (void)scon; gui_grab = 1; }
static void sdl_grab_end(struct sdl2_console *scon) { (void)scon; gui_grab = 0; }
static void sdl2_redraw(struct sdl2_console *scon) { (void)scon; }
static void sdl_show_cursor(struct sdl2_console *scon) { (void)scon; }
static int texture_switches;
static void sdl2_2d_switch(DisplayChangeListener *dcl, DisplaySurface *s) { (void)dcl; (void)s; texture_switches++; }
/* SDL_CreateWindow with SDL_WINDOWPOS_UNDEFINED: centered on the display
 * the harness picks (next_display), full screen there if asked. */
static SDL_Window *next_window;
static int next_display;
static SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int width, int height, int flags)
{
    SDL_Window *w = next_window;
    (void)title; (void)x; (void)y;
    memset(w, 0, sizeof(*w));
    w->display = next_display;
    w->windowed = (SDL_Rect){
        monitors[w->display].x + (monitors[w->display].w - width) / 2,
        monitors[w->display].y + (monitors[w->display].h - height) / 2,
        width, height };
    w->flags = flags & ~SDL_WINDOW_FULLSCREEN_DESKTOP;
    w->now = w->windowed;
    if (flags & SDL_WINDOW_FULLSCREEN)
        SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
    return w;
}
/* Only 2D windows here; the OpenGL calls are never reached. */
static struct sdl2_console sdl2_console[1];
#define SDL_HINT_RENDER_DRIVER ""
#define SDL_HINT_RENDER_BATCHING ""
#define SDL_GL_SHARE_WITH_CURRENT_CONTEXT 0
static void SDL_SetHint(const char *h, const char *v) { (void)h; (void)v; assert(0); }
static void SDL_GL_MakeCurrent(SDL_Window *w, void *c) { (void)w; (void)c; assert(0); }
static void SDL_GL_SetAttribute(int a, int v) { (void)a; (void)v; assert(0); }
static void *SDL_GL_CreateContext(SDL_Window *w) { (void)w; assert(0); return 0; }
static void SDL_GL_SetSwapInterval(int i) { (void)i; assert(0); }
static int win_hold_flags(struct sdl2_console *scon) { (void)scon; return 0; }
static void win_hold_window(struct sdl2_console *scon) { (void)scon; }
static void win_cloak_end(struct sdl2_console *scon) { (void)scon; }
static void win_cloak_presented(struct sdl2_console *scon, int w, int h) { (void)scon; (void)w; (void)h; }
static void win_pinch_create(struct sdl2_console *scon) { (void)scon; }
static void sdl_update_caption(struct sdl2_console *scon) { (void)scon; }
static void *SDL_CreateRenderer(SDL_Window *w, int index, int flags) { (void)index; (void)flags; return w; }
''' + create + resize + toggle + r'''
/* The RESIZED handler, with the size SDL reports. */
static void resized(struct sdl2_console *scon, int width, int height)
{
    QemuUIInfo info = { (unsigned)width, (unsigned)height };
''' + resized + r'''
    asked = info;
}
/* SDL reports the window's size after it changed. */
static void reported(struct sdl2_console *scon)
{
    resized(scon, scon->real_window->now.w, scon->real_window->now.h);
}
/* The launcher keeps a full screen window one line larger (#422). */
static void launcher_grows(struct sdl2_console *scon)
{
    scon->real_window->now.y -= 1;
    scon->real_window->now.h += 1;
    reported(scon);
}
/* QEMU's own sdl2_window_create, after the last window was destroyed. */
static void create(struct sdl2_console *scon, SDL_Window *w, int display)
{
    scon->real_window = NULL;
    next_window = w;
    next_display = display;
    sdl2_window_create(scon);
    assert(scon->real_window == w);
}
/* Ctrl+Alt+0. */
static void zoom_reset(struct sdl2_console *scon)
{
''' + zoom + r'''
}
static void guest_resizes(struct sdl2_console *scon, int w, int h)
{
    scon->surface->w = w;
    scon->surface->h = h;
    sdl2_window_resize(scon);
}
static void user_resizes(SDL_Window *w, int width, int height)
{
    assert(w->flags & SDL_WINDOW_RESIZABLE);
    w->now.w = w->windowed.w = width;
    w->now.h = w->windowed.h = height;
}
/* The window and a Windows 11 frame (8 px sides and bottom, 31 px title)
 * lie within the usable area of the display. */
static bool fits(const SDL_Window *w)
{
    SDL_Rect u = usable[w->display];
    return w->now.x - 8 >= u.x && w->now.y - 31 >= u.y &&
           w->now.x + w->now.w + 8 <= u.x + u.w &&
           w->now.y + w->now.h + 8 <= u.y + u.h;
}
static bool same(SDL_Rect a, SDL_Rect b)
{
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

int main(void)
{
    SDL_Window win;
    DisplaySurface surface;
    struct sdl2_console scon = { .surface = &surface };

    /* A windowed start is resizable and has its own windowed size. */
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    assert(win.flags == SDL_WINDOW_RESIZABLE && !scon.fullscreen_start);
    /* The guest follows the window; in a window QEMU sizes it to the guest. */
    guest_resizes(&scon, 1024, 768);
    assert(win.now.w == 1024 && win.now.h == 768);
    SDL_Rect before = win.now;
    /* Full screen and back gives back the same window, even after the
     * guest took the monitor's size in between. */
    toggle_full_screen(&scon);
    assert(gui_fullscreen && same(win.now, monitors[0]));
    guest_resizes(&scon, 1920, 1081);
    assert(same(win.now, monitors[0]));
    toggle_full_screen(&scon);
    assert(!gui_fullscreen && same(win.now, before) && !(win.flags & SDL_WINDOW_FULLSCREEN));

    /* A full screen start, the launcher growing it one line past the
     * monitor and the guest following (#422). It is resizable, and leaving
     * full screen gives three quarters of the usable area, centered. */
    gui_fullscreen = 1;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    assert(win.flags == (SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE));
    assert(scon.fullscreen_start && same(win.now, monitors[0]));
    size_calls = 0;
    guest_resizes(&scon, 1920, 1081);
    assert(size_calls == 0 && same(win.now, monitors[0]));
    toggle_full_screen(&scon);
    assert(!gui_fullscreen && !scon.fullscreen_start);
    assert(same(win.now, ((SDL_Rect){240, 130, 1440, 780})) && fits(&win));
    /* From then on it is a window like any other: the user's size comes
     * back after the next round of full screen. */
    user_resizes(&win, 1600, 900);
    before = win.now;
    toggle_full_screen(&scon);
    guest_resizes(&scon, 1920, 1081);
    toggle_full_screen(&scon);
    assert(same(win.now, before) && fits(&win));

    /* On a second, larger monitor it is centered there. */
    gui_fullscreen = 1;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 1);
    guest_resizes(&scon, 2560, 1441);
    toggle_full_screen(&scon);
    assert(same(win.now, ((SDL_Rect){1920 + 320, 175, 1920, 1050})) && fits(&win));

    /* Without usable bounds SDL's own windowed size stays. */
    usable_fails = true;
    gui_fullscreen = 1;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    guest_resizes(&scon, 1920, 1081);
    toggle_full_screen(&scon);
    assert(same(win.now, ((SDL_Rect){320, 140, 1280, 800})));
    usable_fails = false;

    /* The guest takes a size a second or more after the window asks for it,
     * so it can still switch to the full screen sizes after the window left
     * full screen. The window keeps its size, and follows any size that
     * was not one of its full screen sizes. */
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    before = win.now;
    toggle_full_screen(&scon);
    reported(&scon);
    launcher_grows(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    assert(same(win.now, before));
    guest_resizes(&scon, 1920, 1080);
    assert(same(win.now, before) && same(win.windowed, before));
    guest_resizes(&scon, 1920, 1081);
    assert(same(win.now, before) && same(win.windowed, before));
    guest_resizes(&scon, 1280, 800);
    guest_resizes(&scon, 1024, 768);
    assert(win.now.w == 1024 && win.now.h == 768);
    /* The next round of full screen remembers its own sizes only: on the
     * second monitor, the first monitor's full screen size is a size like
     * any other afterwards. */
    win.display = 1;
    before = win.now;
    toggle_full_screen(&scon);
    reported(&scon);
    launcher_grows(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    guest_resizes(&scon, 2560, 1441);
    assert(same(win.now, before));
    guest_resizes(&scon, 1920, 1081);
    assert(win.now.w == 1920 && win.now.h == 1081);
    win.display = 0;

    /* A display device without ui-info is never asked for a size: the
     * window follows the guest after full screen as before. */
    ui_info = false;
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    guest_resizes(&scon, 1920, 1080);
    assert(win.now.w == 1920 && win.now.h == 1080);
    ui_info = true;

    /* A full screen start keeps its first windowed size the same way. */
    gui_fullscreen = 1;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    launcher_grows(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    before = win.now;
    guest_resizes(&scon, 1920, 1081);
    assert(same(win.now, before) && fits(&win));
    guest_resizes(&scon, 1440, 780);
    assert(same(win.now, before));

    /* Ctrl+Alt+0 asks for the guest's size and gets it, also while a late
     * full screen size is held back. */
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    before = win.now;
    toggle_full_screen(&scon);
    reported(&scon);
    launcher_grows(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    guest_resizes(&scon, 1920, 1081);
    assert(same(win.now, before));
    zoom_reset(&scon);
    assert(win.now.w == 1920 && win.now.h == 1081 && !scon.after_full_screen);

    /* A round of full screen in which the guest never changed size. The
     * window still holds back the full screen size, which may be on its
     * way, and follows the user's resize, any other size, and Ctrl+Alt+0. */
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    assert(scon.after_full_screen);
    user_resizes(&win, 1024, 768);
    reported(&scon);
    guest_resizes(&scon, 1024, 768);
    assert(win.now.w == 1024 && win.now.h == 768);
    guest_resizes(&scon, 800, 600);
    assert(win.now.w == 800 && win.now.h == 600);
    guest_resizes(&scon, 1920, 1080);
    assert(win.now.w == 800 && win.now.h == 600);
    zoom_reset(&scon);
    assert(win.now.w == 1920 && win.now.h == 1080);
    /* A size the window asks for is followed even if it was a full screen
     * size. */
    toggle_full_screen(&scon);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    user_resizes(&win, 1920, 1080);
    reported(&scon);
    guest_resizes(&scon, 1280, 720);
    guest_resizes(&scon, 1920, 1080);
    assert(win.now.w == 1920 && win.now.h == 1080);

    /* A guest a round of full screen behind: after the window left full
     * screen it shows the windowed size it was given before the round, then
     * the round's full screen size, then the windowed size again. */
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    before = win.now;
    toggle_full_screen(&scon);
    reported(&scon);
    launcher_grows(&scon);
    guest_resizes(&scon, 1920, 1081);
    toggle_full_screen(&scon);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    launcher_grows(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    guest_resizes(&scon, 1280, 800);
    assert(same(win.now, before));
    guest_resizes(&scon, 1920, 1081);
    assert(same(win.now, before) && same(win.windowed, before));
    guest_resizes(&scon, 1280, 800);
    assert(same(win.now, before));

    /* A window destroyed with a size held back and created again starts
     * clean. */
    gui_fullscreen = 0;
    surface = (DisplaySurface){1280, 800};
    create(&scon, &win, 0);
    reported(&scon);
    toggle_full_screen(&scon);
    reported(&scon);
    toggle_full_screen(&scon);
    assert(scon.after_full_screen && scon.full_screen_sizes > 0);
    create(&scon, &win, 0);
    assert(!scon.after_full_screen && scon.full_screen_sizes == 0);
    guest_resizes(&scon, 1920, 1080);
    assert(win.now.w == 1920 && win.now.h == 1080);

    /* Two windows. gui_fullscreen is shared by every window, and Ctrl+Alt+F
     * switches the window it is pressed in, going by that window's state. */
    {
        SDL_Window win2;
        DisplaySurface surface2 = {1280, 800};
        struct sdl2_console scon2 = { .surface = &surface2 };

        /* Both start full screen. Leaving in the first window, then in the
         * second, takes each out of full screen; the second, whose size SDL
         * never reported, still holds back its full screen size. */
        gui_fullscreen = 1;
        surface = (DisplaySurface){1280, 800};
        create(&scon, &win, 0);
        create(&scon2, &win2, 1);
        reported(&scon);
        toggle_full_screen(&scon);
        assert(!(win.flags & SDL_WINDOW_FULLSCREEN) && (win2.flags & SDL_WINDOW_FULLSCREEN));
        toggle_full_screen(&scon2);
        assert(!(win2.flags & SDL_WINDOW_FULLSCREEN) && scon2.after_full_screen);
        before = win2.now;
        reported(&scon2);
        guest_resizes(&scon2, 2560, 1440);
        assert(same(win2.now, before) && fits(&win2));
        /* And back in, each on its own press. */
        toggle_full_screen(&scon2);
        assert((win2.flags & SDL_WINDOW_FULLSCREEN) && !(win.flags & SDL_WINDOW_FULLSCREEN));
        toggle_full_screen(&scon);
        assert(win.flags & SDL_WINDOW_FULLSCREEN);

        /* Windowed start: the second window goes full screen while the first
         * one is, and the first leaves on its next press. A window whose
         * press only saw the shared flag would have its remembered sizes
         * wiped while it stayed full screen. */
        gui_fullscreen = 0;
        surface = (DisplaySurface){1280, 800};
        surface2 = (DisplaySurface){1024, 768};
        create(&scon, &win, 0);
        create(&scon2, &win2, 1);
        reported(&scon);
        reported(&scon2);
        toggle_full_screen(&scon);
        reported(&scon);
        launcher_grows(&scon);
        toggle_full_screen(&scon2);
        reported(&scon2);
        assert((win.flags & SDL_WINDOW_FULLSCREEN) && (win2.flags & SDL_WINDOW_FULLSCREEN));
        before = win.windowed;
        toggle_full_screen(&scon);
        reported(&scon);
        assert(!(win.flags & SDL_WINDOW_FULLSCREEN) && (win2.flags & SDL_WINDOW_FULLSCREEN));
        guest_resizes(&scon, 1920, 1081);
        assert(same(win.now, before));
        toggle_full_screen(&scon2);
        reported(&scon2);
        assert(!(win2.flags & SDL_WINDOW_FULLSCREEN) && win2.now.w == 1024 && win2.now.h == 768);
    }

    puts("ok - full screen starts are resizable and leave full screen inside the usable area; windowed starts keep their size, also when the guest is late");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-fullscreen-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    binary = test / 'test.exe'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(test / 'test.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
