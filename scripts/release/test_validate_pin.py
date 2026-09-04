import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class StablePinTests(unittest.TestCase):
    def test_stable_release_pin_and_mismatched_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            version = re.search(r'currentVersion\s*=\s*"([^"]+)"', (ROOT / "app/update.go").read_text()).group(1)
            for relative in ["app/manifest.go", "app/update.go", "app/cmd/sign-update/main.go"]:
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text((ROOT / relative).read_text().replace(version, "v1.0.0"))
            fixture = root / "app/testdata/SHA256SUMS.v1.0.0"
            fixture.parent.mkdir(parents=True)
            fixture.write_bytes((ROOT / f"app/testdata/SHA256SUMS.{version}").read_bytes())
            command = [sys.executable, str(ROOT / "scripts/release/validate-pin.py")]
            result = subprocess.run(command + ["v1.0.0", "--root", str(root)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run(command + ["v1.0.1", "--root", str(root)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("currentVersion", result.stderr)
