#!/usr/bin/env python3
"""Compile actual SDL capture paths with fake endpoints and deterministic clocks."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
cases = ('unplug-between-get-put', 'reacquire-between-get-put',
         'failed-open-backoff', 'route-open-backoff')
parser.add_argument('--case', choices=cases, help='run one regression case')
args = parser.parse_args()
root = args.source
source = (root / 'audio/sdlaudio.c').read_text(encoding='utf-8')
mixeng = (root / 'audio/audio-mixeng-be.c').read_text(encoding='utf-8')
internal = (root / 'audio/audio_int.h').read_text(encoding='utf-8')

def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]

harness = r'''
#include <assert.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define _WIN32 1
#define QEMU_CLOCK_VIRTUAL 0
#define NANOSECONDS_PER_SECOND 1000000000
#define trace_audio_rate_reset(...) ((void)0)
static int64_t virtual_ns;
static gint64 host_us;
static gint64 fake_monotonic_time(void) { return host_us; }
#define g_get_monotonic_time fake_monotonic_time
static int64_t qemu_clock_get_ns(int clock) { return virtual_ns; }
static uint64_t muldiv64(uint64_t a, uint64_t b, uint64_t c) {
    return (uint64_t)(((__uint128_t)a * b) / c);
}
static uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }
static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
typedef enum {
    AUDIO_FORMAT_U8, AUDIO_FORMAT_U16, AUDIO_FORMAT_U32,
    AUDIO_FORMAT_S8, AUDIO_FORMAT_S16, AUDIO_FORMAT_S32, AUDIO_FORMAT_F32,
    AUDIO_FORMAT__MAX
} AudioFormat;
struct audio_pcm_info {
    int bytes_per_frame, bytes_per_second, nchannels;
    AudioFormat af;
    bool swap_endianness;
};
typedef struct {
    struct audio_pcm_info info;
    size_t size_emul, pos_emul, pending_emul;
    unsigned char *buf_emul;
} HWVoiceIn;
typedef void Audiodev;
typedef unsigned SDL_AudioDeviceID;
typedef unsigned char Uint8;
typedef struct {
    int freq, format, channels, samples;
    void *callback, *userdata;
} SDL_AudioSpec;
#define SDL_AUDIO_STOPPED 0
#define SDL_AUDIO_PLAYING 1
#define SDL_AUDIO_PAUSED 2
static int capture_count, enumerations, opens, closes, failures, status;
static bool fail_open, incompatible;
static gint64 failed_open_us;
static const char *last_name;
#define warn_report(...) ((void)0)
#define error_report(...) (failures++)
static void SDL_ClearError(void) {}
static const char *SDL_GetError(void) { return "unavailable"; }
static int SDL_GetNumAudioDevices(int rec) {
    assert(rec == 1); enumerations++; return capture_count;
}
static const char *SDL_GetAudioDeviceName(int index, int rec) {
    assert(rec == 1); return "Microphone";
}
static bool sdl_default_endpoint_missing(int rec) {
    assert(rec == 1); return false;
}
static SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int rec,
        SDL_AudioSpec *req, SDL_AudioSpec *obt, int changes) {
    /* Preflight must avoid opening an empty inventory. */
    opens++; assert(rec == 1 && capture_count > 0 && changes == 0);
    last_name = name;
    if (fail_open || (name && strcmp(name, "Microphone"))) {
        host_us += failed_open_us; /* real failed activation can take eight seconds */
        return 0;
    }
    *obt = *req;
    if (incompatible) { obt->freq++; }
    status = SDL_AUDIO_PAUSED;
    return 17;
}
static int SDL_GetAudioDeviceStatus(SDL_AudioDeviceID id) {
    assert(id == 17); return status;
}
static void SDL_PauseAudioDevice(SDL_AudioDeviceID id, int paused) {
    assert(id == 17); status = paused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING;
}
static void SDL_CloseAudioDevice(SDL_AudioDeviceID id) {
    assert(id == 17); closes++; status = SDL_AUDIO_STOPPED;
}
static void SDL_LockAudioDevice(SDL_AudioDeviceID id) { assert(id == 17); }
static void SDL_UnlockAudioDevice(SDL_AudioDeviceID id) { assert(id == 17); }
static size_t audio_ring_posb(size_t pos, size_t pending, size_t size) {
    return (pos + size - pending) % size;
}
#define xglue(a, b) a##b
#define glue(a, b) xglue(a, b)
#define MAKE_IDENTIFIER(x) glue(x, __COUNTER__)
#define MIN_INTERNAL(a, b, _a, _b) \
    ({ typeof(1 ? (a) : (b)) _a = (a), _b = (b); _a < _b ? _a : _b; })
#undef MIN
#define MIN(a, b) MIN_INTERNAL((a), (b), MAKE_IDENTIFIER(_a), MAKE_IDENTIFIER(_b))
''' + section(internal, 'typedef struct RateCtl {', 'void audio_rate_start(') + section(
    source, 'typedef struct SDLVoiceIn {', 'static int aud_to_sdlfmt') + section(
    mixeng, 'void audio_pcm_info_clear_buf(', '/*') + section(
    mixeng, 'void audio_rate_start(', 'static const TypeInfo audio_types[]') + section(
    mixeng, 'void *audio_generic_get_buffer_in(', 'void audio_generic_initialize_buffer_out(') + section(
    source, 'static char *sdl_initial_device_name(', 'static void sdl_close_out(') + section(
    source, 'static void sdl_callback_in(', '/*') + section(
    source, 'static void sdl_release_in(', '#define SDL_WRAPPER_FUNC') + section(
    source, '#define SDL_WRAPPER_FUNC', 'SDL_WRAPPER_FUNC(buffer_get_free') + section(
    source, 'SDL_WRAPPER_FUNC(get_buffer_in,', 'static void sdl_fini_out') + section(
    source, 'static void sdl_enable_in(', 'static bool audio_sdl_realize(') + r'''
static void advance(int milliseconds) {
    host_us += milliseconds * 1000;
    virtual_ns += milliseconds * 1000000;
}
static size_t consume(SDLVoiceIn *sdl, unsigned char expected) {
    size_t total = 0;
    for (int i = 0; i < 4; i++) {
        size_t size = sdl->hw.size_emul;
        unsigned char *buf = sdl_get_buffer_in(&sdl->hw, &size);
        for (size_t n = 0; n < size; n++) { assert(buf[n] == expected); }
        sdl_put_buffer_in(&sdl->hw, buf, size);
        total += size;
        if (!size) { break; }
    }
    return total;
}
int main(int argc, char **argv) {
    unsigned char storage[8192], captured[1920];
    SDLVoiceIn sdl = {0};
    sdl.hw.info = (struct audio_pcm_info){4, 192000, 2, AUDIO_FORMAT_S16, false};
    sdl.hw.size_emul = sizeof(storage);
    sdl.hw.buf_emul = storage;
    sdl.reopen_spec = (SDL_AudioSpec){48000, 1, 2, 512, NULL, NULL};
    g_unsetenv("OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY");
    memset(storage, 0x55, sizeof(storage));

    if (argc == 2 && !strcmp(argv[1], "unplug-between-get-put")) {
        capture_count = 1;
        sdl_enable_in(&sdl.hw, true);
        memset(captured, 0x23, sizeof(captured));
        sdl_callback_in(&sdl, captured, sizeof(captured));
        size_t size = sizeof(captured);
        void *buf = sdl_get_buffer_in(&sdl.hw, &size);
        assert(size == sizeof(captured));
        status = SDL_AUDIO_STOPPED;
        capture_count = 0;
        /* Unplug must not discard the outstanding buffer before consumption. */
        sdl_put_buffer_in(&sdl.hw, buf, size);
        assert(sdl.hw.pending_emul == 0 && sdl.devid == 17 && closes == 0);
        assert(consume(&sdl, 0) == 0);
        assert(!sdl.devid && closes == 1);
        advance(10); assert(consume(&sdl, 0) == 1920);
        puts("ok - unplug between get_buffer_in and put_buffer_in");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "reacquire-between-get-put")) {
        sdl_enable_in(&sdl.hw, true);
        advance(10);
        size_t size = sizeof(captured);
        void *buf = sdl_get_buffer_in(&sdl.hw, &size);
        assert(size == sizeof(captured));
        capture_count = 1;
        advance(1000); /* the retry becomes due while a silence buffer is held */
        sdl_put_buffer_in(&sdl.hw, buf, size);
        assert(sdl.hw.pending_emul == 0 && !sdl.devid && opens == 0);
        assert(consume(&sdl, 0) == 0);
        assert(sdl.devid == 17 && opens == 1);
        memset(captured, 0, sizeof(captured));
        sdl_callback_in(&sdl, captured, sizeof(captured));
        assert(consume(&sdl, 0) == sizeof(captured));
        puts("ok - reacquire deferred until after put_buffer_in");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "failed-open-backoff")) {
        capture_count = 1;
        fail_open = true;
        failed_open_us = 8 * G_USEC_PER_SEC;
        sdl_enable_in(&sdl.hw, true);
        assert(opens == 1 && failures == 1 && !sdl.devid);
        /* The ten-second deadline starts after the slow failed activation. */
        assert(sdl.next_route_check == host_us + 10 * G_USEC_PER_SEC);
        for (int i = 0; i < 999; i++) {
            advance(10); assert(consume(&sdl, 0) == 1920);
            sdl_enable_in(&sdl.hw, false); sdl_enable_in(&sdl.hw, true);
        }
        assert(opens == 1);
        advance(10); consume(&sdl, 0);
        assert(opens == 2 && failures == 1 && !sdl.devid); /* reported once */
        assert(sdl.next_route_check == host_us + 10 * G_USEC_PER_SEC);
        fail_open = false;
        advance(10000); consume(&sdl, 0);
        assert(opens == 3 && sdl.devid == 17);
        /* Successful capture clears the long backoff for an empty unplug. */
        status = SDL_AUDIO_STOPPED; capture_count = 0;
        consume(&sdl, 0);
        assert(sdl.next_route_check == host_us + G_USEC_PER_SEC);
        capture_count = 1;
        advance(999); consume(&sdl, 0); assert(opens == 3);
        advance(1); consume(&sdl, 0); assert(opens == 4 && sdl.devid == 17);
        puts("ok - failed listed endpoint backs off ten seconds after open, resets on success");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "route-open-backoff")) {
        capture_count = 1;
        sdl_enable_in(&sdl.hw, true);
        g_autofree char *directory = g_dir_make_tmp("tryomarchy-capture-XXXXXX", NULL);
        assert(directory);
        g_autofree char *input = g_build_filename(directory, "input", NULL);
        g_setenv("OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY", directory, TRUE);
        assert(g_file_set_contents(input, "bWlzc2luZw==\n", -1, NULL));
        fail_open = true;
        failed_open_us = 8 * G_USEC_PER_SEC;
        /* An unlisted selection goes straight to the default: one open. */
        advance(250); consume(&sdl, 0);
        assert(opens == 2 && failures == 1 && sdl.devid == 17);
        assert(sdl.next_route_check == host_us + 10 * G_USEC_PER_SEC);
        advance(9999); consume(&sdl, 0); assert(opens == 2);
        advance(1); consume(&sdl, 0); assert(opens == 3 && failures == 1);
        fail_open = false;
        failed_open_us = 0;
        advance(10000); consume(&sdl, 0);
        assert(opens == 4 && sdl.devid == 17 && !sdl.route_matches);
        assert(sdl.next_route_check == host_us + 250 * 1000);
        assert(g_remove(input) == 0);
        assert(g_rmdir(directory) == 0);
        puts("ok - failed live capture route opens use the same ten-second backoff");
        return 0;
    }
    assert(argc == 1);

    /* No host mic: no WASAPI open, and capture periods advance at 48 kHz. */
    sdl_enable_in(&sdl.hw, true);
    assert(sdl.enabled && !sdl.devid && opens == 0 && enumerations == 1);
    assert(consume(&sdl, 0) == 0);
    for (int i = 0; i < 50; i++) {
        advance(10);
        assert(consume(&sdl, 0) == 1920);
        assert(consume(&sdl, 0) == 0); /* no clock advance, no extra samples */
    }
    assert(enumerations == 1 && opens == 0);
    int before = enumerations;
    for (int i = 0; i < 10; i++) {
        sdl_enable_in(&sdl.hw, false);
        sdl_enable_in(&sdl.hw, true);
    }
    assert(enumerations == before && opens == 0); /* start cannot bypass backoff */

    /* Enumeration errors also avoid an open; later active hotplug recovers. */
    capture_count = -1;
    advance(500); consume(&sdl, 0);
    assert(opens == 0 && enumerations == before + 1);
    capture_count = 1;
    advance(1000); consume(&sdl, 0);
    assert(opens == 1 && sdl.devid == 17 && status == SDL_AUDIO_PLAYING);
    memset(captured, 0x23, sizeof(captured));
    sdl_callback_in(&sdl, captured, sizeof(captured));
    assert(consume(&sdl, 0x23) == sizeof(captured));

    /* Last mic unplugged during recording: close stopped device and resume silence. */
    status = SDL_AUDIO_STOPPED; capture_count = 0;
    consume(&sdl, 0);
    assert(closes == 1 && !sdl.devid && opens == 1);
    advance(10); assert(consume(&sdl, 0) == 1920);
    sdl_enable_in(&sdl.hw, false);
    advance(2000); capture_count = 1;
    assert(consume(&sdl, 0) == 0 && opens == 1); /* disabled mic stays closed */

    /* Nonempty inventory, failed open: ten-second backoff across stream starts. */
    fail_open = true;
    sdl_enable_in(&sdl.hw, true);
    assert(opens == 2 && failures == 1 && !sdl.devid);
    for (int i = 0; i < 20; i++) {
        advance(10); assert(consume(&sdl, 0) == 1920);
        sdl_enable_in(&sdl.hw, false); sdl_enable_in(&sdl.hw, true);
    }
    assert(opens == 2);
    fail_open = false;
    advance(10000); consume(&sdl, 0);
    assert(opens == 3 && sdl.devid == 17);
    sdl_enable_in(&sdl.hw, false);

    /* Missing selected route still falls back; reconnect returns to the selection. */
    sdl.device_name = g_strdup("missing");
    advance(250);
    sdl_enable_in(&sdl.hw, true);
    assert(sdl.devid == 17 && !sdl.route_matches && !last_name);
    sdl_enable_in(&sdl.hw, false);
    g_free(sdl.device_name); sdl.device_name = g_strdup("Microphone");
    advance(250);
    sdl_enable_in(&sdl.hw, true);
    assert(sdl.route_matches && !strcmp(last_name, "Microphone"));
    sdl_enable_in(&sdl.hw, false);

    /* Silence uses the PCM format, including unsigned 8-bit, and wraps the ring. */
    capture_count = 0;
    sdl.hw.info = (struct audio_pcm_info){1, 48000, 1, AUDIO_FORMAT_U8, false};
    sdl_enable_in(&sdl.hw, true);
    sdl.hw.pos_emul = sizeof(storage) - 5;
    advance(10); assert(consume(&sdl, 0x80) == 480);
    assert(sdl.hw.pos_emul == 475);
    sdl_enable_in(&sdl.hw, false);
    assert(sdl.hw.pending_emul == 0);
    g_free(sdl.device_name);
    puts("ok - no-mic capture, paced silence, retry backoff, active hotplug, unplug, idle gating, route fallback and unsigned PCM wrap");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-capture-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-Werror', str(test / 'test.c'), '-o', str(test / 'test'),
                    *flags], check=True)
    selected = (args.case,) if args.case else (None, *cases)
    for case in selected:
        command = [str(test / 'test')]
        if case:
            command.append(case)
        subprocess.run(command, check=True)
