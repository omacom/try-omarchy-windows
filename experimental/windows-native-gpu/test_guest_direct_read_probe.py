"""No-window controls for the opt-in direct-read diagnostic sampling."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


class DirectReadProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cxx = shutil.which('c++')
        if cls.cxx is None:
            raise unittest.SkipTest('A C++ compiler is required')
        cls.source = Path(__file__).with_name('guest_direct_read_probe_test.cpp')

    def test_default_explicit_legacy_and_sampled_read_counts_and_retirement(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-direct-read-') as temporary:
            for flag, count in ((None, 10000), (0, 10000), (1, 2)):
                with self.subTest(flag=flag):
                    command = [self.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror']
                    if flag is not None:
                        command += ['-DWDDM_BRIDGE_SAMPLED_DIRECT_READS=' + str(flag)]
                    binary = Path(temporary) / ('probe-' + str(flag))
                    subprocess.run(command + [str(self.source), '-o', str(binary)], check=True,
                                   capture_output=True, text=True, timeout=30)
                    result = subprocess.run([str(binary)], check=True, capture_output=True,
                                            text=True, timeout=10)
                    self.assertIn('loads=' + str(count) + ' ', result.stdout)
                    self.assertIn('deviceLossRejected=true', result.stdout)
                    self.assertIn('gpu=false', result.stdout)

    def test_unknown_sampling_selection_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-direct-read-invalid-') as temporary:
            result = subprocess.run([self.cxx, '-std=c++17',
                '-DWDDM_BRIDGE_SAMPLED_DIRECT_READS=2', str(self.source),
                '-o', str(Path(temporary) / 'must-not-build')], capture_output=True,
                text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('Direct-read sampling selection must be zero or one', result.stderr)

    @unittest.skipUnless(hasattr(os, 'geteuid') and os.geteuid() == 0,
                         'Root is needed only for the real UID65534 IPC control')
    def test_actual_mapping_classes_keep_mmio_and_retirement_guards(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-direct-mapping-') as temporary:
            for flag, count in ((0, 10000), (1, 2)):
                with self.subTest(flag=flag):
                    binary = Path(temporary) / ('mapping-' + str(flag))
                    compiled = subprocess.run([self.cxx, '-std=c++17', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-pthread', '-DWDDM_BRIDGE_SAMPLED_DIRECT_READS=' + str(flag),
                        str(self.source.with_name('guest_direct_read_mapping_test.cpp')),
                        '-o', str(binary)], capture_output=True,
                        text=True, timeout=60)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
                    result = subprocess.run([str(binary)], capture_output=True,
                                            text=True, timeout=30)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(result.stdout.count('leaseReleased=true'), 7)
                    self.assertEqual(result.stdout.count('loads=' + str(count) + ' '), 7)
                    self.assertEqual(result.stdout.count('gpu=false kernel=mock'), 7)


if __name__ == '__main__':
    unittest.main()
