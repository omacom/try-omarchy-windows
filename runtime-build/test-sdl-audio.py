#!/usr/bin/env python3
"""Compile the patched SDL route parser and exercise live control inputs."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
source = parser.parse_args().source.read_text(encoding='utf-8')
playback_init = source[source.index('static int sdl_init_out('):
                       source.index('static void sdl_enable_out(')]
assert 'sdl_open(' not in playback_init
assert 'SDL_OpenAudioDevice(' not in playback_init
assert 'obt = req;' in playback_init
helper = source[source.index('static char *sdl_initial_device_name('):
                source.index('static void sdl_close_out(')]
harness = r'''
#include <assert.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define _WIN32 1
typedef unsigned SDL_AudioDeviceID;
typedef struct {
    int freq, format, channels, samples;
    void *callback, *userdata;
} SDL_AudioSpec;
static int opens, warnings, clears, failures;
static int playback_count = 2, capture_count = 1;
static bool default_missing, fail_default;
static const char *last_name;
static int last_rec;
static const char *available_name = "Speakers, USB";
#define warn_report(...) (warnings++)
#define error_report(...) (failures++)
static void SDL_ClearError(void) { clears++; }
static const char *SDL_GetError(void) { return "unavailable"; }
static int SDL_GetNumAudioDevices(int rec) { return rec ? capture_count : playback_count; }
static bool sdl_default_endpoint_missing(int rec) { (void)rec; return default_missing; }
static const char *SDL_GetAudioDeviceName(int index, int rec) {
    return rec ? "Microphone, USB" : index ? available_name : "Speakers";
}
static SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int rec,
        SDL_AudioSpec *req, SDL_AudioSpec *obt, int changes) {
    assert(req && obt && changes == 0);
    opens++;
    last_name = name;
    last_rec = rec;
    *obt = *req;
    if (!name && fail_default) {
        return 0;
    }
    return name && !strcmp(name, "missing") ? 0 : 17;
}
''' + helper + r'''
int main(void) {
    SDL_AudioSpec req={48000, 1, 2, 512, 0, 0}, obt={0};
    bool matched, present;
    char *value;
    gint64 next = 0;
    g_autofree char *directory = g_dir_make_tmp("tryomarchy-audio-XXXXXX", NULL);
    assert(directory);
    g_autofree char *output = g_build_filename(directory, "output", NULL);

    g_setenv("OMARCHY_SDL_OUTPUT_DEVICE_NAME", "Speakers, USB", TRUE);
    value = sdl_initial_device_name(0);
    assert(value && !strcmp(value, "Speakers, USB"));
    g_free(value);
    g_unsetenv("OMARCHY_SDL_INPUT_DEVICE_NAME");
    assert(!sdl_initial_device_name(1));

    g_setenv("OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY", directory, TRUE);
    assert(!sdl_route_file_device_name(0, &present) && !present);
    assert(g_file_set_contents(output, "U3BlYWtlcnMsIFVTQg==\n", -1, NULL));
    value = sdl_route_file_device_name(0, &present);
    assert(present && value && !strcmp(value, "Speakers, USB"));
    g_free(value);
    assert(g_file_set_contents(output, "default\n", -1, NULL));
    assert(!sdl_route_file_device_name(0, &present) && present);
    assert(g_file_set_contents(output, "AA==\n", -1, NULL));
    assert(!sdl_route_file_device_name(0, &present) && !present);

    assert(sdl_route_check_due(&next));
    assert(!sdl_route_check_due(&next));
    assert(sdl_named_device_available(NULL, 0));
    assert(sdl_named_device_available("Speakers, USB", 0));
    assert(!sdl_named_device_available("missing", 0));
    assert(sdl_named_device_available("Microphone, USB", 1));
    assert(!sdl_named_device_available("Speakers, USB", 1));
    available_name = "reconnected";
    assert(sdl_named_device_available("reconnected", 0));
    assert(sdl_audio_specs_match(&req, &req));
    obt = req; obt.freq++;
    assert(!sdl_audio_specs_match(&req, &obt));

    available_name = "Speakers, USB";
    assert(sdl_open(&req, &obt, 0, "Speakers, USB", &matched) == 17);
    assert(matched && opens == 1 && last_rec == 0 &&
           !strcmp(last_name, "Speakers, USB"));
    /* A device that left SDL's inventory skips SDL's eight-second wait. */
    assert(sdl_open(&req, &obt, 1, "missing", &matched) == 17);
    assert(!matched && opens == 2 && last_rec == 1 && !last_name);
    assert(clears == 0 && failures == 0);

    /* A listed device that still fails falls back to the default. */
    available_name = "missing";
    assert(sdl_open(&req, &obt, 0, "missing", &matched) == 17);
    assert(!matched && opens == 4 && !last_name && clears == 1);
    available_name = "reconnected";

    /* No playback devices: no SDL open, one report per outage. */
    playback_count = 0;
    warnings = 0;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0 && !matched);
    assert(sdl_open(&req, &obt, 0, "Speakers", &matched) == 0 && !matched);
    assert(opens == 4 && warnings == 1 && failures == 0);
    /* An empty capture inventory stays silent. */
    capture_count = 0;
    assert(sdl_open(&req, &obt, 1, NULL, &matched) == 0 && !matched);
    assert(opens == 4 && warnings == 1);
    capture_count = 1;
    playback_count = 2;

    /* The default endpoint is missing although devices are listed. */
    default_missing = true;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0 && !matched);
    assert(opens == 4 && warnings == 1);
    default_missing = false;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 17 && matched);
    assert(opens == 5);
    default_missing = true;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0);
    assert(opens == 5 && warnings == 2);
    /* A missing endpoint does not hide a different open failure. */
    default_missing = false;
    fail_default = true;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0);
    assert(opens == 6 && warnings == 2 && failures == 1);
    fail_default = false;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 17 && opens == 7);

    /* Other open failures are reported once until an open succeeds. */
    fail_default = true;
    failures = 0;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0);
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0);
    assert(opens == 9 && failures == 1);
    fail_default = false;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 17);
    fail_default = true;
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 0);
    assert(failures == 2);
    fail_default = false;

    /* A listed selection that fails must not fall back into SDL's wait
     * when Windows has no default either. */
    assert(sdl_open(&req, &obt, 0, NULL, &matched) == 17);
    available_name = "missing";
    default_missing = true;
    warnings = 0;
    int before = opens;
    assert(sdl_open(&req, &obt, 0, "missing", &matched) == 0 && !matched);
    assert(opens == before + 1 && !strcmp(last_name, "missing"));
    assert(warnings == 2); /* selection fell back, default unavailable */
    default_missing = false;
    remove(output);
    g_rmdir(directory);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-audio-') as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(harness, encoding='utf-8')
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    subprocess.run(['gcc', '-std=gnu11', '-O2', str(root / 'test.c'),
                    '-o', str(root / 'test.exe'), *flags], check=True)
    subprocess.run([str(root / 'test.exe')], check=True)
print('ok - independent SDL routes, control file parsing, missing-device fallback, non-blocking missing endpoints and route polling')
