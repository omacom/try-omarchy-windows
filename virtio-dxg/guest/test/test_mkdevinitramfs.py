"""Small synthetic boot archives for the matching-kernel checks."""
import io
from pathlib import Path
import tempfile
import unittest

from mkdevinitramfs import boot_config, kernel_release, staged_image
from mkinitramfs import entry


def archive(*trees):
    data = io.BytesIO()
    entry(data, 'config', 0o100644, b'LATEHOOKS="base"\n')
    for tree in trees:
        entry(data, tree, 0o100644, b'')
    entry(data, 'TRAILER!!!', 0)
    return data.getvalue()


class BootInputs(unittest.TestCase):
    def test_module_indexes(self):
        for index in ('usr/lib/modules/7.3.5/modules.dep', './lib/modules/7.3.5/modules.dep.bin'):
            with self.subTest(index=index):
                self.assertEqual(boot_config(archive(index), '7.3.5'), 'LATEHOOKS="base"\n')

    def test_missing_mismatched_and_mixed_trees(self):
        old = 'lib/modules/7.2.15/modules.dep'
        new = 'lib/modules/7.3.5/modules.dep'
        for indexes in ((), (old,), (old, new)):
            with self.subTest(indexes=indexes), self.assertRaises(ValueError):
                boot_config(archive(*indexes), '7.3.5')

    def test_kernel_header_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            kernel = Path(tmp, 'vmlinuz-linux')
            header = bytearray(0x300)
            header[0x202:0x206] = b'HdrS'
            header[0x20e:0x210] = (0x100).to_bytes(2, 'little')
            kernel.write_bytes(header + b'7.3.5-arch1-1 (builder)\0')
            self.assertEqual(kernel_release(kernel), '7.3.5-arch1-1')
            kernel.write_bytes(b'HdrS')
            with self.assertRaises(ValueError):
                kernel_release(kernel)

    def test_failed_update_keeps_previous_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp, 'dev.img')
            target.write_bytes(b'original')
            with self.assertRaises(OSError), staged_image(target) as out:
                out.write(b'incomplete')
                raise OSError('simulated write failure')
            self.assertEqual(target.read_bytes(), b'original')
            self.assertEqual(list(target.parent.iterdir()), [target])
            with staged_image(target) as out:
                out.write(b'complete')
            self.assertEqual(target.read_bytes(), b'complete')


if __name__ == '__main__':
    unittest.main()
