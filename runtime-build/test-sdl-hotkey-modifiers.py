#!/usr/bin/env python3
"""Judge QEMU's Ctrl+Alt hotkeys by the modifiers of the queued key events."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
root = parser.parse_args().source
source = (root / 'ui/sdl2.c').read_text(encoding='utf-8')


def section(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


keys = section(source, 'static int get_mod_state(', 'static void handle_textinput(')
window = section(source, '    case SDL_WINDOWEVENT_FOCUS_GAINED:', '    case SDL_WINDOWEVENT_FOCUS_LOST:')

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
/* SDL's own values; QEMU compares them with an int as here. */
typedef enum {
    KMOD_NONE = 0, KMOD_LSHIFT = 0x1, KMOD_RSHIFT = 0x2,
    KMOD_LCTRL = 0x40, KMOD_RCTRL = 0x80, KMOD_LALT = 0x100, KMOD_RALT = 0x200
} SDL_Keymod;
typedef enum {
    SDL_SCANCODE_F = 9, SDL_SCANCODE_G = 10,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_KP_PLUS = 87, SDL_SCANCODE_KP_MINUS = 86,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LALT = 226
} SDL_Scancode;
enum { SDL_WINDOWEVENT = 0x200, SDL_KEYDOWN = 0x300, SDL_KEYUP };
enum { SDL_WINDOWEVENT_ENTER = 10, SDL_WINDOWEVENT_FOCUS_GAINED = 12, SDL_WINDOWEVENT_FOCUS_LOST = 13 };
typedef struct { SDL_Scancode scancode; uint16_t mod; } SDL_Keysym;
typedef struct { uint32_t type, windowID; uint8_t state, repeat; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { uint32_t type, windowID; uint8_t event; } SDL_WindowEvent;
typedef union { uint32_t type; SDL_KeyboardEvent key; SDL_WindowEvent window; } SDL_Event;
typedef struct { int id; } SDL_Window;
typedef struct { int unused; } QemuConsole;
typedef struct { QemuConsole *con; } DisplayChangeListener;
typedef struct { int w, h; } DisplaySurface;
struct sdl2_console {
    DisplayChangeListener dcl;
    SDL_Window *real_window;
    DisplaySurface *surface;
    bool opengl, hidden, gui_keysym;
    int ignore_hotkeys;
};
static QemuConsole console;
static SDL_Window qemu_window = { 1 };
static struct sdl2_console sdl2_console[1] = { { .dcl = { &console }, .real_window = &qemu_window } };
static int sdl2_num_outputs = 1;
static int gui_grab, gui_fullscreen, absolute_enabled;
static bool alt_grab, ctrl_grab;
static int gui_grab_code = KMOD_LALT | KMOD_LCTRL;
/* SDL counts every key it has queued; QEMU handles the queue afterwards. */
static uint16_t held, pumped;
static int toggles, guest_f;
__attribute__((unused)) static SDL_Keymod SDL_GetModState(void) { return (SDL_Keymod)pumped; }
static struct sdl2_console *get_scon_from_window(uint32_t id)
{
    return id == 1 ? &sdl2_console[0] : NULL;
}
static void toggle_full_screen(struct sdl2_console *scon) { (void)scon; gui_fullscreen = !gui_fullscreen; toggles++; }
static void sdl_grab_start(struct sdl2_console *scon) { (void)scon; gui_grab = 1; }
static void sdl_grab_end(struct sdl2_console *scon) { (void)scon; gui_grab = 0; }
static void SDL_HideWindow(SDL_Window *w) { (void)w; }
static void SDL_ShowWindow(SDL_Window *w) { (void)w; }
static void sdl2_release_modifiers(struct sdl2_console *scon) { (void)scon; }
static void sdl2_window_resize(struct sdl2_console *scon) { (void)scon; }
static void sdl2_2d_switch(DisplayChangeListener *dcl, DisplaySurface *s) { (void)dcl; (void)s; }
static void sdl2_process_key(struct sdl2_console *scon, SDL_KeyboardEvent *ev)
{
    (void)scon;
    if (ev->type == SDL_KEYDOWN && ev->keysym.scancode == SDL_SCANCODE_F) {
        guest_f++;
    }
}
__attribute__((unused)) static bool qemu_console_is_graphic(QemuConsole *c) { (void)c; return true; }
static bool qemu_input_is_absolute(QemuConsole *c) { (void)c; return true; }
static void absolute_mouse_grab(struct sdl2_console *scon) { (void)scon; }
static void sdl_show_cursor(struct sdl2_console *scon) { (void)scon; }
__attribute__((unused)) static void SDL_StopTextInput(void) {}
__attribute__((unused)) static void SDL_StartTextInput(void) {}
''' + keys + r'''
static void handle_window(struct sdl2_console *scon, int event)
{
    switch (event) {
''' + window + r'''
    default:
        break;
    }
}

static SDL_Event queue[16];
static int queued;

/* SDL turns a Windows key message into an event carrying the modifiers
 * as they are with that key. */
static void key(SDL_Scancode code, bool down)
{
    uint16_t mod = code == SDL_SCANCODE_LCTRL ? KMOD_LCTRL : code == SDL_SCANCODE_LALT ? KMOD_LALT : 0;
    held = down ? (held | mod) : (held & ~mod);
    queue[queued++].key = (SDL_KeyboardEvent){ down ? SDL_KEYDOWN : SDL_KEYUP, 1, down, 0, { code, held } };
}
static void pointer_or_focus(uint8_t event)
{
    queue[queued++].window = (SDL_WindowEvent){ SDL_WINDOWEVENT, 1, event };
}
static void chord_down(void) { key(SDL_SCANCODE_LCTRL, true); key(SDL_SCANCODE_LALT, true); key(SDL_SCANCODE_F, true); }
static void chord_up(void) { key(SDL_SCANCODE_F, false); key(SDL_SCANCODE_LALT, false); key(SDL_SCANCODE_LCTRL, false); }
/* One pass of sdl2_poll_events: SDL pumps the Windows messages, then QEMU
 * handles the events in order. */
static void poll(void)
{
    pumped = held;
    for (int i = 0; i < queued; i++) {
        SDL_Event *ev = &queue[i];
        if (ev->type == SDL_KEYDOWN) {
            handle_keydown(ev);
        } else if (ev->type == SDL_KEYUP) {
            handle_keyup(ev);
        } else {
            handle_window(get_scon_from_window(ev->window.windowID), ev->window.event);
        }
    }
    queued = 0;
}
static void reset(void) { toggles = guest_f = 0; }

int main(void)
{
    /* A press with the pointer already in the window. */
    pointer_or_focus(SDL_WINDOWEVENT_ENTER); poll();
    chord_down(); poll(); chord_up(); poll();
    assert(toggles == 1 && guest_f == 0);

    /* The pointer enters the window just before Ctrl+Alt+F, within one
     * poll: still the hotkey, not keys for the guest. */
    reset();
    pointer_or_focus(SDL_WINDOWEVENT_ENTER); chord_down(); poll(); chord_up(); poll();
    assert(toggles == 1 && guest_f == 0);

    /* The whole chord pressed and let go between two polls of a busy QEMU. */
    reset();
    chord_down(); chord_up(); poll();
    assert(toggles == 1 && guest_f == 0);

    /* Ctrl and Alt already held when the window gets focus: hotkeys wait
     * for a key to go up, as before. */
    reset();
    key(SDL_SCANCODE_LCTRL, true); key(SDL_SCANCODE_LALT, true); poll();
    pointer_or_focus(SDL_WINDOWEVENT_FOCUS_GAINED); key(SDL_SCANCODE_F, true); poll();
    assert(toggles == 0 && guest_f == 1);
    key(SDL_SCANCODE_F, false); poll();
    key(SDL_SCANCODE_F, true); poll();
    assert(toggles == 1 && guest_f == 1);
    chord_up(); poll();

    /* Focus arriving in the same poll as the chord, after the keys of the
     * last one went up. */
    reset();
    pointer_or_focus(SDL_WINDOWEVENT_FOCUS_GAINED); chord_down(); chord_up(); poll();
    assert(toggles == 1 && guest_f == 0);

    /* Focus goes to another app with Ctrl and Alt held. SDL lets go of its
     * keys then and learns nothing of keys pressed or let go elsewhere, so
     * QEMU's view comes back empty, not stale: a fresh chord on return is
     * the hotkey. */
    reset();
    key(SDL_SCANCODE_LCTRL, true); key(SDL_SCANCODE_LALT, true); poll();
    pointer_or_focus(SDL_WINDOWEVENT_FOCUS_LOST);
    key(SDL_SCANCODE_LALT, false); key(SDL_SCANCODE_LCTRL, false); poll();
    held = KMOD_LCTRL;
    pointer_or_focus(SDL_WINDOWEVENT_FOCUS_GAINED); poll();
    held = 0;
    chord_down(); poll(); chord_up(); poll();
    assert(toggles == 1 && guest_f == 0);

    puts("ok - Ctrl+Alt hotkeys follow the modifiers of the queued key events");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-sdl-hotkeys-') as temporary:
    test = Path(temporary)
    (test / 'test.c').write_text(harness, encoding='utf-8')
    binary = test / 'test.exe'
    subprocess.run(['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
                    str(test / 'test.c'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
