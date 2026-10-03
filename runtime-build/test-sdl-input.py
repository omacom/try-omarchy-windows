#!/usr/bin/env python3
"""Exercise real SDL window lookup and pointer handlers during output changes."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
source = parser.parse_args().source.read_text(encoding='utf-8')
lookup = source[source.index('static struct sdl2_console *get_scon_from_window('):
                source.index('void sdl2_window_create(')]
handlers = source[source.index('static void handle_mousemotion('):
                  source.index('static void sdl_send_wheel_buttons(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {int w, h;} SDL_Window;
typedef struct {int w, h;} Surface;
typedef struct {uint32_t windowID; int x, y, button;} SDL_MouseButtonEvent;
typedef struct {int type; struct {uint32_t windowID; int x,y,xrel,yrel,state;} motion;
    SDL_MouseButtonEvent button;} SDL_Event;
struct sdl2_console {SDL_Window *real_window; Surface *surface; struct {int con;} dcl;};
static SDL_Window window = {320,200};
static Surface surface = {640,400};
static struct sdl2_console sdl2_console[2];
static int sdl2_num_outputs = 2, gui_grab, gui_fullscreen, absolute_enabled;
static int sent, last_x, last_y;
enum {SDL_MOUSEBUTTONUP=1,SDL_MOUSEBUTTONDOWN,SDL_BUTTON_LEFT=1};
#define SDL_BUTTON(b) (1 << ((b)-1))
static SDL_Window *SDL_GetWindowFromID(uint32_t id) {return id==11?&window:NULL;}
static void SDL_GetWindowSize(SDL_Window *w,int *x,int *y) {*x=w?w->w:0;*y=w?w->h:0;}
static int SDL_GetMouseState(void *x,void *y) {return 0;}
static bool qemu_console_is_graphic(int con) {return true;}
static bool qemu_input_is_absolute(int con) {return true;}
static void sdl_grab_start(struct sdl2_console *s) {}
static void sdl_grab_end(struct sdl2_console *s) {}
static int surface_width(Surface *s) {return s->w;}
static int surface_height(Surface *s) {return s->h;}
static void sdl_send_mouse_event(struct sdl2_console *s,int dx,int dy,int x,int y,int state) {
    sent++;last_x=x;last_y=y;
}
''' + lookup + handlers + r'''
int main(void) {
    sdl2_console[0].surface=&surface; /* inactive secondary, no SDL window */
    sdl2_console[1].surface=&surface;sdl2_console[1].real_window=&window;
    assert(!get_scon_from_window(0));
    assert(!get_scon_from_window(99));
    assert(get_scon_from_window(11)==&sdl2_console[1]);
    SDL_Event ev={.type=SDL_MOUSEBUTTONDOWN,.motion={.windowID=99,.x=50,.y=25},
        .button={.windowID=99,.x=20,.y=10,.button=1}};
    handle_mousemotion(&ev);handle_mousebutton(&ev);assert(sent==0);
    ev.motion.windowID=ev.button.windowID=11;
    window.w=0;handle_mousemotion(&ev);handle_mousebutton(&ev);assert(sent==0);
    window.w=320;window.h=0;handle_mousemotion(&ev);handle_mousebutton(&ev);assert(sent==0);
    window.h=200;sdl2_console[1].surface=NULL;
    handle_mousemotion(&ev);handle_mousebutton(&ev);assert(sent==0);
    sdl2_console[1].surface=&surface;
    handle_mousemotion(&ev);assert(sent==1 && last_x==100 && last_y==50);
    handle_mousebutton(&ev);assert(sent==2 && last_x==40 && last_y==20);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-input-') as temporary:
    root = Path(temporary)
    (root / 'test.c').write_text(harness, encoding='utf-8')
    subprocess.run(['gcc', '-std=gnu11', '-O2', str(root / 'test.c'),
                    '-o', str(root / 'test.exe')], check=True)
    subprocess.run([str(root / 'test.exe')], check=True)
print('ok - stale SDL ids, zero-size windows, missing surfaces and valid pointer scaling')
