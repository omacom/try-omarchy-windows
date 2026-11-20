#!/usr/bin/env python3
"""Exercise the held SDL windows the launcher shows, and their cloak, with mocked SDL and Win32."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
root = parser.parse_args().source
header = (root / 'ui/sdl2-held-win32.h').read_text(encoding='utf-8')
source = (root / 'ui/sdl2.c').read_text(encoding='utf-8')
body = header[header.index('#ifdef CONFIG_WIN32\n') + len('#ifdef CONFIG_WIN32\n'):header.index('#else\n')]
# The mocks below stand in for the DWM types.
assert body.startswith('#include <dwmapi.h>\n')
body = body[len('#include <dwmapi.h>\n'):]
# Nothing waits for DWM on QEMU's main thread.
assert 'DwmFlush' not in header


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


# The window is hidden before SDL creates it and marked right after, before
# the GL context or anything else that could take long.
create = section(source, 'void sdl2_window_create(', 'void sdl2_window_destroy(')
assert create.index('win_hold_flags(scon)') < create.index('SDL_CreateWindow(')
assert create.index('SDL_CreateWindow(') < create.index('win_hold_window(scon)') < create.index('SDL_GL_CreateContext(')
assert 'win_hold_release(scon, false)' in section(source, 'void sdl2_window_destroy(', 'void sdl2_window_resize(')
shown = section(source, 'case SDL_WINDOWEVENT_SHOWN:', 'break;')
assert 'win_hold_release(scon, false)' in shown
poll = section(source, 'void sdl2_poll_events(', 'static void sdl_mouse_warp(')
assert poll.index('while (SDL_PollEvent(ev))') < poll.index('win_hold_poll(scon)')
# A window that came on screen cloaked gets a frame at once.
cloak_poll = section(poll, 'if (win_cloak_poll(scon)) {', '    }\n')
assert cloak_poll.index('sdl2_redraw(scon);') < cloak_poll.index('win_cloak_poll(scon);', len('if (win_cloak_poll(scon))'))
assert poll.index('win_hold_poll(scon)') < poll.index('win_cloak_poll(scon)')
# Every way out uncloaks: destroyed, and Ctrl+Alt+F.
destroy = section(source, 'void sdl2_window_destroy(', 'void sdl2_window_resize(')
assert destroy.index('win_cloak_end(scon);\n    scon->cloaked = false;') < destroy.index('SDL_DestroyWindow(')
toggle = section(source, 'static void toggle_full_screen(', 'static int get_mod_state(')
assert toggle.index('win_cloak_end(scon);') < toggle.index('SDL_SetWindowFullscreen(')
assert 'win_cloak_presented(scon, w, h)' in section(source, 'void sdl2_window_presented(', 'static void sdl2_redraw(')
# Every present reports the size it drew at.
for name, call in (('ui/sdl2-gl.c', 'SDL_GL_SwapWindow(scon->real_window);'),
                   ('ui/sdl2-2d.c', 'SDL_RenderPresent(scon->real_renderer);')):
    text = (root / name).read_text(encoding='utf-8')
    parts = text.split(call)
    assert len(parts) > 1, name
    for after in parts[1:]:
        assert 'sdl2_window_presented(scon, ww, wh);' in after[:120], name

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef wchar_t WCHAR;
typedef void *HANDLE;
typedef struct window { int id; } SDL_Window, *HWND;
typedef int BOOL;
typedef int32_t HRESULT;
typedef unsigned long DWORD;
typedef const void *LPCVOID;
typedef void *HMODULE, *HRGN, *HDC, *HBRUSH, *HGDIOBJ;
typedef void (*FARPROC)(void);
typedef struct { long left, top, right, bottom; } RECT;
typedef uint64_t UINT64;
typedef struct { unsigned cbSize; UINT64 cFrame; char rest[300]; } DWM_TIMING_INFO;
#define WINAPI
#define TRUE 1
#define FALSE 0
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)
#define NULLREGION 1
#define SIMPLEREGION 2
#define BLACK_BRUSH 4
#define SDL_WINDOW_HIDDEN 8
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define QEMU_CLOCK_REALTIME 0
typedef struct { int version; struct { struct { HWND window; } win; } info; } SDL_SysWMinfo;
#define SDL_VERSION(v) (*(v) = 2)
struct sdl2_console { SDL_Window *real_window; int hidden; int64_t hold_ms, held_until;
    bool cloaked, cloak_framed, cloak_counted; uint64_t cloak_composed; int64_t cloak_shown, cloak_until, cloak_framed_at; int cloak_w, cloak_h; int cloak_rect[4]; };
static const char *env;
static int64_t clock_ms;
static int shows, props, prop_fails;
static const WCHAR *prop_name;
static const char *getenv_mock(const char *name) {
    assert(!strcmp(name, "OMARCHY_SDL_HOLD_WINDOWS_MS"));
    return env;
}
#define getenv getenv_mock
static int qemu_strtoi64(const char *s, const char **end, int base, int64_t *out) {
    char *stop;
    assert(!end && base == 10);
    long long value = strtoll(s, &stop, 10);
    if (stop == s || *stop) return -1;
    *out = value;
    return 0;
}
static int64_t qemu_clock_get_ms(int clock) { return clock_ms; }
static int SDL_GetWindowWMInfo(SDL_Window *w, SDL_SysWMinfo *info) {
    assert(info->version == 2);
    info->info.win.window = w;
    return 1;
}
static BOOL SetPropW(HWND hwnd, const WCHAR *name, HANDLE value) {
    if (prop_fails) return 0;
    assert(value == (HANDLE)1 && !props);
    prop_name = name; props++;
    return 1;
}
static HANDLE RemovePropW(HWND hwnd, const WCHAR *name) {
    assert(name == prop_name && props == 1);
    props--;
    return (HANDLE)1;
}
static void SDL_ShowWindow(SDL_Window *w) { shows++; }
/* The window as Windows and DWM see it. */
static int visible, region = 0, client_w = 800, client_h = 600, frame_x, frame_y, frame_left, cloak, cloak_calls, dwm_refuses, fills;
/* DWM's count of composed frames; timing_fails makes it unable to say. */
static UINT64 composed;
static int timing_fails;
static BOOL IsWindowVisible(HWND hwnd) { return visible; }
static HRGN CreateRectRgn(int a, int b, int c, int d) { return (HRGN)1; }
static int GetWindowRgn(HWND hwnd, HRGN r) { return region; }
static BOOL DeleteObject(HGDIOBJ o) { return 1; }
static BOOL GetClientRect(HWND hwnd, RECT *r) { r->left = r->top = 0; r->right = client_w; r->bottom = client_h; return 1; }
/* The window's rectangle: its client area, moved by frame_x and frame_y. */
static BOOL GetWindowRect(HWND hwnd, RECT *r) {
    r->left = -frame_x - frame_left; r->top = -frame_y; r->right = client_w + frame_x; r->bottom = client_h + frame_x;
    return 1;
}
static HDC GetDC(HWND hwnd) { return (HDC)hwnd; }
static int ReleaseDC(HWND hwnd, HDC dc) { return 1; }
static HGDIOBJ GetStockObject(int i) { assert(i == BLACK_BRUSH); return (HGDIOBJ)8; }
static int FillRect(HDC dc, const RECT *r, HBRUSH b) {
    assert(b == (HBRUSH)8 && r->right == client_w && r->bottom == client_h);
    fills++;
    return 1;
}
static HRESULT WINAPI set_attribute(HWND hwnd, DWORD attribute, LPCVOID value, DWORD size) {
    assert(attribute == 13 && size == sizeof(BOOL));
    cloak_calls++;
    if (dwm_refuses) return (HRESULT)0x80070005;
    cloak = *(const BOOL *)value;
    return 0;
}
static HRESULT WINAPI timing(HWND hwnd, DWM_TIMING_INFO *info) {
    assert(!hwnd && info->cbSize == sizeof(*info));
    if (timing_fails) return (HRESULT)0x80263001;
    info->cFrame = composed;
    return 0;
}
static HMODULE LoadLibraryW(const WCHAR *name) { assert(!wcscmp(name, L"dwmapi.dll")); return (HMODULE)1; }
static FARPROC GetProcAddress(HMODULE dll, const char *name) {
    if (!strcmp(name, "DwmSetWindowAttribute")) return (FARPROC)set_attribute;
    if (!strcmp(name, "DwmGetCompositionTimingInfo")) return (FARPROC)timing;
    return 0;
}
''' + body + r'''
static struct window window = {1};
static struct sdl2_console fresh(void) {
    struct sdl2_console scon = { .real_window = 0, .hidden = 0, .hold_ms = 7, .held_until = 9 };
    shows = props = prop_fails = 0;
    visible = 0; region = 0; client_w = 800; client_h = 600; frame_x = frame_y = frame_left = 0;
    cloak = cloak_calls = dwm_refuses = fills = timing_fails = 0;
    return scon;
}
static int create(struct sdl2_console *scon) {
    int flags = win_hold_flags(scon);
    scon->real_window = &window;
    win_hold_window(scon);
    return flags;
}
int main(void) {
    struct sdl2_console scon;
    /* Without the launcher's variable SDL shows the window as always. */
    const char *off[] = { NULL, "", "0", "-5", "abc", "10s", "1 " };
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        env = off[i];
        scon = fresh();
        assert(create(&scon) == 0 && !scon.hold_ms && !props && !scon.cloaked && !cloak_calls);
        clock_ms = 100000;
        win_hold_poll(&scon); win_hold_poll(&scon);
        win_hold_release(&scon, true);
        assert(!shows);
    }
    /* QEMU's own hidden consoles keep their state. */
    env = "10000";
    scon = fresh();
    scon.hidden = 1;
    assert(create(&scon) == 0 && !props && !shows);
    /* Held, then shown by the launcher: the mark goes, QEMU never shows it. */
    scon = fresh();
    assert(create(&scon) == SDL_WINDOW_HIDDEN && props == 1 && !wcscmp(prop_name, L"TryOmarchy.HeldWindow"));
    clock_ms = 5000;
    win_hold_poll(&scon);
    assert(scon.held_until == 15000 && !shows);
    win_hold_release(&scon, false);
    assert(!props && !scon.hold_ms);
    clock_ms = 99999;
    win_hold_poll(&scon);
    assert(!shows);
    /* Nobody shows it: the wait starts at the first round of messages, so a
     * slow start does not use it up, and QEMU shows the window once. */
    scon = fresh();
    create(&scon);
    clock_ms = 40000;
    win_hold_poll(&scon);
    clock_ms = 49999;
    win_hold_poll(&scon);
    assert(!shows && props == 1);
    clock_ms = 50000;
    win_hold_poll(&scon);
    assert(shows == 1 && !props && !scon.hold_ms);
    win_hold_poll(&scon);
    win_hold_release(&scon, false);
    assert(shows == 1);
    /* The wait is bounded. */
    env = "999999999";
    scon = fresh();
    create(&scon);
    assert(scon.hold_ms == 60000);
    /* A window that cannot be marked is shown at once, and not cloaked. */
    env = "10000";
    scon = fresh();
    prop_fails = 1;
    assert(create(&scon) == SDL_WINDOW_HIDDEN && shows == 1 && !scon.hold_ms);
    assert(!scon.cloaked && !cloak_calls);
    /* A window destroyed while held is not shown. */
    scon = fresh();
    create(&scon);
    win_hold_release(&scon, false);
    assert(!shows && !props);

    /* The curtain: held windows are cloaked until their first frame on screen. */
    clock_ms = 1000;
    scon = fresh();
    create(&scon);
    assert(scon.cloaked && cloak == 1);
    /* Hidden, then shown by the launcher with an empty region: frames drawn
     * then never count, and the cloak stays past the hold. */
    assert(!win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    visible = 1; region = NULLREGION;
    win_hold_release(&scon, false);
    clock_ms = 30000;
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && scon.cloaked && cloak && !fills);
    /* The region comes back: the surface goes black and a frame is asked for. */
    region = 0;
    assert(win_cloak_poll(&scon) && fills == 1 && cloak);
    assert(!win_cloak_poll(&scon) && cloak && fills == 1);
    /* A frame at another size than the window has does not count. */
    win_cloak_presented(&scon, 640, 480);
    assert(!win_cloak_poll(&scon) && cloak);
    /* One at its size does, once DWM has composed a frame since; nothing
     * waits for that. */
    composed = 40;
    win_cloak_presented(&scon, 800, 600);
    assert(cloak);
    assert(!win_cloak_poll(&scon) && cloak);
    assert(!win_cloak_poll(&scon) && cloak);
    composed = 41;
    assert(!win_cloak_poll(&scon) && !cloak && !scon.cloaked);
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && cloak_calls == 2);

    /* Resized after its frame (a resize in flight at the lift): it waits for
     * a frame at the new size. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    client_w = 1280; client_h = 720;
    assert(win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 1280, 720);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* The window gets its frame back, or is moved, after its frame: the
     * client size stays but the picture moves, so it waits for a frame
     * drawn after that. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    frame_x = 8; frame_y = 31;
    composed++;
    assert(win_cloak_poll(&scon) && cloak);
    composed++;
    assert(!win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && cloak);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);
    /* Only the top moves: a caption 32 px high comes back over a window
     * whose sides and bottom stay where they were. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    frame_y = 32;
    composed++;
    assert(win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 800, 600);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);
    /* Only the left edge moves. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    frame_left = 8;
    composed++;
    assert(win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 800, 600);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* The region goes empty again before a frame: frames then do not count
     * and the wait starts over. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon) && fills == 1);
    region = NULLREGION;
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && cloak);
    region = SIMPLEREGION;
    assert(win_cloak_poll(&scon) && fills == 2 && cloak);
    win_cloak_presented(&scon, 800, 600);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* No frame comes: shown black WIN_CLOAK_FRAME_WAIT_MS after it came on screen. */
    clock_ms = 2000;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon) && fills == 1);
    clock_ms = 2999;
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 3000;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* Never on screen (the launcher never lifts, or is gone): uncloaked
     * WIN_CLOAK_MAX_MS after it was cloaked. */
    clock_ms = 10000;
    scon = fresh();
    create(&scon);
    visible = 1; region = NULLREGION;
    clock_ms = 609999;
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 610000;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* Released the old way: QEMU shows it itself with no region, and it
     * shows with its first frame. */
    clock_ms = 0;
    scon = fresh();
    create(&scon);
    win_hold_poll(&scon);
    clock_ms = 10000;
    win_hold_poll(&scon);
    assert(shows == 1 && cloak);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* Ctrl+Alt+F and destroying the window uncloak at once. */
    scon = fresh();
    create(&scon);
    win_cloak_end(&scon);
    assert(!cloak && !scon.cloaked);
    win_cloak_end(&scon);
    assert(cloak_calls == 2);

    /* A refused uncloak is tried again on the next round. */
    clock_ms = 5;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    composed++;
    dwm_refuses = 1;
    assert(!win_cloak_poll(&scon) && cloak && scon.cloaked);
    dwm_refuses = 0;
    assert(!win_cloak_poll(&scon) && !cloak && !scon.cloaked);

    /* DWM composes nothing (session locked, display off): the window shows
     * at the time limit, and no round waited. */
    clock_ms = 5;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    clock_ms = 1004;
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 1005;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* DWM cannot say: never in the round that presented the frame, but the
     * first round WIN_CLOAK_UNCOUNTED_MS after it. */
    clock_ms = 5;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    timing_fails = 1;
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 38;
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 39;
    assert(!win_cloak_poll(&scon) && !cloak);
    /* It stops saying after the frame was counted. */
    clock_ms = 5;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    timing_fails = 1;
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 39;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* A guest that presents more often than WIN_CLOAK_UNCOUNTED_MS: the
     * first frame is the one waited for, later ones do not move the wait. */
    clock_ms = 5;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    timing_fails = 1;
    win_cloak_presented(&scon, 800, 600);
    clock_ms = 25;
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && cloak);
    clock_ms = 39;
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && !cloak);
    /* With the count: a present just before each round does not move the
     * count the wait started from. */
    clock_ms = 5;
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    composed++;
    win_cloak_presented(&scon, 800, 600);
    assert(!win_cloak_poll(&scon) && !cloak);
    /* Resized after the frame, with a frame at the new size presented before
     * the round sees the resize: the round asks for a frame, and the one at
     * the new size counts. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    win_cloak_presented(&scon, 800, 600);
    client_w = 1280; client_h = 720;
    win_cloak_presented(&scon, 1280, 720);
    composed++;
    assert(win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 1280, 720);
    assert(!win_cloak_poll(&scon) && cloak);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* The region empties and comes back between two rounds, with a frame
     * presented while it was empty: that frame does not count. */
    scon = fresh();
    create(&scon);
    visible = 1;
    assert(win_cloak_poll(&scon));
    region = NULLREGION;
    win_cloak_presented(&scon, 800, 600);
    region = 0;
    composed++;
    assert(!win_cloak_poll(&scon) && cloak);
    win_cloak_presented(&scon, 800, 600);
    composed++;
    assert(!win_cloak_poll(&scon) && !cloak);

    /* DWM refuses the cloak: the window behaves as before. */
    scon = fresh();
    dwm_refuses = 1;
    create(&scon);
    assert(!scon.cloaked && !cloak && cloak_calls == 1);
    visible = 1;
    assert(!win_cloak_poll(&scon) && !fills);
    win_cloak_presented(&scon, 800, 600);
    win_cloak_end(&scon);
    assert(cloak_calls == 1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sdl-held-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness, encoding='utf-8')
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Werror', str(work / 'test.c'), '-o', str(work / 'test.exe')], check=True)
    subprocess.run([str(work / 'test.exe')], check=True)
print('ok - held windows: opt-in, own hidden consoles, launcher show, bounded fallback from the first messages, failed mark;'
      ' cloaked until a frame at their size on screen, black and bounded without one, every exit uncloaks, refused cloak')
