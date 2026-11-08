#!/usr/bin/env python3
"""Compile the SDL playback start and route paths with fake endpoints and clocks."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched audio/sdlaudio.c')
source = parser.parse_args().source.read_text(encoding='utf-8')


def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


harness = r'''
#include <assert.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define _WIN32 1
static gint64 host_us;
static gint64 fake_monotonic_time(void) { return host_us; }
#define g_get_monotonic_time fake_monotonic_time
typedef struct { int unused; } HWVoiceOut;
typedef void Audiodev;
typedef unsigned SDL_AudioDeviceID;
typedef struct {
    int freq, format, channels, samples;
    void *callback, *userdata;
} SDL_AudioSpec;
#define SDL_AUDIO_STOPPED 0
#define SDL_AUDIO_PLAYING 1
#define SDL_AUDIO_PAUSED 2
static int playback_count = 1, opens, closes, failures, status;
static bool fail_open, default_missing;
static gint64 failed_open_us;
#define warn_report(...) ((void)0)
#define error_report(...) (failures++)
static void SDL_ClearError(void) {}
static const char *SDL_GetError(void) { return "unavailable"; }
static int SDL_GetNumAudioDevices(int rec) {
    assert(rec == 0); return playback_count;
}
static const char *SDL_GetAudioDeviceName(int index, int rec) {
    assert(rec == 0); return "Speakers";
}
static bool sdl_default_endpoint_missing(int rec) {
    assert(rec == 0); return default_missing;
}
static SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int rec,
        SDL_AudioSpec *req, SDL_AudioSpec *obt, int changes) {
    /* The fail-fast checks must keep SDL away from a missing endpoint. */
    opens++; assert(rec == 0 && playback_count > 0 && !default_missing);
    if (fail_open) {
        host_us += failed_open_us; /* a failed activation can take eight seconds */
        return 0;
    }
    *obt = *req;
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
''' + section('typedef struct SDLVoiceOut {', 'typedef struct SDLVoiceIn {') + section(
    'static char *sdl_initial_device_name(', 'static void sdl_close_out(') + section(
    'static void sdl_release_out(', '/* The recording counterpart of sdl_release_out(). */') + section(
    'static void sdl_enable_out(', 'static void sdl_fini_in(') + r'''
static void advance(int milliseconds) { host_us += milliseconds * 1000; }
/* Every guest buffer callback refreshes the route first. */
static void poll_route(SDLVoiceOut *sdl) { sdl_refresh_route_Out(sdl); }
/* One short guest sound, such as the terminal bell, at a given spacing. */
static void chime(SDLVoiceOut *sdl, int spacing) {
    advance(spacing);
    sdl_enable_out(&sdl->hw, true);
    poll_route(sdl);
    advance(10);
    poll_route(sdl);
    sdl_enable_out(&sdl->hw, false);
}
int main(void) {
    SDLVoiceOut sdl = {0};
    sdl.reopen_spec = (SDL_AudioSpec){48000, 1, 2, 512, NULL, NULL};
    g_unsetenv("OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY");
    host_us = 100 * G_USEC_PER_SEC;

    /* Normal sounds open at once, close afterwards and reopen immediately. */
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 1 && sdl.devid == 17 && status == SDL_AUDIO_PLAYING);
    advance(300); poll_route(&sdl);
    sdl_enable_out(&sdl.hw, false);
    assert(closes == 1 && !sdl.devid);
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 2 && sdl.devid == 17); /* no route-poll delay on a start */
    sdl_enable_out(&sdl.hw, false);

    /* A slow failed open is not repeated by route polls or new sounds. */
    fail_open = true;
    failed_open_us = 8 * G_USEC_PER_SEC;
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 3 && failures == 1 && !sdl.devid);
    assert(sdl.next_open_attempt == host_us + 10 * G_USEC_PER_SEC);
    for (int i = 0; i < 400; i++) {
        advance(10); poll_route(&sdl);
    }
    sdl_enable_out(&sdl.hw, false);
    for (int i = 0; i < 50; i++) { chime(&sdl, 100); }
    assert(opens == 3);
    advance(10000 - 4000 - 50 * 110);
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 4 && failures == 1 && !sdl.devid); /* reported once */
    sdl_enable_out(&sdl.hw, false);

    /* Recovery clears the backoff, so the next sound opens at once. */
    fail_open = false;
    advance(10000);
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 5 && sdl.devid == 17 && !sdl.next_open_attempt);
    sdl_enable_out(&sdl.hw, false);
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 6 && sdl.devid == 17);
    sdl_enable_out(&sdl.hw, false);

    /* A missing default fails without SDL and is checked again each second. */
    default_missing = true;
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 6 && !sdl.devid);
    assert(sdl.next_open_attempt == host_us + G_USEC_PER_SEC);
    sdl_enable_out(&sdl.hw, false);
    for (int i = 0; i < 100; i++) { chime(&sdl, 100); }
    assert(opens == 6 && !sdl.devid);
    default_missing = false;
    chime(&sdl, 1000);
    assert(opens == 7);

    /* No playback devices: one-second checks keep hotplug responsive. */
    playback_count = 0;
    sdl_enable_out(&sdl.hw, true);
    assert(opens == 7 && sdl.next_open_attempt == host_us + G_USEC_PER_SEC);
    advance(999); poll_route(&sdl); assert(opens == 7);
    playback_count = 1;
    advance(250); poll_route(&sdl);
    assert(opens == 8 && sdl.devid == 17 && status == SDL_AUDIO_PLAYING);

    /* A failed live route change keeps the open device and backs off. */
    g_autofree char *directory = g_dir_make_tmp("tryomarchy-playback-XXXXXX", NULL);
    assert(directory);
    g_autofree char *output = g_build_filename(directory, "output", NULL);
    g_setenv("OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY", directory, TRUE);
    assert(g_file_set_contents(output, "U3BlYWtlcnM=\n", -1, NULL)); /* Speakers */
    fail_open = true;
    failed_open_us = 0;
    advance(250); poll_route(&sdl);
    assert(opens == 10 && sdl.devid == 17 && status == SDL_AUDIO_PLAYING);
    for (int i = 0; i < 39; i++) { advance(250); poll_route(&sdl); }
    assert(opens == 10);
    fail_open = false;
    advance(250); poll_route(&sdl);
    assert(opens == 11 && closes == 6 && sdl.devid == 17 && sdl.route_matches);
    assert(!strcmp(sdl.device_name, "Speakers") && status == SDL_AUDIO_PLAYING);
    sdl_enable_out(&sdl.hw, false);
    assert(g_remove(output) == 0 && g_rmdir(directory) == 0);
    g_free(sdl.device_name);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-playback-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra',
                    '-Wno-unused-parameter', '-Wno-unused-function',
                    '-Werror', str(test / 'test.c'), '-o', str(test / 'test'),
                    *flags], check=True)
    subprocess.run([str(test / 'test')], check=True)
print('ok - playback opens at once, backs off after failed or missing endpoints, and recovers')
