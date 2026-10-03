#!/usr/bin/env python3
"""Compile the actual patched virtio wheel dispatch with transport stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path, help='patched QEMU source directory')
root = parser.parse_args().source
source = (root / 'hw/input/virtio-input-hid.c').read_text()
capability = source[source.index('static bool virtio_input_has_rel('):
                    source.index('static void virtio_input_handle_event(')]
cases = source[source.index('    case INPUT_EVENT_KIND_BTN:'):
               source.index('    case INPUT_EVENT_KIND_ABS:')]
harness = Path(__file__).with_name('virtio-scroll-harness.c').read_text()
harness = harness.replace('/* REL_CAPABILITY */', capability).replace('/* WHEEL_DISPATCH */', cases)
with tempfile.TemporaryDirectory(prefix='tryomarchy-virtio-scroll-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(harness)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
# Exercise the real routing decision: precision support follows the button
# receiver so a guest still using PS/2 does not lose its wheel input.
routing_source = (root / 'ui/input.c').read_text()
routing = routing_source[routing_source.index('bool qemu_input_has_wheel('):
                         routing_source.index('void qmp_input_send_event(')]
routing_harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
enum { INPUT_EVENT_KIND_BTN, INPUT_EVENT_KIND_REL, INPUT_EVENT_KIND_ABS };
enum { INPUT_AXIS_X, INPUT_AXIS_Y, INPUT_AXIS_WHEEL, INPUT_AXIS_HWHEEL };
#define INPUT_EVENT_MASK_BTN (1 << INPUT_EVENT_KIND_BTN)
#define INPUT_EVENT_MASK_WHEEL (1 << 5)
typedef int QemuConsole;
typedef struct { uint32_t mask; } QemuInputHandler;
typedef struct { QemuInputHandler *handler; } QemuInputHandlerState;
typedef struct { int axis; } Move;
typedef struct { int type; struct { struct { Move *data; } rel; } u; } InputEvent;
static QemuInputHandler handler;
static QemuInputHandlerState state = { &handler };
static bool present;
static QemuInputHandlerState *qemu_input_find_handler(uint32_t mask, QemuConsole *con) {
    (void)con; assert(mask == INPUT_EVENT_MASK_BTN); return present ? &state : NULL;
}
''' + routing + r'''
int main(void) {
    assert(!qemu_input_has_wheel(NULL));
    present=true; handler.mask=INPUT_EVENT_MASK_BTN;
    assert(!qemu_input_has_wheel(NULL));
    handler.mask |= INPUT_EVENT_MASK_WHEEL;
    assert(qemu_input_has_wheel(NULL));
    Move move = { INPUT_AXIS_WHEEL };
    InputEvent evt = { .type=INPUT_EVENT_KIND_REL, .u.rel.data=&move };
    assert(qemu_input_event_mask(&evt) == INPUT_EVENT_MASK_WHEEL);
    move.axis=INPUT_AXIS_HWHEEL; assert(qemu_input_event_mask(&evt) == INPUT_EVENT_MASK_WHEEL);
    move.axis=INPUT_AXIS_X; assert(qemu_input_event_mask(&evt) == 1 << INPUT_EVENT_KIND_REL);
    move.axis=INPUT_AXIS_Y; assert(qemu_input_event_mask(&evt) == 1 << INPUT_EVENT_KIND_REL);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='tryomarchy-wheel-routing-') as directory:
    work = Path(directory)
    (work / 'test.c').write_text(routing_harness)
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(work / 'test.c'), '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True)
print('ok - virtio hi-res/legacy axes, fractional boundaries, mouse after touchpad, releases and missing capabilities')
