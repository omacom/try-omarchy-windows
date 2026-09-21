#!/usr/bin/env python3
"""Exercise actual SDL window creation after a secondary GL context is lost."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
source = parser.parse_args().source.read_text(encoding='utf-8')
create = source[source.index('void sdl2_window_create('):
                source.index('void sdl2_window_destroy(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define SDL_WINDOW_FULLSCREEN_DESKTOP 1
#define SDL_WINDOW_RESIZABLE 2
#define SDL_WINDOW_HIDDEN 4
#define SDL_WINDOW_OPENGL 8
#define SDL_WINDOWPOS_UNDEFINED 0
#define DISPLAY_GL_MODE_ES 1
#define SDL_HINT_RENDER_DRIVER "driver"
#define SDL_HINT_RENDER_BATCHING "batching"
#define SDL_GL_SHARE_WITH_CURRENT_CONTEXT 1
struct options { int gl; };
struct sdl2_console {
    void *surface;
    bool hidden, opengl;
    int real_window, winctx, real_renderer;
    struct options *opts;
};
static struct sdl2_console consoles[3];
static struct sdl2_console *sdl2_console = consoles;
static int gui_fullscreen, current_context, sharing, next_window = 10;
static int surface_width(void *s) { return 800; }
static int surface_height(void *s) { return 600; }
static int SDL_CreateWindow(const char *s, int x, int y, int w, int h, int f) {
    return ++next_window;
}
static void SDL_SetHint(const char *s, const char *v) {}
static int SDL_GL_MakeCurrent(int window, int context) {
    assert(window == consoles[0].real_window);
    assert(context == consoles[0].winctx);
    current_context = context;
    return 0;
}
static int SDL_GL_SetAttribute(int attr, int value) {
    assert(attr == SDL_GL_SHARE_WITH_CURRENT_CONTEXT);
    sharing = value;
    return 0;
}
static int SDL_GL_CreateContext(int window) {
    /* A texture made by the primary is visible only in its share group. */
    if (window != consoles[0].real_window && consoles[0].winctx) {
        assert(sharing && current_context == consoles[0].winctx);
    }
    return 100 + window;
}
static void SDL_GL_SetSwapInterval(int interval) {}
static int SDL_CreateRenderer(int window, int index, int flags) { return 1; }
static void sdl_update_caption(struct sdl2_console *s) {}
static void win_pinch_create(struct sdl2_console *s) {}
''' + create + r'''
int main(void) {
    struct options opts = {0};
    for (int i = 0; i < 3; i++) {
        consoles[i].surface = (void *)1;
        consoles[i].opts = &opts;
        consoles[i].opengl = true;
    }
    sdl2_window_create(&consoles[0]);
    for (int repeat = 0; repeat < 3; repeat++) {
        for (int i = 1; i < 3; i++) {
            current_context = 0; /* SDL_GL_MakeCurrent(NULL, NULL) at switch. */
            sharing = 0;
            consoles[i].real_window = consoles[i].winctx = 0;
            sdl2_window_create(&consoles[i]);
            assert(consoles[i].real_window && consoles[i].winctx);
        }
    }
    /* Software display creation must not acquire a GL context. */
    consoles[1].real_window = consoles[1].winctx = 0;
    consoles[1].opengl = false;
    current_context = 0;
    sdl2_window_create(&consoles[1]);
    assert(!consoles[1].winctx && !current_context);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-gl-context-') as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(harness, encoding='utf-8')
    subprocess.run(['gcc', '-std=gnu11', '-O2', str(root / 'test.c'),
                    '-o', str(root / 'test.exe')], check=True)
    subprocess.run([str(root / 'test.exe')], check=True)
print('ok - secondary GL contexts share primary textures after recreation')
