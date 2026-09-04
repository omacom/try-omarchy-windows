from __future__ import annotations

import hashlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("verify-release-assets.py")


class VerifyReleaseAssetsTests(unittest.TestCase):
    def run_verify(self, root: Path, *names: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(SCRIPT), str(root / "SHA256SUMS"), str(root), *names],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_accepts_matching_regular_asset(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            payload = b"release payload"
            (root / "asset.zip").write_bytes(payload)
            digest = hashlib.sha256(payload).hexdigest()
            (root / "SHA256SUMS").write_text(
                f"{digest}  asset.zip\n", encoding="utf-8"
            )
            result = self.run_verify(root, "asset.zip")
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_mismatch_duplicate_and_unsafe_name(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "asset.zip").write_bytes(b"wrong")
            digest = hashlib.sha256(b"expected").hexdigest()
            manifest = root / "SHA256SUMS"

            manifest.write_text(f"{digest}  asset.zip\n", encoding="utf-8")
            self.assertNotEqual(self.run_verify(root, "asset.zip").returncode, 0)

            manifest.write_text(
                f"{digest}  asset.zip\n{digest}  asset.zip\n", encoding="utf-8"
            )
            self.assertNotEqual(self.run_verify(root, "asset.zip").returncode, 0)

            manifest.write_text(f"{digest}  ../asset.zip\n", encoding="utf-8")
            self.assertNotEqual(self.run_verify(root, "../asset.zip").returncode, 0)


if __name__ == "__main__":
    unittest.main()
