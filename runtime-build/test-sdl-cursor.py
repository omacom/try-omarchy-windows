#!/usr/bin/env python3
"""Compile real SDL cursor, grab and window-event paths with mocked SDL."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
root = parser.parse_args().source
source = (root / 'ui/sdl2.c').read_text()
header = (root / 'include/ui/sdl2.h').read_text()


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_OPENGL 1
#define _WIN32 1
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define SDL_TRUE 1
#define SDL_FALSE 0
#define SDL_ENABLE 1
#define SDL_DISABLE 0
#define SDL_WINDOW_INPUT_FOCUS 1
#define SDL_WINDOW_FULLSCREEN_DESKTOP 2
#define GUI_REFRESH_INTERVAL_DEFAULT 30
#define SHUTDOWN_ACTION_POWEROFF 1
#define SHUTDOWN_CAUSE_HOST_UI 1
#define KMOD_RCTRL 1
#define KMOD_LSHIFT 2
#define KMOD_LALT 4
#define KMOD_LCTRL 8
typedef struct {int w,h; bool graphic,absolute;} QemuConsole;
typedef struct {QemuConsole *con;} DisplayChangeListener;
typedef struct {int x,y,legacy_x,legacy_y;} SDLScroll;
typedef struct {bool has_show_cursor,show_cursor,has_window_close,window_close;} DisplayOptions;
typedef struct {int w,h,id,flags;} SDL_Window;
typedef struct {int w,h;} DisplaySurface;
typedef struct {int w,h,hot_x,hot_y; uint32_t first,last;} SDL_Cursor;
typedef struct {void *pixels; int pitch,w,h;} SDL_Surface;
typedef struct {int width,height;} QemuUIInfo;
typedef struct {int width,height,hot_x,hot_y,refs; uint32_t data[4096];} QEMUCursor;
typedef struct {struct {uint32_t windowID; int event,data1,data2;} window;} SDL_Event;
enum {SDL_WINDOWEVENT_RESIZED,SDL_WINDOWEVENT_EXPOSED,SDL_WINDOWEVENT_FOCUS_GAINED,
 SDL_WINDOWEVENT_ENTER,SDL_WINDOWEVENT_FOCUS_LOST,SDL_WINDOWEVENT_LEAVE,
 SDL_WINDOWEVENT_RESTORED,SDL_WINDOWEVENT_MINIMIZED,SDL_WINDOWEVENT_CLOSE,
 SDL_WINDOWEVENT_SHOWN,SDL_WINDOWEVENT_HIDDEN};
struct sdl2_console {
 DisplayChangeListener dcl; DisplayOptions *opts; SDL_Window *real_window;
 DisplaySurface *surface; bool opengl,scanout_mode,hidden;
 void *winctx,*real_renderer;
 int ignore_hotkeys; struct {int width,height;} guest_fb;
''' + section(header, '    SDLScroll scroll;', '    bool has_dmabuf;') + r'''
};
static struct sdl2_console consoles[3], *sdl2_console=consoles;
static int sdl2_num_outputs=3, gui_grab,absolute_enabled,gui_fullscreen,gui_saved_grab;
static int gui_grab_code,shutdown_action; static bool alt_grab,ctrl_grab;
static SDL_Cursor arrow,hidden,*sdl_cursor_normal=&arrow,*sdl_cursor_hidden=&hidden;
static SDL_Cursor *current=&arrow;
static SDL_Window *focus;
static int visible=1,relative,creates,frees,warps; static bool fail_cursor,fail_surface;
static int surface_width(DisplaySurface *s) {return s->w;}
static int surface_height(DisplaySurface *s) {return s->h;}
static bool qemu_console_is_graphic(QemuConsole *c) {return c->graphic;}
static bool qemu_input_is_absolute(QemuConsole *c) {return c->absolute;}
static QEMUCursor *cursor_ref(QEMUCursor *c) {c->refs++; return c;}
static void cursor_unref(QEMUCursor *c) {assert(c->refs>0); c->refs--;}
static SDL_Window *SDL_GetMouseFocus(void) {return focus;}
static SDL_Cursor *SDL_GetCursor(void) {return current;}
static void SDL_SetCursor(SDL_Cursor *c) {assert(c); current=c;}
static void SDL_ShowCursor(int show) {visible=show;}
static void SDL_SetRelativeMouseMode(int mode) {relative=mode;}
static void SDL_FreeCursor(SDL_Cursor *c) {assert(current!=c); frees++; free(c);}
static void SDL_GetWindowSize(SDL_Window *w,int *x,int *y) {*x=w->w;*y=w->h;}
static SDL_Surface *SDL_CreateRGBSurface(int f,int w,int h,int d,
 uint32_t r,uint32_t g,uint32_t b,uint32_t a) {
 assert(d==32 && r==0xff0000 && g==0x00ff00 && b==0xff && a==0xff000000);
 if(fail_surface) return NULL;
 SDL_Surface *s=calloc(1,sizeof(*s));s->w=w;s->h=h;s->pitch=w*4;
 s->pixels=calloc(h,s->pitch);return s;
}
static void SDL_FreeSurface(SDL_Surface *s) {free(s->pixels);free(s);}
static SDL_Cursor *SDL_CreateColorCursor(SDL_Surface *s,int x,int y) {
 creates++; if(fail_cursor) return NULL;
 SDL_Cursor *c=malloc(sizeof(*c)); *c=(SDL_Cursor){s->w,s->h,x,y,
 ((uint32_t *)s->pixels)[0],((uint32_t *)s->pixels)[s->w*s->h-1]}; return c;
}
static int SDL_GetWindowFlags(SDL_Window *w) {return w->flags;}
static void SDL_SetWindowGrab(SDL_Window *w,int grab) {}
static void SDL_WarpMouseInWindow(SDL_Window *w,int x,int y) {assert(w==focus);warps++;}
static void SDL_GetMouseState(int *x,int *y) {*x=*y=50;}
static void sdl_update_caption(struct sdl2_console *s) {}
static void SDL_SetWindowFullscreen(SDL_Window *w,int f) {w->w=f?1600:800;w->h=f?1200:600;}
static void sdl2_redraw(struct sdl2_console *s) {}
static SDL_Window *SDL_GetWindowFromID(uint32_t id) {
 for(int i=0;i<3;i++) if(consoles[i].real_window && consoles[i].real_window->id==id)
  return consoles[i].real_window;
 return NULL;
}
static int get_mod_state(void) {return 0;}
static void dpy_set_ui_info(QemuConsole *c,QemuUIInfo *i,bool b) {}
static void SDL_StopTextInput(void) {}
static void SDL_StartTextInput(void) {}
static void update_displaychangelistener(DisplayChangeListener *d,int i) {}
static void qemu_system_shutdown_request(int reason) {}
static void SDL_HideWindow(SDL_Window *w) {}
static void SDL_QuitSubSystem(int s) {}
static void win_pinch_destroy(struct sdl2_console *s) {}
static void win_hold_release(struct sdl2_console *s, bool show) {}
static void sdl2_window_size_after_full_screen(struct sdl2_console *s) {}
static void SDL_GL_DeleteContext(void *ctx) {}
static void SDL_DestroyRenderer(void *renderer) {}
static void SDL_DestroyWindow(SDL_Window *w) {if (focus==w) focus=NULL;}
#define SDL_INIT_VIDEO 1
''' + section(source, 'static struct sdl2_console *get_scon_from_window(',
              'void sdl2_window_create(') + section(
    source, '/* SDL has one current cursor.', '/*\n * Mouse mode change') + section(
    source, 'void sdl2_window_destroy(', 'void sdl2_window_resize(') + section(
    source, 'static void toggle_full_screen(', 'static int get_mod_state(') + section(
    source, 'static void handle_windowevent(', '/* SDL reports one path') + section(
    source, 'static void sdl_mouse_warp(', 'static const DisplayChangeListenerOps dcl_2d_ops') + r'''
static void event(int i,int type) {
 SDL_Event e={.window={.windowID=consoles[i].real_window->id,.event=type}};
 handle_windowevent(&e);
}
int main(void) {
 DisplayOptions opts={.has_show_cursor=true};
 SDL_Window windows[3]={{800,600,1,1},{800,600,2,1},{800,600,3,1}};
 DisplaySurface surfaces[3]={{800,600},{800,600},{800,600}};
 QemuConsole qcon[3]={{800,600,true,true},{800,600,true,true},{800,600,false,false}};
 QEMUCursor sprite={.width=64,.height=64,.hot_x=3,.hot_y=1}, other=sprite,empty=sprite;
 sprite.data[0]=0xff123456;sprite.data[4095]=0xffabcdef;
 other.data[0]=0xff654321;other.hot_x=11;other.hot_y=12;
 for(int i=0;i<3;i++) {
  consoles[i].opts=&opts; consoles[i].real_window=&windows[i];
  consoles[i].surface=&surfaces[i];consoles[i].dcl.con=&qcon[i];
 }
 focus=&windows[0];sdl_show_cursor(&consoles[0]);assert(!visible && current==&hidden);
 sdl_mouse_warp(&consoles[0].dcl,10,20,true);assert(!visible);
 puts("ok - no definition keeps show-cursor=off hidden");
 sdl_mouse_define(&consoles[0].dcl,&sprite);
 assert(visible && current==consoles[0].guest_sprite && current->hot_x==3 && current->hot_y==1);
 assert(current->first==0xff123456 && current->last==0xffabcdef && sprite.refs==1);
 sdl_mouse_warp(&consoles[0].dcl,10,20,false);assert(!visible);
 sdl_mouse_warp(&consoles[0].dcl,10,20,true);assert(visible && warps==0);
 sdl_mouse_define(&consoles[0].dcl,&empty);assert(!visible && !consoles[0].cursor && sprite.refs==0);
 sdl_mouse_warp(&consoles[0].dcl,10,20,true);assert(!visible);
 sdl_mouse_define(&consoles[0].dcl,&sprite);assert(visible);
 puts("ok - define pixels and hotspot, hide, restore and empty sprite");
 SDL_Cursor *first=current;
 sdl_mouse_define(&consoles[1].dcl,&other);assert(current==first && visible && other.refs==1);
 sdl_mouse_warp(&consoles[1].dcl,0,0,false);assert(current==first && visible);
 focus=&windows[1];event(1,SDL_WINDOWEVENT_ENTER);assert(!visible);
 sdl_mouse_warp(&consoles[1].dcl,0,0,true);assert(visible && current->first==other.data[0]);
 focus=&windows[0];event(0,SDL_WINDOWEVENT_FOCUS_GAINED);assert(visible && current==first);
 focus=NULL;event(0,SDL_WINDOWEVENT_LEAVE);assert(visible && current==&arrow && !relative);
 focus=&windows[2];event(2,SDL_WINDOWEVENT_ENTER);assert(visible && current==&arrow);
 puts("ok - other head cannot overwrite state, focus selects head, leave and text use native arrow");
 focus=&windows[0];sdl_grab_end(&consoles[0]);assert(visible && current==first);
 sdl_grab_start(&consoles[0]);assert(visible && gui_grab);
 sdl_mouse_warp(&consoles[0].dcl,0,0,false);sdl_grab_end(&consoles[0]);assert(!visible);
 sdl_mouse_warp(&consoles[0].dcl,0,0,true);
 qcon[0].absolute=false;sdl_grab_start(&consoles[0]);assert(visible && warps==1 && !relative);
 sdl_mouse_warp(&consoles[0].dcl,0,0,false);assert(!visible && relative);
 sdl_grab_end(&consoles[0]);assert(visible && current==&arrow && !relative);
 qcon[0].absolute=true;sdl_mouse_warp(&consoles[0].dcl,0,0,true);
 puts("ok - grab and release preserve absolute cursor policy and relative mode");
 toggle_full_screen(&consoles[0]);assert(current->w==128 && current->h==128 && current->hot_x==6);
 toggle_full_screen(&consoles[0]);assert(current->w==64 && current->h==64 && current->hot_x==3);
 windows[0].w=1600;windows[0].h=600;event(0,SDL_WINDOWEVENT_RESIZED);sdl_cursor_focus();
 assert(current->w==64 && current->h==64); /* letterbox scale is min(x,y) */
 consoles[0].opengl=true;consoles[0].scanout_mode=true;
 consoles[0].guest_fb.width=800;consoles[0].guest_fb.height=600;sdl_cursor_focus();
 assert(current->w==128 && current->h==64 && current->hot_x==6 && current->hot_y==1);
 puts("ok - fullscreen, letterbox and stretched GL scanout scale sprite and hotspot together");
 fail_cursor=true;sdl_mouse_define(&consoles[0].dcl,&other);assert(!visible && !consoles[0].guest_sprite);
 int attempts=creates;sdl_show_cursor(&consoles[0]);assert(creates==attempts);
 sdl_mouse_warp(&consoles[0].dcl,0,0,true);assert(!visible);
 fail_cursor=false;sdl_mouse_define(&consoles[0].dcl,&sprite);assert(visible);
 fail_surface=true;sdl_mouse_define(&consoles[0].dcl,&other);assert(!visible);
 fail_surface=false;sdl_mouse_define(&consoles[0].dcl,&sprite);assert(visible);
 puts("ok - color cursor and surface failures hide, avoid refresh retry, next definition recovers");
 opts.show_cursor=true;sdl_mouse_warp(&consoles[0].dcl,0,0,false);assert(visible && current==&arrow);
 opts.show_cursor=false;sdl_mouse_warp(&consoles[0].dcl,0,0,true);assert(visible);
 sdl2_window_destroy(&consoles[0]);
 assert(!visible && !consoles[0].guest_sprite && !consoles[0].real_window);
 sdl_cursor_focus();assert(visible && current==&arrow);
 consoles[0].real_window=&windows[0];focus=&windows[0];
 sdl_show_cursor(&consoles[0]);assert(visible); /* recreation uses retained QEMU pixels */
 QEMUCursor invalid=sprite;invalid.hot_x=-1;
 sdl_mouse_define(&consoles[0].dcl,&invalid);assert(!visible && !consoles[0].cursor);
 invalid=sprite;invalid.width=65;
 sdl_mouse_define(&consoles[0].dcl,&invalid);assert(!visible && !consoles[0].cursor);
 sdl_mouse_define(&consoles[0].dcl,&sprite);assert(visible);
 sdl_cleanup();assert(!sprite.refs && !other.refs && frees>0);
 puts("ok - diagnostic override and cursor resource lifetime through recreation and cleanup");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-cursor-') as temporary:
    output = Path(temporary)
    (output / 'test.c').write_text(harness)
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Werror',
                    '-Wno-unused-function', '-Wno-unused-variable',
                    str(output / 'test.c'), '-o', str(output / 'test')], check=True)
    subprocess.run([str(output / 'test')], check=True)
