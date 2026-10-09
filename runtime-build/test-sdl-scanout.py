#!/usr/bin/env python3
"""Exercise actual SDL teardown helpers with a delayed scanout-disable event."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
source = parser.parse_args().source.read_text(encoding='utf-8')
mode = source[source.index('static void sdl2_set_scanout_mode('):
              source.index('static void sdl2_gl_render_surface(')]
switch = source[source.index('void sdl2_gl_switch('):
                source.index('void sdl2_gl_refresh(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define WINQ_TRACE_ENTER(...) 0
#define WINQ_TRACE_EXIT(call, ...) do { (void)(call); } while (0)
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
typedef struct { bool placeholder; int width, height; } DisplaySurface;
typedef struct { int con; } DisplayChangeListener;
struct sdl2_console {
    bool scanout_mode, opengl;
    int guest_fb, real_window, winctx;
    void *gls;
    DisplaySurface *surface;
    DisplayChangeListener dcl;
};
static void egl_fb_destroy(int *fb) { *fb = 0; }
static void surface_gl_destroy_texture(void *gls, DisplaySurface *surface) {}
static void surface_gl_create_texture(void *gls, DisplaySurface *surface) { assert(gls); }
static int SDL_GL_MakeCurrent(int window, int context) { return 0; }
static bool surface_is_placeholder(DisplaySurface *surface) { return surface->placeholder; }
static int qemu_console_get_index(int con) { return con; }
static void qemu_gl_fini_shader(void *gls) {}
static void *qemu_gl_init_shader(void) { return (void *)1; }
static void sdl2_window_destroy(struct sdl2_console *s) { s->real_window = s->winctx = 0; }
static void sdl2_window_create(struct sdl2_console *s) { s->real_window = s->winctx = 1; }
static void sdl2_window_resize(struct sdl2_console *s) {}
static int surface_width(DisplaySurface *s) { return s->width; }
static int surface_height(DisplaySurface *s) { return s->height; }
''' + mode + switch + r'''
int main(void) {
    DisplaySurface active = {false, 1280, 720}, disabled = {true, 640, 480};
    struct sdl2_console s = {.opengl = true, .real_window = 1, .winctx = 1,
        .gls = (void *)1, .surface = &active, .dcl = {.con = 1}};
    for (int repeat = 0; repeat < 3; repeat++) {
        s.scanout_mode = true;
        s.guest_fb = 7;
        sdl2_gl_switch(&s.dcl, &disabled);
        /* A delayed disable must not recreate a texture with a dead shader. */
        sdl2_set_scanout_mode(&s, false);
        assert(!s.guest_fb && !s.scanout_mode && !s.gls && !s.real_window);
        sdl2_gl_switch(&s.dcl, &active);
        assert(s.gls && s.real_window && s.surface == &active);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-scanout-') as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(harness, encoding='utf-8')
    subprocess.run(['gcc', '-std=gnu11', '-O2', str(root / 'test.c'),
                    '-o', str(root / 'test.exe')], check=True)
    subprocess.run([str(root / 'test.exe')], check=True)
print('ok - secondary SDL scanout teardown, delayed disable and reactivation')
