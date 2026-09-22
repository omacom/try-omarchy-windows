#!/usr/bin/env python3
"""Compile and exercise the geometry shipped in the QEMU patch, without Win32."""

import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "runtime-build/patches/qemu/0014-forward-windows-pinch.patch"


def added_file(path):
    section = PATCH.read_text().split(f"diff --git a/{path} b/{path}\n", 1)[1]
    section = section.split("\ndiff --git ", 1)[0]
    return "".join(line[1:] + "\n" for line in section.splitlines()
                   if line.startswith("+") and not line.startswith("+++"))


class WindowsPinchTests(unittest.TestCase):
    def test_geometry_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "windows-pinch.h").write_text(added_file("include/ui/windows-pinch.h"))
            (work / "test.c").write_text(r'''
#include <assert.h>
#include <float.h>
#include "windows-pinch.h"

static WindowsPinchFrame frames[16];
static unsigned count;

static void record(void *opaque, const WindowsPinchFrame *frame)
{
    assert(opaque == frames);
    assert(count < 16);
    frames[count++] = *frame;
}

static void check_bounds(const WindowsPinch *pinch)
{
    int left = windows_pinch_x(pinch, 0);
    int right = windows_pinch_x(pinch, 1);
    assert(left >= 0 && right <= WINDOWS_PINCH_AXIS_MAX);
    assert(left < right);
    assert(left + right == 2 * WINDOWS_PINCH_CENTER);
}

int main(void)
{
    WindowsPinch pinch = {0};
    assert(!windows_pinch_update(&pinch, 0.5));
    assert(!windows_pinch_end(&pinch));

    windows_pinch_begin(&pinch);
    assert(pinch.active && pinch.scale == 1.0);
    int original = windows_pinch_x(&pinch, 1) - windows_pinch_x(&pinch, 0);
    assert(windows_pinch_update(&pinch, 0.5));
    int expanded = windows_pinch_x(&pinch, 1) - windows_pinch_x(&pinch, 0);
    assert(expanded == original * 3 / 2);
    assert(windows_pinch_update(&pinch, -1.0 / 3.0));
    assert(fabs(pinch.scale - 1.0) < 1e-12);
    check_bounds(&pinch);

    /* Small events accumulate continuously instead of becoming key presses. */
    for (int i = 0; i < 100; i++) {
        assert(windows_pinch_update(&pinch, 0.001));
        check_bounds(&pinch);
    }
    assert(fabs(pinch.scale - pow(1.001, 100)) < 1e-12);

    /* Extreme finite input saturates; non-finite/invalid deltas are rejected. */
    assert(windows_pinch_update(&pinch, DBL_MAX));
    check_bounds(&pinch);
    assert(windows_pinch_update(&pinch, DBL_MAX));
    check_bounds(&pinch);
    for (int i = 0; i < 100; i++) {
        assert(windows_pinch_update(&pinch, -0.9));
        check_bounds(&pinch);
    }
    double before = pinch.scale;
    assert(!windows_pinch_update(&pinch, NAN));
    assert(!windows_pinch_update(&pinch, INFINITY));
    assert(!windows_pinch_update(&pinch, -INFINITY));
    assert(!windows_pinch_update(&pinch, -1.0));
    assert(!windows_pinch_update(&pinch, -2.0));
    assert(pinch.scale == before);

    /* End/cancel/focus loss emit one release, and orphaned updates stay idle. */
    assert(windows_pinch_end(&pinch));
    assert(!windows_pinch_end(&pinch));
    assert(!windows_pinch_update(&pinch, 0.1));
    windows_pinch_begin(&pinch);
    assert(pinch.scale == 1.0);
    assert(windows_pinch_end(&pinch));

    /* Exercise the same lifecycle dispatch used by the Windows event handler. */
    assert(windows_pinch_event(&pinch, 0, true, 0.5, record, frames));
    assert(count == 0);  /* orphaned update */
    assert(!windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, false, 0.1,
                             record, frames));
    assert(count == 0);  /* background window */
    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, 0, record, frames);
    assert(count == 1 && frames[0].down);
    windows_pinch_event(&pinch, 0, true, 0.5, record, frames);
    assert(count == 2 && frames[1].down);
    assert(frames[1].x[1] > frames[0].x[1]);
    windows_pinch_event(&pinch, WINDOWS_PINCH_END, true, 0, record, frames);
    assert(count == 3 && !frames[2].down);
    windows_pinch_event(&pinch, WINDOWS_PINCH_END, true, 0, record, frames);
    assert(count == 3);

    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, 0, record, frames);
    assert(frames[3].x[1] == frames[0].x[1]);
    windows_pinch_cancel(&pinch, record, frames);  /* windowDidResignKey */
    assert(count == 5 && !frames[4].down);
    windows_pinch_event(&pinch, 0, true, 0.5, record, frames);
    assert(count == 5);  /* focus return does not resume old contacts */

    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, 0, record, frames);
    windows_pinch_event(&pinch, WINDOWS_PINCH_CANCEL, true, 0.5, record, frames);
    assert(count == 7 && !frames[6].down);
    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, NAN, record, frames);
    assert(count == 7);  /* invalid begin never creates contacts */
    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, 0, record, frames);
    windows_pinch_event(&pinch, 0, true, NAN, record, frames);
    assert(count == 9 && !frames[8].down);  /* invalid update releases */
    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, 0, record, frames);
    windows_pinch_event(&pinch, 0, false, 0, record, frames);
    assert(count == 11 && !frames[10].down);  /* pointer leaves guest */
    windows_pinch_event(&pinch, WINDOWS_PINCH_BEGIN, true, 0, record, frames);
    windows_pinch_vm_state(&pinch, false, record, frames);
    assert(count == 12 && !pinch.active);  /* no input while stopped */
    windows_pinch_cancel(&pinch, record, frames);  /* focus loss while stopped */
    windows_pinch_vm_state(&pinch, true, record, frames);
    assert(count == 13 && !frames[12].down);  /* clear guest slots on resume */
    windows_pinch_event(&pinch, 0, true, 0.2, record, frames);
    assert(count == 13);
    return 0;
}
''')
            compiler = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                           str(work / "test.c"), "-lm", "-o", str(work / "test")],
                           check=True)
            subprocess.run([str(work / "test")], check=True)

    def test_native_pan_and_pinch_dispatch(self):
        source = added_file("ui/sdl2-pinch-win32.h")
        start = source.index("static void CALLBACK win_pinch_output(")
        end = source.index("\nstatic void win_pinch_cancel", start)
        callback = source[start:end]
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "windows-pinch.h").write_text(added_file("include/ui/windows-pinch.h"))
            (work / "test.c").write_text(r'''
#include <assert.h>
#include "windows-pinch.h"
#define CALLBACK
#define INTERACTION_ID_MANIPULATION 1
#define INTERACTION_FLAG_BEGIN 1
#define INTERACTION_FLAG_END 2
#define INTERACTION_FLAG_CANCEL 4
#define INTERACTION_FLAG_INERTIA 8
#define POINTER_INPUT_TYPE int
typedef struct {
    int interactionId, interactionFlags, inputType;
    struct { struct { struct { double scale; } delta; } manipulation; } arguments;
} INTERACTION_CONTEXT_OUTPUT;
typedef struct {
    WindowsPinch pinch;
    bool consumed, pending_begin, stopping, blocked;
    int frames, releases;
} WinPinch;
static bool win_pinch_eligible(WinPinch *p) { return !p->blocked; }
static void win_pinch_emit(void *opaque, const WindowsPinchFrame *frame) {
    WinPinch *p = opaque;
    p->frames++;
    if (!frame->down) p->releases++;
}
''' + callback + r'''
static void event(WinPinch *p, unsigned flags, double scale) {
    INTERACTION_CONTEXT_OUTPUT out = {0};
    out.interactionId = 1; out.inputType = 5; out.interactionFlags = flags;
    out.arguments.manipulation.delta.scale = scale;
    p->consumed = p->pinch.active;
    win_pinch_output(p, &out);
}
int main(void) {
    WinPinch p = {0};
    event(&p, 1, 1); assert(!p.consumed && p.frames == 0);
    event(&p, 0, 1); assert(!p.consumed && p.frames == 0);
    event(&p, 2, 1); assert(!p.consumed && p.frames == 0 && !p.pending_begin);
    event(&p, 1, 1); event(&p, 0, 1.2);
    assert(p.consumed && p.pinch.active && p.frames == 2);
    event(&p, 2, 1); assert(p.consumed && !p.pinch.active && p.releases == 1);
    event(&p, 1, 1.1); event(&p, 4, 1);
    assert(!p.pinch.active && p.releases == 2);
    p.blocked = true; event(&p, 1, 1.2);
    assert(!p.pinch.active && !p.consumed);
    return 0;
}
''')
            compiler = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                           str(work / "test.c"), "-lm", "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True)

    def test_history_coalesces_motion_but_preserves_releases(self):
        source = added_file("ui/sdl2-pinch-win32.h")
        start = source.index("static void win_pinch_flush(")
        end = source.index("static bool win_pinch_eligible", start)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "windows-pinch.h").write_text(added_file("include/ui/windows-pinch.h"))
            (work / "test.c").write_text(r'''
#include <assert.h>
#include "windows-pinch.h"
typedef struct {
    WindowsPinchFrame queued;
    bool have_queued;
} WinPinch;
static WindowsPinchFrame delivered[8];
static int count;
static void win_pinch_deliver(void *opaque, const WindowsPinchFrame *frame) {
    assert(opaque && count < 8);
    delivered[count++] = *frame;
}
''' + source[start:end] + r'''
int main(void) {
    WinPinch p = {0};
    WindowsPinchFrame first = {true, {12000, 20000}};
    WindowsPinchFrame last = {true, {10000, 22000}};
    WindowsPinchFrame release = {false, {0, 0}};
    win_pinch_emit(&p, &first);
    win_pinch_emit(&p, &last);
    assert(count == 0);
    win_pinch_flush(&p);
    assert(count == 1 && delivered[0].x[0] == 10000);
    win_pinch_flush(&p); assert(count == 1);
    win_pinch_emit(&p, &last);
    win_pinch_emit(&p, &release);
    assert(count == 2 && delivered[1].down);
    win_pinch_emit(&p, &first);
    assert(count == 3 && !delivered[2].down);
    win_pinch_flush(&p);
    assert(count == 4 && delivered[3].down);
    return 0;
}
''')
            compiler = shlex.split(os.environ.get("CC", "cc"))
            subprocess.run(compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                           str(work / "test.c"), "-lm", "-o", str(work / "test")], check=True)
            subprocess.run([str(work / "test")], check=True)

    def test_builder_verifies_exact_patch(self):
        lock = json.loads((ROOT / "runtime-build/sources.lock.json").read_text())
        entry = next(p for p in lock["qemu"]["patches"]
                     if p["file"] == str(PATCH.relative_to(ROOT / "runtime-build")))
        self.assertEqual(hashlib.sha256(PATCH.read_bytes()).hexdigest(), entry["sha256"])


if __name__ == "__main__":
    unittest.main()
