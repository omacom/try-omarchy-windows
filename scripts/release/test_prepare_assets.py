from __future__ import annotations

import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("prepare-assets.sh")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class PrepareAssetsTests(unittest.TestCase):
    def test_adds_locked_runtime_and_source_archives(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            artifacts = root / "artifacts"
            payloads = root / "payloads"
            artifacts.mkdir()
            payloads.mkdir()
            seed = b"guest metadata"
            (artifacts / "guest.json").write_bytes(seed)
            (artifacts / "SHA256SUMS").write_text(
                f"{sha256(seed)}  guest.json\n", encoding="utf-8"
            )

            entries = {}
            for role, name in (
                ("runtime", "runtime.zip"),
                ("source", "source.zip"),
            ):
                data = f"{role} archive".encode()
                path = payloads / name
                path.write_bytes(data)
                entries[role] = {
                    "url": path.as_uri(),
                    "filename": name,
                    "sha256": sha256(data),
                }
            lock = root / "runtime.lock.json"
            lock.write_text(json.dumps(entries), encoding="utf-8")
            env = os.environ.copy()
            env["TRYOMARCHY_RUNTIME_LOCK"] = str(lock)

            result = subprocess.run(
                ["bash", str(SCRIPT), str(artifacts)],
                text=True,
                capture_output=True,
                check=False,
                env=env,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            sums = (artifacts / "SHA256SUMS").read_text(encoding="utf-8")
            self.assertIn(f"{entries['runtime']['sha256']}  runtime.zip\n", sums)
            self.assertIn(f"{entries['source']['sha256']}  source.zip\n", sums)


if __name__ == "__main__":
    unittest.main()
