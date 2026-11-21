#!/usr/bin/env python3
"""Compile the GLSL 4.30 request for storage-buffer shaders against fake configs (#383)."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched virglrenderer source directory')
args = parser.parse_args()
renderer = (args.source / 'src/vrend/vrend_renderer.c').read_text(encoding='utf-8')
shader = (args.source / 'src/vrend/vrend_shader.c').read_text(encoding='utf-8')

# The switch is read once at init, next to the other runtime switches.
init_start = renderer.index('const char *ssbo_glsl430 = getenv("TRYOMARCHY_SSBO_GLSL430");')
init = renderer[init_start:renderer.index(';', renderer.index('vrend_state.ssbo_glsl430 =', init_start)) + 1]
assert 'grctx->shader_cfg.ssbo_glsl430 = vrend_state.ssbo_glsl430;' in renderer

# The request in vrend_convert_shader. It needs the scan's file counts and has
# to come before the declarations are converted, so none of them picks a
# pre-1.40 built-in name first.
start = shader.index('   if (cfg->ssbo_glsl430 &&')
end = shader.index(';', start) + 1
request = shader[start:end]
convert = shader.index('bool vrend_convert_shader(')
scan = shader.index('if (!tgsi_scan_shader(tokens, &ctx.info))', convert)
assert scan < start < shader.index('if (!allocate_strbuffers(&ctx.glsl_strbufs))', scan)

# Invariant inputs, which GLSL 4.30 rejects, from the loop in iter_instruction.
inv_start = shader.index('         for (uint32_t i = 0; i < ctx->num_inputs; ++i) {',
                         shader.index("GLES doesn't allow invariant specifiers on inputs"))
inv_end = shader.index('\n      }\n', shader.index('ctx->inputs[i].invariant = 0;', inv_start)) + 1
invariant = shader[inv_start:inv_end]

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define TGSI_FILE_BUFFER 3
#define TGSI_FILE_COUNT 8
struct vrend_shader_cfg {
    uint32_t glsl_version : 12;
    uint32_t use_gles : 1;
    uint32_t use_core_profile : 1;
    uint32_t ssbo_glsl430 : 1;
};
struct tgsi_shader_info { unsigned file_count[TGSI_FILE_COUNT]; };
struct vrend_shader_key { uint32_t force_invariant_inputs[4]; };
struct vrend_shader_io { int name; int sid; bool invariant; };
struct dump_ctx {
    int glsl_ver_required;
    struct tgsi_shader_info info;
    const struct vrend_shader_key *key;
    uint32_t num_inputs;
    struct vrend_shader_io inputs[2];
};
static uint32_t varying_bit_from_semantic_and_index(int name, int sid) { (void)name; return (uint32_t)sid; }
static struct { bool ssbo_glsl430; } vrend_state;
static const char *env_value;
static const char *getenv(const char *name) { (void)name; return env_value; }
static int require_glsl_ver(const struct dump_ctx *ctx, int glsl_ver)
{
    return glsl_ver > ctx->glsl_ver_required ? glsl_ver : ctx->glsl_ver_required;
}
static void read_switch(void) {
''' + init + r'''
}
static int version_for(bool on, bool gles, bool core, int host, unsigned buffers, int before)
{
    struct vrend_shader_cfg c = {0};
    struct vrend_shader_cfg *cfg = &c;
    struct dump_ctx ctx = {0};
    c.ssbo_glsl430 = on;
    c.use_gles = gles;
    c.use_core_profile = core;
    c.glsl_version = host;
    ctx.glsl_ver_required = before;
    ctx.info.file_count[TGSI_FILE_BUFFER] = buffers;
''' + request + r'''
    return ctx.glsl_ver_required;
}
static bool input_invariant(int version, bool forced)
{
    struct vrend_shader_key key = {{forced ? 1u : 0u}};
    struct dump_ctx c = {0};
    struct dump_ctx *ctx = &c;
    c.key = &key;
    c.glsl_ver_required = version;
    c.num_inputs = 1;
    c.inputs[0].invariant = true;
''' + invariant + r'''
    return c.inputs[0].invariant;
}
int main(void)
{
    /* The switch is on unless it says off. */
    env_value = NULL;  read_switch(); assert(vrend_state.ssbo_glsl430);
    env_value = "on";  read_switch(); assert(vrend_state.ssbo_glsl430);
    env_value = "0";   read_switch(); assert(vrend_state.ssbo_glsl430);
    env_value = "off"; read_switch(); assert(!vrend_state.ssbo_glsl430);

    /* A core-profile host with GLSL 4.30 or later gets #version 430. */
    assert(version_for(true, false, true, 460, 1, 140) == 430);
    assert(version_for(true, false, true, 430, 2, 130) == 430);
    /* A shader that already needs more keeps its version. */
    assert(version_for(true, false, true, 460, 1, 450) == 450);
    /* Without storage buffers nothing changes. */
    assert(version_for(true, false, true, 460, 0, 140) == 140);
    /* Older hosts, GLES and compatibility contexts keep the extension path. */
    assert(version_for(true, false, true, 420, 1, 140) == 140);
    assert(version_for(true, true, true, 460, 1, 140) == 140);
    assert(version_for(true, false, false, 460, 1, 140) == 140);
    /* Off restores the old version choice. */
    assert(version_for(false, false, true, 460, 1, 140) == 140);
    /* Inputs matching an invariant output stay invariant below GLSL 4.30
     * only; 4.30 rejects the qualifier on inputs. */
    assert(input_invariant(140, true));
    assert(!input_invariant(140, false));
    assert(!input_invariant(430, true));
    assert(!input_invariant(450, true));
    puts("ok - SSBO GLSL 4.30 switch, core hosts, existing versions, no buffers, older hosts, GLES, compatibility, invariant inputs");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-ssbo-glsl430-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    binary = test / 'test.exe'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-Werror', str(test / 'test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
