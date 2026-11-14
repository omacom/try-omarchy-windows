#!/usr/bin/env python3
"""Exercise the held SDL windows the launcher shows, with mocked SDL and Win32."""
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
#define SDL_WINDOW_HIDDEN 8
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define QEMU_CLOCK_REALTIME 0
typedef struct { int version; struct { struct { HWND window; } win; } info; } SDL_SysWMinfo;
#define SDL_VERSION(v) (*(v) = 2)
struct sdl2_console { SDL_Window *real_window; int hidden; int64_t hold_ms, held_until; };
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
''' + body + r'''
static struct window window = {1};
static struct sdl2_console fresh(void) {
    struct sdl2_console scon = { .real_window = 0, .hidden = 0, .hold_ms = 7, .held_until = 9 };
    shows = props = prop_fails = 0;
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
        assert(create(&scon) == 0 && !scon.hold_ms && !props);
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
    /* A window that cannot be marked is shown at once. */
    env = "10000";
    scon = fresh();
    prop_fails = 1;
    assert(create(&scon) == SDL_WINDOW_HIDDEN && shows == 1 && !scon.hold_ms);
    /* A window destroyed while held is not shown. */
    scon = fresh();
    create(&scon);
    win_hold_release(&scon, false);
    assert(!shows && !props);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sdl-held-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness, encoding='utf-8')
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Werror', str(work / 'test.c'), '-o', str(work / 'test.exe')], check=True)
    subprocess.run([str(work / 'test.exe')], check=True)
print('ok - held windows: opt-in, own hidden consoles, launcher show, bounded fallback from the first messages, failed mark')
