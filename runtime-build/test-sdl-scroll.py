#!/usr/bin/env python3
"""Compile the patched SDL wheel handler with portable transport stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
root = parser.parse_args().source
source = (root / 'ui/sdl2.c').read_text()
handler = source[source.index('static void sdl_send_wheel_buttons('):
                 source.index('static void handle_windowevent(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "sdl2-scroll.h"
#ifndef PRECISE
#define PRECISE 1
#endif
#define SDL_VERSION_ATLEAST(a,b,c) PRECISE
#define SDL_MOUSEWHEEL_FLIPPED 1
#if PRECISE
typedef struct { unsigned windowID, direction; int x,y; float preciseX,preciseY; } SDL_MouseWheelEvent;
#else
typedef struct { unsigned windowID, direction; int x,y; } SDL_MouseWheelEvent;
#endif
typedef struct { SDL_MouseWheelEvent wheel; } SDL_Event;
typedef enum { INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN,
    INPUT_BUTTON_WHEEL_LEFT, INPUT_BUTTON_WHEEL_RIGHT } InputButton;
enum { INPUT_AXIS_WHEEL, INPUT_AXIS_HWHEEL };
struct sdl2_console { void *real_window, *surface; struct {int con;} dcl; SDLScroll scroll; };
static struct sdl2_console consoles[2];
static bool graphic=true, hires=true;
static int axes[64], values[64], count, syncs, clicks[4];
static struct sdl2_console *get_scon_from_window(unsigned id) { return id==11?&consoles[0]:id==12?&consoles[1]:NULL; }
static bool qemu_console_is_graphic(int con) { (void)con; return graphic; }
static bool qemu_input_has_wheel(int con) { (void)con; return hires; }
static void qemu_input_queue_rel(int con, int axis, int value) {
    (void)con; assert(count<64); axes[count]=axis; values[count++]=value;
}
static void qemu_input_queue_btn(int con, InputButton button, bool down) {
    (void)con; if (down) clicks[button]++;
}
static void qemu_input_event_sync(void) { syncs++; }
''' + handler + r'''
static void clear(void) {
    count=syncs=0; for (int i=0;i<4;i++) clicks[i]=0;
}
static void wheel(unsigned id, float x, float y, unsigned direction) {
    SDL_Event ev={.wheel={.windowID=id, .direction=direction, .x=(int)x, .y=(int)y}};
#if PRECISE
    ev.wheel.preciseX=x; ev.wheel.preciseY=y;
#endif
    handle_mousewheel(&ev);
}
int main(void) {
    consoles[0].real_window=consoles[0].surface=(void*)1;
    consoles[1].real_window=consoles[1].surface=(void*)1;
    wheel(99,0,1,0); assert(count==0 && syncs==0);
    consoles[0].surface=NULL; wheel(11,0,1,0); assert(count==0);
    consoles[0].surface=(void*)1; graphic=false; wheel(11,0,1,0); assert(count==0); graphic=true;
#if PRECISE
    /* Both fractional axes are sent even when SDL's integer fields are zero. */
    wheel(11,0.25f,0.125f,0);
    assert(count==2 && syncs==1 && axes[0]==INPUT_AXIS_HWHEEL && values[0]==30);
    assert(axes[1]==INPUT_AXIS_WHEEL && values[1]==15);
    clear(); wheel(11,0.25f,0.5f,SDL_MOUSEWHEEL_FLIPPED);
    assert(count==2 && values[0]==-30 && values[1]==-60);
    clear(); for(int i=0;i<4;i++) wheel(11,0,0.03125f,0);
    assert(count==4 && values[0]+values[1]+values[2]+values[3]==15);
    assert(consoles[0].scroll.y==0);
    clear(); wheel(11,0,0.00390625f,0); assert(count==0);
    wheel(12,0,0.00390625f,0); assert(count==0); /* each output owns its remainder */
    wheel(11,0,-0.00390625f,0); assert(count==0 && consoles[0].scroll.y==0);
#endif
    clear(); wheel(11,2,-3,0);
    assert(count==2 && values[0]==240 && values[1]==-360 && syncs==1);
    /* No hires device: accumulate whole notches and release every button. */
    clear(); hires=false;
#if PRECISE
    for(int i=0;i<3;i++) wheel(11,-0.25f,0.25f,0);
    assert(syncs==0 && clicks[INPUT_BUTTON_WHEEL_UP]==0);
    wheel(11,-0.25f,0.25f,0);
    assert(syncs==4 && clicks[INPUT_BUTTON_WHEEL_UP]==1 && clicks[INPUT_BUTTON_WHEEL_LEFT]==1);
#endif
    clear(); wheel(11,2,-3,0);
    assert(count==0 && clicks[INPUT_BUTTON_WHEEL_RIGHT]==2 && clicks[INPUT_BUTTON_WHEEL_DOWN]==3 && syncs==10);
    clear(); wheel(11,1,-1,SDL_MOUSEWHEEL_FLIPPED);
    assert(clicks[INPUT_BUTTON_WHEEL_LEFT]==1 && clicks[INPUT_BUTTON_WHEEL_UP]==1);
    double remainder=0;
    assert(sdl_scroll_axis(NAN,&remainder)==0 && remainder==0);
    assert(sdl_scroll_axis(INFINITY,&remainder)==0 && remainder==0);
    assert(sdl_scroll_axis(1e30,&remainder)==INT_MAX && remainder==0);
    assert(sdl_scroll_axis(-1e30,&remainder)==INT_MIN && remainder==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-scroll-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    for precise in (1, 0):
        subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        f'-DPRECISE={precise}', '-I', str(root / 'include/ui'),
                        str(work / 'test.c'), '-o', str(work / 'test')], check=True)
        subprocess.run([str(work / 'test')], check=True)
print('ok - precise/old SDL, fractional axes, flipped direction, detents, per-window remainders and fallback')
