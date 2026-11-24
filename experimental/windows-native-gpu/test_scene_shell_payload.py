"""Exercise the actual terminal fixture's Bash startup and readiness marker.

The Linux control substitutes an isolated marker path and a mock filesystem
listing. It does not assert anything about GPU pixels or the real Omarchy OS.
"""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


SCENE = Path(__file__).with_name('gpu-terminal-scene.sh')


class SceneShellPayloadTests(unittest.TestCase):
    def test_windows_checkout_retains_linux_shell_bytes(self):
        source = SCENE.read_bytes()
        self.assertTrue(source.startswith(b'#!/bin/bash\n'))
        self.assertNotIn(b'\r', source)

    @unittest.skipUnless(sys.platform == 'linux', 'Actual Bash execution requires Linux')
    def test_actual_script_reaches_marker_and_rejects_crlf_control(self):
        source = SCENE.read_bytes()
        for crlf in (False, True):
            with self.subTest(crlf=crlf), tempfile.TemporaryDirectory(prefix='omarchy-shell-control-') as tmp:
                root = Path(tmp)
                marker = root / 'ready'
                environment = root / 'bash-env'
                environment.write_text(
                    "ls() { printf '%s\\n' 'fixture omarchy files'; }\n"
                    'sleep() { kill -HUP "$BASHPID"; }\n', encoding='utf-8')
                script = source.decode('utf-8').replace('/tmp/omarchy-gpu-terminal-ready', shlex.quote(str(marker)))
                if crlf:
                    script = script.replace('\n', '\r\n')
                env = dict(os.environ, BASH_ENV=str(environment))
                result = subprocess.run(['bash', '-c', script], cwd=root, env=env,
                                        capture_output=True, timeout=5)
                if crlf:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(b'invalid option', result.stderr)
                    self.assertFalse(marker.exists())
                else:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(b'Omarchy', result.stdout)
                    self.assertIn(b'fixture omarchy files', result.stdout)
                    self.assertTrue(marker.read_text(encoding='utf-8').strip().isdigit())
        self.assertEqual(SCENE.read_bytes(), source)


if __name__ == '__main__':
    unittest.main()
