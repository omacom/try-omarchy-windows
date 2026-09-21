#!/usr/bin/env python3
"""Observe only QEMU's dedicated pinch device for a bounded acceptance test.

Requires permission to open that /dev/input/event node and installed libinput.
Does not grab input or change compositor configuration. No keyboard is opened.
"""
import argparse
import ctypes as C
import ctypes.util
import errno
import json
import os
from pathlib import Path
import select
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=float, default=15)
    parser.add_argument('--expect', choices=('pinch', 'scroll'), default='pinch')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error('--seconds must be between 1 and 60')
    matches = [p.parent.parent.name for p in Path('/sys/class/input').glob('event*/device/name')
               if p.read_text().strip() == 'QEMU Virtio Pinch Touchpad']
    if len(matches) != 1:
        raise SystemExit('Expected exactly one QEMU Virtio Pinch Touchpad')
    node = '/dev/input/' + matches[0]
    allowed = {node}
    tablet = None
    if args.expect == 'scroll':
        tablets = [p.parent.parent.name for p in Path('/sys/class/input').glob('event*/device/name')
                   if p.read_text().strip() == 'QEMU Virtio Tablet']
        if len(tablets) != 1:
            raise SystemExit('Expected exactly one QEMU Virtio Tablet')
        tablet = '/dev/input/' + tablets[0]
        allowed.add(tablet)
    lib = C.CDLL(ctypes.util.find_library('input') or 'libinput.so.10')
    Open = C.CFUNCTYPE(C.c_int, C.c_char_p, C.c_int, C.c_void_p)
    Close = C.CFUNCTYPE(None, C.c_int, C.c_void_p)

    @Open
    def open_node(path, flags, _):
        if os.fsdecode(path) not in allowed:
            return -errno.EACCES
        try:
            return os.open(os.fsdecode(path), flags | os.O_CLOEXEC)
        except OSError as exc:
            return -exc.errno

    @Close
    def close_node(fd, _):
        os.close(fd)

    class Interface(C.Structure):
        _fields_ = [('open', Open), ('close', Close)]

    def fn(name, restype, *argtypes):
        f = getattr(lib, name)
        f.restype, f.argtypes = restype, argtypes
        return f

    ptr = C.c_void_p
    create = fn('libinput_path_create_context', ptr, C.POINTER(Interface), ptr)
    add = fn('libinput_path_add_device', ptr, ptr, C.c_char_p)
    unref = fn('libinput_unref', ptr, ptr)
    dispatch = fn('libinput_dispatch', C.c_int, ptr)
    get = fn('libinput_get_event', ptr, ptr)
    kind = fn('libinput_event_get_type', C.c_int, ptr)
    destroy = fn('libinput_event_destroy', None, ptr)
    gesture = fn('libinput_event_get_gesture_event', ptr, ptr)
    scale = fn('libinput_event_gesture_get_scale', C.c_double, ptr)
    fd = fn('libinput_get_fd', C.c_int, ptr)
    tap = fn('libinput_device_config_tap_set_enabled', C.c_int, ptr, C.c_int)
    dwt = fn('libinput_device_config_dwt_set_enabled', C.c_int, ptr, C.c_int)
    interface = Interface(open_node, close_node)
    context = create(C.byref(interface), None)
    if not context:
        raise SystemExit('Could not create libinput context')
    counts, scales, errors = {}, [], []
    Log = C.CFUNCTYPE(None, ptr, C.c_int, C.c_char_p, ptr)

    @Log
    def log_error(_context, priority, message, _arguments):
        if priority >= 30:  # libinput's ERROR level; reject kernel/input errors.
            errors.append(message.decode(errors='replace'))

    fn('libinput_log_set_handler', None, ptr, Log)(context, log_error)
    try:
        device = add(context, os.fsencode(node))
        if not device:
            raise SystemExit('Could not open pinch device')
        if tablet and not add(context, os.fsencode(tablet)):
            raise SystemExit('Could not open tablet')
        tap(device, 0)
        dwt(device, 0)
        print(json.dumps({'ready': True, 'device': node}), flush=True)
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            if dispatch(context) < 0:
                raise SystemExit('libinput dispatch failed')
            while event := get(context):
                k = kind(event)
                counts[k] = counts.get(k, 0) + 1
                if k in (803, 804, 805):
                    scales.append(scale(gesture(event)))
                destroy(event)
            select.select([fd(context)], [], [], min(.1, max(0, deadline - time.monotonic())))
    finally:
        unref(context)
    passed = all(counts.get(k, 0) for k in (803, 804, 805)) and not counts.get(402, 0)
    if args.expect == 'scroll':
        passed = bool(counts.get(403, 0) or counts.get(404, 0)) and not any(counts.get(k, 0) for k in (402, 803, 804, 805))
    passed = passed and not errors
    print(json.dumps({'passed': bool(passed), 'events': counts, 'errors': errors,
                      'minScale': min(scales, default=None), 'maxScale': max(scales, default=None)}))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
