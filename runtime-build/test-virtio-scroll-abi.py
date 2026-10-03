#!/usr/bin/env python3
"""Optional diskless guest ABI test with a qtest-capable patched QEMU.

QEMU_SCROLL_TEST_BINARY=/path/to/qemu-system-x86_64 python3 this-file.py
"""
import importlib.util
import os
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('pinch_test', Path(__file__).with_name('test-virtio-pinch.py'))
pinch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pinch)
pinch.BINARY = os.environ.get('QEMU_SCROLL_TEST_BINARY')


@unittest.skipUnless(pinch.BINARY, 'set QEMU_SCROLL_TEST_BINARY to run the guest ABI test')
class VirtioScrollTests(pinch.VirtioPinchTests):
    # Use the existing diskless setup/transport helpers; inspect the tablet at 03.0.
    __unittest_skip__ = not bool(pinch.BINARY)
    test_touchpad_capabilities_and_contact_frames = None

    def pci_read(self, offset):
        self.qt(f'outl 0xcf8 {0x80001800 + offset:#x}')
        return int(self.qt('inl 0xcfc'), 0)

    def pci_write(self, offset, value):
        self.qt(f'outl 0xcf8 {0x80001800 + offset:#x}')
        self.qt(f'outl 0xcfc {value:#x}')

    def test_tablet_wheel_events(self):
        self.assertEqual(self.config(1).rstrip(b'\0'), b'QEMU Virtio Tablet')
        axes = int.from_bytes(self.config(0x11, 2), 'little')
        for axis in (6, 8, 11, 12):  # horizontal, vertical, hi-res vertical/horizontal
            self.assertTrue(axes & (1 << axis))
        self.assertFalse(axes & 3)  # tablet must not advertise REL_X/REL_Y
        common = self.caps[1]
        self.qt(f'writeb {common + 20:#x} 3')
        self.qt(f'writel {common + 8:#x} 1')
        self.qt(f'writel {common + 12:#x} 1')
        self.qt(f'writeb {common + 20:#x} 11')
        self.assertEqual(int(self.qt(f'readb {common + 20:#x}'), 0), 11)
        self.qt(f'writew {common + 22:#x} 0')
        self.qt(f'writew {common + 24:#x} 64')
        desc, avail, used, buffers = 0x100000, 0x101000, 0x102000, 0x103000
        self.write(desc, b''.join(struct.pack('<QIHH', buffers + i * 8, 8, 2, 0)
                                  for i in range(64)))
        self.write(avail, struct.pack('<66H', 0, 64, *range(64)))
        for offset, address in ((32, desc), (40, avail), (48, used)):
            self.qt(f'writeq {common + offset:#x} {address:#x}')
        self.qt(f'writew {common + 28:#x} 1')
        self.qt(f'writeb {common + 20:#x} 15')
        self.command('cont')

        def rel(axis, value):
            self.command('input-send-event', {'events': [
                {'type': 'rel', 'data': {'axis': axis, 'value': value}}]})

        def button(name, down):
            self.command('input-send-event', {'events': [
                {'type': 'btn', 'data': {'button': name, 'down': down}}]})

        rel('wheel', 30)
        button('wheel-up', True)
        button('wheel-up', False)
        rel('wheel', 90)
        rel('hwheel', -30)
        rel('hwheel', -210)
        rel('hwheel', 360)
        button('wheel-right', True)
        button('wheel-right', False)
        count = struct.unpack('<H', self.read(used + 2, 2))[0]
        events = [struct.unpack('<HHi', self.read(buffers + i * 8, 8))
                  for i in range(count)]
        self.assertEqual([event for event in events if event[0] != 0], [
            (2, 11, 30), (2, 11, 120), (2, 8, 1),
            (2, 11, 90), (2, 8, 1), (2, 12, -30),
            (2, 12, -210), (2, 6, -2), (2, 12, 360), (2, 6, 3),
            (2, 12, 120), (2, 6, 1),
        ])
        self.assertEqual(sum(event[0] == 0 for event in events), 9)
        # An older Linux client can ignore the hi-res codes and retain all notches.
        self.assertEqual(sum(event[2] for event in events if event[:2] == (2, 8)), 2)
        self.assertEqual(sum(event[2] for event in events if event[:2] == (2, 6)), 2)


if __name__ == '__main__':
    unittest.main()
