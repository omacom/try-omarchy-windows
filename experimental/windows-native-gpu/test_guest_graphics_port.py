"""Linux controls for graphics port selection. No device is opened or loaded.

--device-fixtures requires CAP_MKNOD for temporary /dev/null-identity nodes.
Those fixtures validate discovery, not a real virtio transport or GPU workload.
"""
from pathlib import Path
import argparse
import ctypes
import errno
import os
import stat
import subprocess
import tempfile
import unittest

DEVICE_FIXTURES = False


class GraphicsPortTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix='graphics-port-build-')
        root = Path(cls.build.name)
        source = root/'controls.c'
        source.write_text('''#include "guest_graphics_port.h"
int resolve(const char* sysfs,const char* devices,char* path,size_t size) {
    return wddm_graphics_port_resolve(sysfs,devices,path,size);
}
int node_name(const char* text) { return wddm_port_node_name(text); }
int device_id(const char* text,unsigned* major_id,unsigned* minor_id) {
    dev_t value=0;
    if(wddm_port_device(text,&value)) return -1;
    *major_id=major(value);*minor_id=minor(value);return 0;
}
''', encoding='utf-8')
        subprocess.run(['cc', '-D_GNU_SOURCE', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-shared', '-fPIC', '-I', str(Path(__file__).resolve().parent), str(source),
            '-o', str(root/'controls.so')], check=True)
        cls.lib = ctypes.CDLL(str(root/'controls.so'), use_errno=True)
        cls.lib.resolve.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.resolve.restype = ctypes.c_int
        cls.lib.node_name.argtypes = [ctypes.c_char_p]
        cls.lib.node_name.restype = ctypes.c_int
        cls.lib.device_id.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        cls.lib.device_id.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix='graphics-port-fixture-')
        self.root = Path(self.fixture.name)
        self.sysfs, self.devices = self.root/'sysfs', self.root/'devices'
        self.sysfs.mkdir()
        self.devices.mkdir()

    def tearDown(self):
        self.fixture.cleanup()

    def port(self, node='vport9p1', name=b'org.7wdev.wddm\n', device=b'1:3\n'):
        path = self.sysfs/node
        path.mkdir()
        (path/'name').write_bytes(name)
        (path/'dev').write_bytes(device)
        return path

    def resolve(self, capacity=512):
        output = ctypes.create_string_buffer(b'not-a-result', 512)
        result = self.lib.resolve(os.fsencode(self.sysfs), os.fsencode(self.devices), output, capacity)
        return result, ctypes.get_errno(), output.value

    def character(self, node='vport9p1', device=None):
        if not DEVICE_FIXTURES:
            self.skipTest('use --device-fixtures with CAP_MKNOD for character-device controls')
        os.mknod(self.devices/node, stat.S_IFCHR | 0o600, os.makedev(1, 3) if device is None else device)

    def test_kernel_node_spelling(self):
        for node in (b'vport0p1', b'vport9p2', b'vport4294967295p4294967295'):
            with self.subTest(node=node): self.assertEqual(self.lib.node_name(node), 1)
        for node in (b'', b'vport', b'vport0', b'vport0p', b'vport-1p1', b'vport01p1',
                     b'vport0p01', b'vport4294967296p1', b'vport0p4294967296',
                     b'vport0p1/../input', b'vport0p1\n', b'event0', b'../vport0p1'):
            with self.subTest(node=node): self.assertEqual(self.lib.node_name(node), 0)

    def test_device_numbers_are_bounded_canonical_decimal(self):
        major_id, minor_id = ctypes.c_uint(), ctypes.c_uint()
        for text, expected in ((b'1:0\n', (1, 0)), (b'4095:1048575\n', (4095, 1048575))):
            self.assertEqual(self.lib.device_id(text, major_id, minor_id), 0)
            self.assertEqual((major_id.value, minor_id.value), expected)
        for text in (b'0:1\n', b'4096:1\n', b'1:1048576\n', b'1:1', b'01:1\n',
                     b'1:01\n', b'+1:1\n', b' 1:1\n', b'1:1\nextra', b'99999999999999999999:1\n'):
            with self.subTest(text=text): self.assertEqual(self.lib.device_id(text, major_id, minor_id), -1)

    def test_missing_named_port_does_not_fall_back_to_camera(self):
        self.port('vport0p1', b'dev.tryomarchy.camera\n')
        self.port('vport0p2', b'dev.tryomarchy.authentication\n')
        self.assertEqual(self.resolve(), (-1, errno.ENODEV, b''))

    def test_moved_graphics_port_with_original_devices(self):
        self.port('vport0p1', b'dev.tryomarchy.camera\n')
        self.port('vport0p2', b'dev.tryomarchy.authentication\n')
        self.port('vport19p7')
        self.character('vport19p7')
        self.assertEqual(self.resolve()[::2], (0, os.fsencode(self.devices/'vport19p7')))
        self.assertFalse((self.devices/'vport0p1').exists())

    def test_duplicate_name_rejected(self):
        self.port('vport0p1')
        self.port('vport9p1')
        self.assertEqual(self.resolve(), (-1, errno.EEXIST, b''))

    def test_named_regular_file_rejected(self):
        self.port()
        (self.devices/'vport9p1').touch()
        self.assertEqual(self.resolve(), (-1, errno.ENODEV, b''))

    def test_named_symlink_rejected(self):
        self.port()
        (self.devices/'vport9p1').symlink_to('/dev/null')
        self.assertEqual(self.resolve(), (-1, errno.ENODEV, b''))

    def test_character_identity_mismatch_rejected(self):
        self.port(device=b'1:5\n')
        self.character()
        self.assertEqual(self.resolve(), (-1, errno.ENODEV, b''))

    def test_short_output_rejected_and_cleared(self):
        self.port()
        self.assertEqual(self.resolve(2), (-1, errno.ENAMETOOLONG, b''))

    def test_missing_node_retried_without_guessing(self):
        self.port()
        self.assertEqual(self.resolve(), (-1, errno.ENOENT, b''))

    def test_attribute_symlink_rejected(self):
        path = self.port()
        (path/'name').unlink()
        (path/'name').symlink_to(path/'dev')
        self.assertEqual(self.resolve(), (-1, errno.ELOOP, b''))

    def test_embedded_nul_and_overlong_attributes_rejected(self):
        path = self.port()
        for name in (b'org.7wdev.wddm\n\0extra', b'x'*127, b''):
            (path/'name').write_bytes(name)
            self.assertEqual(self.resolve(), (-1, errno.EINVAL, b''))

    def test_malformed_device_attribute_rejected(self):
        self.port(device=b'242:1\nother')
        self.assertEqual(self.resolve(), (-1, errno.EINVAL, b''))

    def test_directory_scan_bounded(self):
        for index in range(257): (self.sysfs/f'unrelated-{index}').touch()
        self.assertEqual(self.resolve(), (-1, errno.E2BIG, b''))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device-fixtures', action='store_true')
    args, rest = parser.parse_known_args()
    DEVICE_FIXTURES = args.device_fixtures
    if DEVICE_FIXTURES and os.geteuid() != 0:
        parser.error('--device-fixtures requires a root-owned temporary Linux fixture')
    unittest.main(argv=[__file__, *rest])
