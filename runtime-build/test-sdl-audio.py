#!/usr/bin/env python3
"""Compile the actual SDL route helper and exercise direction and failure paths."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
source = parser.parse_args().source.read_text(encoding='utf-8')
helper = source[source.index('static SDL_AudioDeviceID sdl_open_selected_device('):
                source.index('static SDL_AudioDeviceID sdl_open(')]
harness = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <string.h>
typedef unsigned SDL_AudioDeviceID;
typedef struct { int dummy; } SDL_AudioSpec;
static int calls, warnings, clears, expected_rec;
static bool default_fails;
static const char *expected_name, *requested_output, *requested_input;
static const char *test_getenv(const char *key) {
    return !strcmp(key, "OMARCHY_SDL_OUTPUT_DEVICE_NAME") ? requested_output : requested_input;
}
/* Windows GLib rejects invalid UTF-8 when setting the real environment.
 * Inject the getter result so that branch is actually exercised on Windows. */
#define g_getenv test_getenv
#define warn_report(...) (warnings++)
static void SDL_ClearError(void) { clears++; }
static SDL_AudioDeviceID SDL_OpenAudioDevice(const char *name, int rec,
        SDL_AudioSpec *req, SDL_AudioSpec *obt, int changes) {
    assert(rec == expected_rec && req && obt && changes == 0);
    calls++;
    if (name) { assert(expected_name && !strcmp(name, expected_name)); }
    if (name && !strcmp(name, "disconnected")) return 0;
    return default_fails ? 0 : 17;
}
''' + helper + r'''
int main(void) {
    SDL_AudioSpec req={0}, obt={0};
    assert(sdl_open_selected_device(&req,&obt,0)==17 && calls==1);
    requested_output="Speakers, USB";
    requested_input="Microphone";
    expected_name="Speakers, USB";
    assert(sdl_open_selected_device(&req,&obt,0)==17 && calls==2);
    expected_name="Microphone";expected_rec=1;
    assert(sdl_open_selected_device(&req,&obt,1)==17 && calls==3);
    requested_input="disconnected";
    expected_name="disconnected";
    assert(sdl_open_selected_device(&req,&obt,1)==17 && calls==5);
    assert(warnings==1 && clears==1);
    default_fails=true;
    assert(sdl_open_selected_device(&req,&obt,1)==0 && calls==7);
    assert(warnings==2 && clears==2);
    default_fails=false;expected_name=NULL;
    requested_input="";
    assert(sdl_open_selected_device(&req,&obt,1)==17 && calls==8);
    requested_input="\xff";
    assert(sdl_open_selected_device(&req,&obt,1)==17 && calls==9);
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
print('ok - independent SDL routes, missing-device fallback, open failures and invalid UTF-8')
