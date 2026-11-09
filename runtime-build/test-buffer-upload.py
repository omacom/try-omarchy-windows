#!/usr/bin/env python3
"""Compile actual buffer uploads and iovec helpers against fake GL calls."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched virglrenderer source directory')
args = parser.parse_args()
renderer = (args.source / 'src/vrend/vrend_renderer.c').read_text(encoding='utf-8')
iov = (args.source / 'src/vrend/iov.c').read_text(encoding='utf-8')


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


init = function(renderer, 'int vrend_renderer_init(')
assert 'vrend_state.intel_windows_buffer_uploads = vrend_intel_windows_buffer_uploads();' in init
transfer = renderer.index('static int vrend_renderer_transfer_write_iov_unsynced(')
buffer = renderer.index('if (has_bit(res->storage_bits, VREND_STORAGE_GL_BUFFER))', transfer)
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if TEST_WINDOWS
#ifndef _WIN32
#define _WIN32 1
#endif
#else
#undef _WIN32
#endif
typedef unsigned int GLuint;
typedef unsigned int GLenum;
#define GL_VENDOR 0x1f00
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER 0x8892
#define GL_MAP_WRITE_BIT 2
#define GL_MAP_INVALIDATE_RANGE_BIT 4
#define GL_MAP_UNSYNCHRONIZED_BIT 32
#define VREND_STORAGE_GL_BUFFER 4
#define VREND_STORAGE_GL_IMMUTABLE 64
#define has_bit(value, bit) (((value) & (bit)) != 0)
struct iovec { void *iov_base; size_t iov_len; };
typedef void (*iov_cb)(void *, unsigned int, void *, int);
struct pipe_box { int x, y, z, width, height, depth; };
struct vrend_transfer_info { uint64_t offset; struct pipe_box *box; bool synchronized; };
struct vrend_resource { unsigned storage_bits; GLuint gl_id; GLenum target; };
struct virgl_sub_upload_data { GLenum target; struct pipe_box *box; };
static const char *vendor;
static unsigned char storage[64];
static unsigned bound, maps, uploads, unmaps, failures, access_flags, vendor_reads;
static bool fail_map, fail_alloc;
static const void *uploaded_from;
static void *test_malloc(size_t size) { return fail_alloc ? NULL : malloc(size); }
#define malloc(size) test_malloc(size)
static struct { bool intel_windows_buffer_uploads; } vrend_state;
static const unsigned char *glGetString(GLenum key) {
    assert(key == GL_VENDOR); vendor_reads++; return (const unsigned char *)vendor;
}
static void glBindBufferARB(GLenum target, GLuint id) {
    assert(target == GL_ELEMENT_ARRAY_BUFFER || target == GL_ARRAY_BUFFER);
    bound = id;
}
static void *glMapBufferRange(GLenum target, size_t offset, size_t size, unsigned flags) {
    (void)target; assert(bound == 50 && offset + size <= sizeof(storage));
    maps++; access_flags = flags;
    return fail_map ? NULL : storage + offset;
}
static void glBufferSubData(GLenum target, size_t offset, size_t size, const void *data) {
    (void)target; assert(bound == 50 && offset + size <= sizeof(storage));
    uploads++; uploaded_from = data; memcpy(storage + offset, data, size);
}
static void glUnmapBuffer(GLenum target) { (void)target; unmaps++; }
#define virgl_error(...) (failures++)
'''
harness += function(iov, 'size_t vrend_read_from_iovec(')
harness += function(iov, 'size_t vrend_read_from_iovec_cb(')
harness += function(renderer, 'static void iov_buffer_upload(')
harness += function(renderer, 'static bool iov_buffer_upload_once(')
harness += function(renderer, 'static bool vrend_intel_windows_buffer_uploads(')
harness += '''
static void upload(struct vrend_resource *res, const struct iovec *iov,
                   int num_iovs, const struct vrend_transfer_info *info) {
    void *data;
'''
harness += body(renderer, buffer) + '\n}\n'
harness += r'''
static void check(const char *driver, GLenum target, bool immutable, bool synchronized,
                  bool failed, unsigned expected_maps, bool no_memory) {
    unsigned char a[] = "abcdef", b[] = "ghijklmnop";
    struct iovec vectors[] = {{a, 6}, {b, 0}, {b, 10}};
    struct pipe_box box = {.x = 5, .width = 12};
    struct vrend_transfer_info info = {.offset = 2, .box = &box, .synchronized = synchronized};
    struct vrend_resource resource = {VREND_STORAGE_GL_BUFFER |
        (immutable ? VREND_STORAGE_GL_IMMUTABLE : 0), 50, target};
    /* Renderer init reads the vendor once; uploads only read the flag. */
    vendor = driver; vendor_reads = 0;
    vrend_state.intel_windows_buffer_uploads = TEST_WINDOWS && vrend_intel_windows_buffer_uploads();
    fail_map = failed; fail_alloc = no_memory;
    bound = maps = uploads = unmaps = failures = access_flags = vendor_reads = 0;
    memset(storage, 0x7e, sizeof(storage));
    upload(&resource, vectors, 3, &info);
    assert(maps == expected_maps);
    assert(!memcmp(storage + 5, "cdefghijklmn", 12));
    for (unsigned i = 0; i < sizeof(storage); i++)
        if (i < 5 || i >= 17) assert(storage[i] == 0x7e);
    assert(bound == 0);
    /* The sub-data path gathers the pages into one call unless it is out of memory. */
    if (!expected_maps) assert(uploads == (no_memory ? 2u : 1u) && unmaps == 0);
    else if (failed) assert(uploads == 2 && unmaps == 0);
    else assert(uploads == 0 && unmaps == 1);
    if (expected_maps) {
        assert(access_flags == (GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT |
                                (synchronized ? 0 : GL_MAP_UNSYNCHRONIZED_BIT)));
    }
    assert(failures == (failed && expected_maps ? 1u : 0u));
    assert(vendor_reads == 0);
}
static void check_one_page(void) {
    /* A range inside one page is sent straight from guest memory. */
    unsigned char a[] = "abcdefghij";
    struct iovec vectors[] = {{a, 0}, {a, 4}, {a + 4, 6}};
    struct pipe_box box = {.x = 3, .width = 5};
    struct vrend_transfer_info info = {.offset = 5, .box = &box};
    struct vrend_resource resource = {VREND_STORAGE_GL_BUFFER, 50, GL_ARRAY_BUFFER};
    vrend_state.intel_windows_buffer_uploads = TEST_WINDOWS;
    fail_map = fail_alloc = false;
    bound = maps = uploads = unmaps = failures = 0; uploaded_from = NULL;
    memset(storage, 0x7e, sizeof(storage));
    upload(&resource, vectors, 3, &info);
    assert(!memcmp(storage + 3, "fghij", 5));
    if (TEST_WINDOWS) assert(maps == 0 && uploads == 1 && uploaded_from == a + 5);
    else assert(maps == 1 && uploads == 0);
}
int main(void) {
    /* Captured Intel Windows index and vertex uploads must avoid the map call. */
    check("Intel", GL_ELEMENT_ARRAY_BUFFER, false, false, false, !TEST_WINDOWS, false);
    check("Intel", GL_ELEMENT_ARRAY_BUFFER, false, false, false, !TEST_WINDOWS, true);
    check("Intel", GL_ELEMENT_ARRAY_BUFFER, false, true, false, !TEST_WINDOWS, false);
    check("Intel", GL_ELEMENT_ARRAY_BUFFER, true, false, false, 1, false);
    check("Intel", GL_ARRAY_BUFFER, false, false, false, !TEST_WINDOWS, false);
    check("Intel", GL_ARRAY_BUFFER, true, false, false, 1, false);
    check("AMD", GL_ELEMENT_ARRAY_BUFFER, false, false, false, 1, false);
    check(NULL, GL_ELEMENT_ARRAY_BUFFER, false, false, false, 1, false);
    check("AMD", GL_ELEMENT_ARRAY_BUFFER, false, true, true, 1, false);
    check_one_page();
    puts("ok - mutable uploads, one call per upload, immutable storage, other vendors, Linux, partial multi-iovec writes and fallbacks");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-buffer-upload-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    for windows in (1, 0):
        binary = test / f'test-{windows}.exe'
        subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                        '-Wno-unused-function', '-Werror', f'-DTEST_WINDOWS={windows}',
                        str(test / 'test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
