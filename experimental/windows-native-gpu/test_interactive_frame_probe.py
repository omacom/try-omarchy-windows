"""Compile and exercise the interactive queue without opening windows or a GPU."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile
import unittest


class InteractiveFrameProbe(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which('cc')
        if cls.cc is None:
            raise unittest.SkipTest('A POSIX C compiler is required for this guest hook')
        cls.gpu = Path(__file__).resolve().parent
        cls.source = cls.gpu / 'interactive-frame-probe'

    def test_published_sources_match_the_trial_and_declared_hardening(self):
        receipt = json.loads((self.source / 'source-receipt.json').read_bytes())
        for name, digest in receipt['publishedSourceSha256'].items():
            self.assertEqual(hashlib.sha256((self.source / name).read_bytes()).hexdigest(), digest)
        changed = [name for name, digest in receipt['baseSourceSha256'].items()
                   if digest != receipt['publishedSourceSha256'][name]]
        self.assertEqual(changed, ['active_queue_probe.h'])
        self.assertFalse(receipt['postTrialHardwareVerified'])
        self.assertFalse(receipt['officialInputChanged'])

    def test_queue_and_complete_controller_build(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-background-queue-') as temporary:
            output = Path(temporary)
            common = [self.cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                      '-DHYPRLAND_QUEUE_END_STREAM=1', '-DHYPRLAND_QUEUE_FRAMES=1800',
                      '-I' + str(self.gpu)]
            subprocess.run(common + [str(self.source / 'queue_test.c'), '-ldl',
                           '-o', str(output / 'queue-test')], check=True,
                           capture_output=True, text=True, timeout=60)
            result = subprocess.run([str(output / 'queue-test')], check=True,
                                    capture_output=True, text=True, timeout=15)
            self.assertIn('windowsOpened=0', result.stdout)
            self.assertIn('lateFinishRejected=true', result.stdout)
            self.assertIn('failurePreserved=true', result.stdout)
            subprocess.run(common + ['-DWDDM_BRIDGE_COMPOSITOR_HOST_PRESENT=1',
                           '-DWDDM_BRIDGE_COMPOSITOR_HOST_SCENE=1',
                           '-DWDDM_BRIDGE_DESKTOP_RESIZE_PROBE=1', '-shared', '-fPIC',
                           str(self.source / 'active_probe.c'), '-ldl', '-pthread',
                           '-o', str(output / 'interactive-controller.so')], check=True,
                           capture_output=True, text=True, timeout=60)

    def test_missing_end_ack_and_invalid_minimum_are_compile_errors(self):
        with tempfile.TemporaryDirectory(prefix='omarchy-background-invalid-') as temporary:
            for end, frames in ((0, 1800), (1, 1), (1, 1801)):
                with self.subTest(end=end, frames=frames):
                    result = subprocess.run([self.cc, '-std=c11', '-O2',
                        '-DHYPRLAND_QUEUE_END_STREAM=' + str(end),
                        '-DHYPRLAND_QUEUE_FRAMES=' + str(frames), '-I' + str(self.gpu),
                        str(self.source / 'queue_test.c'), '-ldl',
                        '-o', str(Path(temporary) / 'must-not-build')],
                        capture_output=True, text=True, timeout=60)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn('#error', result.stderr)


if __name__ == '__main__':
    unittest.main()
