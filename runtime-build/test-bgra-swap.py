#!/usr/bin/env python3
"""Compile the imported BGRA sampler swizzle against fake views (#400)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched virglrenderer source directory')
args = parser.parse_args()
renderer = (args.source / 'src/vrend/vrend_renderer.c').read_text(encoding='utf-8')

# The switch is read once at init, next to the pitch copy decision.
init_start = renderer.index('const char *bgra_swap = getenv("TRYOMARCHY_BGRA_SWAP");')
init = renderer[init_start:renderer.index(';', renderer.index('vrend_state.memobj_bgra_swap =', init_start)) + 1]

# The swap in vrend_create_sampler_view, up to the separate sRGB branch that
# only memory objects take.
start = renderer.index('const bool memobj = has_bit(res->storage_bits, VREND_STORAGE_GL_MEMOBJ);')
end = renderer.index('} else if (res->is_imported &&', start)
swap = renderer[start:end] + '}\n'
assert 'view->srgb_decode = GL_SKIP_DECODE_EXT;' in swap

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int GLint;
#define GL_RED 0x1903
#define GL_GREEN 0x1904
#define GL_BLUE 0x1905
#define GL_ALPHA 0x1906
#define GL_ZERO 0
#define GL_ONE 1
#define GL_DECODE_EXT 0x8a49
#define GL_SKIP_DECODE_EXT 0x8a4a
#define VREND_STORAGE_GL_TEXTURE (1u << 1)
#define VREND_STORAGE_GL_MEMOBJ (1u << 7)
#define has_bit(mask, bit) (((mask) & (bit)) != 0)
enum virgl_formats { RGBA8 = 1, BGRA8 = 2, BGRX8 = 3, RGBX8 = 4 };
static bool vrend_format_is_bgra(enum virgl_formats f) { return f == BGRA8 || f == BGRX8; }
struct pipe_resource { enum virgl_formats format; };
struct vrend_resource { struct pipe_resource base; bool is_imported; uint32_t storage_bits; };
struct vrend_sampler_view { struct vrend_resource *texture; enum virgl_formats format;
                            GLint gl_swizzle[4]; GLint srgb_decode; };
static struct { bool memobj_bgra_swap; } vrend_state;
static const char *env_value;
static const char *test_getenv(const char *name) {
    assert(!strcmp(name, "TRYOMARCHY_BGRA_SWAP")); return env_value;
}
#define getenv(name) test_getenv(name)
static void read_switch(void) {
'''
harness += init + '\n}\n'
harness += r'''
static void sample(struct vrend_resource *res, struct vrend_sampler_view *view) {
'''
harness += swap + '}\n'
harness += r'''
static const GLint rgba[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
static const GLint bgra[4] = {GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA};
/* entered: the import was handled here, so sRGB decoding is skipped too. */
static bool view_reads(uint32_t storage, bool imported, enum virgl_formats res_format,
                       enum virgl_formats view_format, const GLint in[4], const GLint want[4],
                       bool entered) {
    struct vrend_resource res = {{res_format}, imported, storage};
    struct vrend_sampler_view view = {&res, view_format, {in[0], in[1], in[2], in[3]}, GL_DECODE_EXT};
    sample(&res, &view);
    return !memcmp(view.gl_swizzle, want, sizeof(view.gl_swizzle)) &&
           view.srgb_decode == (entered ? GL_SKIP_DECODE_EXT : GL_DECODE_EXT);
}
int main(void) {
    const uint32_t memobj = VREND_STORAGE_GL_TEXTURE | VREND_STORAGE_GL_MEMOBJ;
    const uint32_t egl = VREND_STORAGE_GL_TEXTURE;
    /* The switch is on unless it says off. */
    env_value = NULL;  read_switch(); assert(vrend_state.memobj_bgra_swap);
    env_value = "on";  read_switch(); assert(vrend_state.memobj_bgra_swap);
    env_value = "0";   read_switch(); assert(vrend_state.memobj_bgra_swap);
    env_value = "off"; read_switch(); assert(!vrend_state.memobj_bgra_swap);

    vrend_state.memobj_bgra_swap = true;
    /* A BGRA memory object sampled as BGRA swaps R and B, BGRX too. */
    assert(view_reads(memobj, true, BGRA8, BGRA8, rgba, bgra, true));
    assert(view_reads(memobj, true, BGRX8, BGRX8, rgba, bgra, true));
    /* Selections are swapped, not entries: constants and alpha stay put. */
    const GLint rgb1[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ONE};
    const GLint bgr1[4] = {GL_BLUE, GL_GREEN, GL_RED, GL_ONE};
    assert(view_reads(memobj, true, BGRA8, BGRA8, rgb1, bgr1, true));
    const GLint b0r0[4] = {GL_BLUE, GL_ZERO, GL_RED, GL_ZERO};
    const GLint r0b0[4] = {GL_RED, GL_ZERO, GL_BLUE, GL_ZERO};
    assert(view_reads(memobj, true, BGRA8, BGRA8, b0r0, r0b0, true));
    const GLint gga1[4] = {GL_GREEN, GL_GREEN, GL_ALPHA, GL_ONE};
    assert(view_reads(memobj, true, BGRA8, BGRA8, gga1, gga1, true));
    /* A memory object viewed as RGBA reads the bytes as they are. */
    assert(view_reads(memobj, true, BGRA8, RGBA8, rgba, rgba, false));
    /* RGBA memory objects and textures that were not imported are untouched. */
    assert(view_reads(memobj, true, RGBA8, RGBA8, rgba, rgba, false));
    assert(view_reads(memobj, false, BGRA8, BGRA8, rgba, rgba, false));
    assert(view_reads(egl, false, BGRA8, BGRA8, rgba, rgba, false));
    /* The Linux EGL path swaps every view of a BGRA import, as before. */
    assert(view_reads(egl, true, BGRA8, BGRA8, rgba, bgra, true));
    assert(view_reads(egl, true, BGRA8, RGBA8, rgba, bgra, true));
    assert(view_reads(egl, true, RGBA8, RGBA8, rgba, rgba, false));

    /* Off restores the old memory object sampling and leaves EGL alone. */
    vrend_state.memobj_bgra_swap = false;
    assert(view_reads(memobj, true, BGRA8, BGRA8, rgba, rgba, false));
    assert(view_reads(egl, true, BGRA8, BGRA8, rgba, bgra, true));
    puts("ok - BGRA swap switch, memory object BGRA views, constant swizzles, RGBA views, EGL path");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-bgra-swap-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    binary = test / 'test.exe'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-Werror', str(test / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
