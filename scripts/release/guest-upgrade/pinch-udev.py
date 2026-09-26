#!/usr/bin/env python3
"""Check that udev hides the pinch touchpad from libinput until the guest is ready.

Creates a uinput device with the pinch touchpad's kernel name, then checks its
LIBINPUT_IGNORE_DEVICE property with and without /run/try-omarchy/pinch-ready.
Run as root inside the guest.
"""
import fcntl
import os
from pathlib import Path
import struct
import subprocess
import time

NAME = 'QEMU Virtio Pinch Touchpad'
MARKER = Path('/run/try-omarchy/pinch-ready')
UI_SET_EVBIT = 0x40045564
UI_SET_KEYBIT = 0x40045565
UI_DEV_SETUP = 0x405C5503
UI_DEV_CREATE = 0x5501
UI_DEV_DESTROY = 0x5502


def event_node():
    for _ in range(50):
        for name in Path('/sys/class/input').glob('input*/name'):
            if name.read_text().strip() == NAME:
                events = list(name.parent.glob('event*'))
                if events:
                    return name.parent.resolve(), Path('/dev/input') / events[0].name
        time.sleep(0.1)
    raise SystemExit('the test device did not appear')


def ignored(node):
    subprocess.run(['udevadm', 'settle', '--timeout=10'], check=True)
    properties = subprocess.run(['udevadm', 'info', '--query=property', f'--name={node}'],
                                check=True, capture_output=True, text=True).stdout.splitlines()
    return 'LIBINPUT_IGNORE_DEVICE=1' in properties


def main():
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, UI_SET_EVBIT, 1)  # EV_KEY
        fcntl.ioctl(fd, UI_SET_KEYBIT, 0x110)  # BTN_LEFT
        fcntl.ioctl(fd, UI_DEV_SETUP, struct.pack('HHHH80sI', 0x06, 0x0627, 0x0004, 1, NAME.encode(), 0))
        fcntl.ioctl(fd, UI_DEV_CREATE)
        device, node = event_node()
        subprocess.run(['/usr/local/lib/try-omarchy/pinch-ready'], check=True)
        assert MARKER.exists(), 'the upgraded guest should be ready for pinch'
        assert not ignored(node), 'a ready guest must expose the pinch device'
        MARKER.unlink()
        subprocess.run(['udevadm', 'trigger', '--action=change', f'--parent-match={device}'], check=True)
        assert ignored(node), 'without the marker the pinch device must be ignored'
        subprocess.run(['/usr/local/lib/try-omarchy/pinch-ready'], check=True)
        assert not ignored(node), 'the gate must expose the device again once ready'
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)
    print('pinch udev gate passed')


if __name__ == '__main__':
    main()
