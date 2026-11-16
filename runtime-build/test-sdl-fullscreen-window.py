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
flags = section(create, '    /* A window that starts full screen', '    if (scon->hidden) {')
# The flags are settled before SDL creates the window.
assert create.index('scon->fullscreen_start = gui_fullscreen;') < create.index('SDL_CreateWindow(')
resize = section(source, 'void sdl2_window_resize(', 'static void sdl2_redraw(')
toggle = section(source, 'static void sdl2_window_size_after_full_screen(', 'static int get_mod_state(')
# Ctrl+Alt+F still goes through toggle_full_screen.
keys = section(source, 'case SDL_SCANCODE_F:', 'break;')
assert 'toggle_full_screen(scon);' in keys

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
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
typedef struct { int w, h; } DisplaySurface;
struct sdl2_console {
    SDL_Window *real_window;
    DisplaySurface *surface;
    bool fullscreen_start;
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

static int create_flags(struct sdl2_console *scon)
{
    int flags = 0;
''' + flags + r'''
    return flags;
}
''' + resize + toggle + r'''
/* SDL_CreateWindow with SDL_WINDOWPOS_UNDEFINED: centered on the display. */
static void create(struct sdl2_console *scon, SDL_Window *w, int display)
{
    int flags = create_flags(scon);
    memset(w, 0, sizeof(*w));
    w->display = display;
    w->windowed = (SDL_Rect){
        monitors[display].x + (monitors[display].w - scon->surface->w) / 2,
        monitors[display].y + (monitors[display].h - scon->surface->h) / 2,
        scon->surface->w, scon->surface->h };
    w->flags = flags & ~SDL_WINDOW_FULLSCREEN_DESKTOP;
    w->now = w->windowed;
    if (flags & SDL_WINDOW_FULLSCREEN)
        SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
    scon->real_window = w;
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

    puts("ok - full screen starts are resizable and leave full screen inside the usable area; windowed starts keep their size");
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
