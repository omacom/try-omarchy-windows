#!/usr/bin/env python3
"""Compile the imported image pitch copy against fake GL calls (#394)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched virglrenderer source directory')
args = parser.parse_args()
renderer = (args.source / 'src/vrend/vrend_renderer.c').read_text(encoding='utf-8')


def body(text, start):
    opening = text.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[opening:end]


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    return text[start:opening] + body(text, opening)


# The decision is made once at init, after the Intel driver check.
init = function(renderer, 'int vrend_renderer_init(')
intel = init.index('vrend_state.intel_windows_buffer_uploads = vrend_intel_windows_buffer_uploads();')
pitch = init.index('vrend_pitch_copy_imports(vrend_state.intel_windows_buffer_uploads)')
assert intel < pitch

# Imports with a wider pitch view the memory one pitch wide and keep gl_id as
# a plain texture of the image's size; other imports are unchanged.
# Its preprocessor branches leave braces unbalanced, so slice to the next function.
set_type = renderer[renderer.index('\nvrend_renderer_pipe_resource_set_type('):
                    renderer.index('uint32_t vrend_renderer_resource_get_map_info(')]
wide = set_type[set_type.index('if (pitch_width > width) {'):set_type.index('} else {', set_type.index('if (pitch_width > width) {'))]
assert 'glTexStorageMem2DEXT(gr->target, 1, internalformat, pitch_width, height, mem_object, 0);' in wide
assert 'glTexStorage2D(gr->target, 1, internalformat, width, height);' in wide
assert 'vrend_pitch_copy_count++;' in wide
assert 'glTexStorageMem2DEXT(gr->target, 1, internalformat, width, height, mem_object, 0);' in set_type

# Every read refreshes the copy first: draws, blits, copies and readback.
draw = function(renderer, 'int vrend_draw_vbo(')
assert draw.index('vrend_draw_refresh_pitch_copies(sub_ctx);') < draw.index('vrend_draw_bind_objects(sub_ctx,')
assert 'vrend_resource_refresh_pitch_copy(src_res);' in function(renderer, 'void vrend_renderer_blit(')
assert 'vrend_resource_refresh_pitch_copy(src_res);' in function(renderer, 'void vrend_renderer_resource_copy_region(')
assert 'vrend_resource_refresh_pitch_copy(res);' in function(renderer, 'static int vrend_renderer_transfer_send_iov(')
destroy = function(renderer, 'void vrend_renderer_resource_destroy(')
assert 'glDeleteTextures(1, &res->pitch_src_id);' in destroy and 'vrend_pitch_copy_count--;' in destroy

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int GLuint;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
#define GL_TEXTURE_2D 0x0de1
#define PIPE_TEXTURE_2D 2
#define PIPE_TEXTURE_3D 3
#define PIPE_SHADER_VERTEX 0
#define PIPE_SHADER_FRAGMENT 1
#define VIRGL_GBM_MAX_PLANES 4
enum features { feat_copy_image, feat_texture_storage };
static bool features[2];
#define has_feature(f) (features[f])
struct pipe_resource { int target; unsigned format; unsigned width0, height0; unsigned nr_samples; };
struct vrend_resource { struct pipe_resource base; GLuint gl_id; GLenum target; GLuint pitch_src_id; };
struct vrend_renderer_resource_set_type_args {
    uint32_t format, bind, width, height, usage; uint64_t modifier; uint32_t plane_count;
    uint32_t plane_strides[VIRGL_GBM_MAX_PLANES]; uint32_t plane_offsets[VIRGL_GBM_MAX_PLANES];
};
struct vrend_sampler_view { struct vrend_resource *texture; };
struct vrend_shader_view { struct vrend_sampler_view *views[32]; };
struct vrend_linked_shader_program { uint32_t samplers_used_mask[2]; };
struct vrend_sub_context { struct vrend_linked_shader_program *prog; int last_shader_idx;
                           struct vrend_shader_view views[2]; };
static struct { bool pitch_copy_imports; } vrend_state;
static unsigned util_format_get_blocksize(unsigned format) { return format; }
static int u_bit_scan(uint32_t *mask) { int i = __builtin_ctz(*mask); *mask &= *mask - 1; return i; }
static const char *env_value;
static const char *test_getenv(const char *name) {
    assert(!strcmp(name, "TRYOMARCHY_PITCH_COPY")); return env_value;
}
#define getenv(name) test_getenv(name)
static unsigned copies;
static GLuint copy_src, copy_dst;
static GLsizei copy_w, copy_h, copy_d;
static void glCopyImageSubData(GLuint src, GLenum st, GLint sl, GLint sx, GLint sy, GLint sz,
                               GLuint dst, GLenum dt, GLint dl, GLint dx, GLint dy, GLint dz,
                               GLsizei w, GLsizei h, GLsizei d) {
    assert(st == GL_TEXTURE_2D && dt == GL_TEXTURE_2D);
    assert(!sl && !sx && !sy && !sz && !dl && !dx && !dy && !dz);
    copies++; copy_src = src; copy_dst = dst; copy_w = w; copy_h = h; copy_d = d;
}
'''
harness += 'static uint32_t vrend_pitch_copy_count;\n'
harness += function(renderer, 'static bool vrend_pitch_copy_imports(')
harness += function(renderer, 'static GLsizei vrend_import_pitch_width(')
harness += function(renderer, 'static void vrend_resource_refresh_pitch_copy(')
harness += function(renderer, 'static void vrend_draw_refresh_pitch_copies(')
harness += r'''
static GLsizei width_for(unsigned w, unsigned h, unsigned stride, unsigned planes,
                         uint64_t map_size, int target, unsigned samples) {
    struct vrend_resource gr = {{target, 4, w, h, samples}, 0, GL_TEXTURE_2D, 0};
    struct vrend_renderer_resource_set_type_args a = {.width = w, .height = h, .plane_count = planes};
    a.plane_strides[0] = stride;
    return vrend_import_pitch_width(&gr, &a, map_size);
}
int main(void) {
    /* Intel on Windows decides by default; the variable overrides either way. */
    env_value = NULL;  assert(vrend_pitch_copy_imports(true) && !vrend_pitch_copy_imports(false));
    env_value = "on";  assert(vrend_pitch_copy_imports(false));
    env_value = "off"; assert(!vrend_pitch_copy_imports(true));
    env_value = "yes"; assert(vrend_pitch_copy_imports(true) && !vrend_pitch_copy_imports(false));

    features[feat_copy_image] = features[feat_texture_storage] = true;
    vrend_state.pitch_copy_imports = true;
    const uint64_t big = 1u << 30;
    /* The reported widths: a 256-byte pitch makes 1000 px rows 1024 px wide. */
    assert(width_for(1000, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 1024);
    assert(width_for(1001, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 1024);
    assert(width_for(813, 700, 3328, 1, big, PIPE_TEXTURE_2D, 0) == 832);
    assert(width_for(1921, 1081, 7936, 1, big, PIPE_TEXTURE_2D, 0) == 1984);
    /* Packed rows, as at 1024 px, keep the direct import. */
    assert(width_for(1024, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 0);
    /* A pitch narrower than the row, not whole texels, too far off, or past
     * the memory keeps the direct import; so do other shapes and planes. */
    assert(width_for(1000, 700, 3968, 1, big, PIPE_TEXTURE_2D, 0) == 0);
    assert(width_for(1000, 700, 4098, 1, big, PIPE_TEXTURE_2D, 0) == 0);
    assert(width_for(1000, 700, 4 * (1000 + 4097), 1, big, PIPE_TEXTURE_2D, 0) == 0);
    assert(width_for(1000, 700, 4096, 1, 4096u * 700 - 1, PIPE_TEXTURE_2D, 0) == 0);
    assert(width_for(1000, 700, 4096, 1, 4096u * 700, PIPE_TEXTURE_2D, 0) == 1024);
    assert(width_for(1000, 700, 4096, 2, big, PIPE_TEXTURE_2D, 0) == 0);
    assert(width_for(1000, 700, 4096, 1, big, PIPE_TEXTURE_3D, 0) == 0);
    assert(width_for(1000, 700, 4096, 1, big, PIPE_TEXTURE_2D, 4) == 0);
    assert(width_for(0, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 0);
    /* Off, or without copy_image or texture storage, imports stay direct. */
    vrend_state.pitch_copy_imports = false;
    assert(width_for(1000, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 0);
    vrend_state.pitch_copy_imports = true; features[feat_copy_image] = false;
    assert(width_for(1000, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 0);
    features[feat_copy_image] = true; features[feat_texture_storage] = false;
    assert(width_for(1000, 700, 4096, 1, big, PIPE_TEXTURE_2D, 0) == 0);

    /* A refresh copies exactly the visible image from the pitch view. */
    struct vrend_resource copied = {{PIPE_TEXTURE_2D, 4, 1000, 700, 0}, 11, GL_TEXTURE_2D, 12};
    struct vrend_resource direct = {{PIPE_TEXTURE_2D, 4, 1024, 700, 0}, 13, GL_TEXTURE_2D, 0};
    vrend_resource_refresh_pitch_copy(&copied);
    assert(copies == 1 && copy_src == 12 && copy_dst == 11 && copy_w == 1000 && copy_h == 700 && copy_d == 1);
    vrend_resource_refresh_pitch_copy(&direct);
    vrend_resource_refresh_pitch_copy(NULL);
    assert(copies == 1);

    /* A draw refreshes each sampled copy, every time, and nothing else. */
    struct vrend_sampler_view a = {&copied}, b = {&direct}, unused = {&copied};
    struct vrend_linked_shader_program prog = {{0, (1u << 0) | (1u << 3)}};
    struct vrend_sub_context sub = {&prog, PIPE_SHADER_FRAGMENT, {{{0}}, {{0}}}};
    sub.views[PIPE_SHADER_FRAGMENT].views[0] = &a;
    sub.views[PIPE_SHADER_FRAGMENT].views[3] = &b;
    sub.views[PIPE_SHADER_FRAGMENT].views[5] = &unused;
    copies = 0; vrend_pitch_copy_count = 0;
    vrend_draw_refresh_pitch_copies(&sub);
    assert(copies == 0);
    vrend_pitch_copy_count = 1;
    vrend_draw_refresh_pitch_copies(&sub);
    vrend_draw_refresh_pitch_copies(&sub);
    assert(copies == 2);
    prog.samplers_used_mask[PIPE_SHADER_FRAGMENT] = 1u << 2;
    vrend_draw_refresh_pitch_copies(&sub);
    assert(copies == 2);
    puts("ok - pitch copy decision, widths, bounds, refresh on draws and direct imports");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-image-pitch-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    binary = test / 'test.exe'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-Werror', str(test / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
