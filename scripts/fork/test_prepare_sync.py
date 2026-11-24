import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("prepare_sync.py").resolve()


class SyncTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "-b", "master")
        self.git("config", "user.name", "Sync test")
        self.git("config", "user.email", "sync@example.invalid")
        self.git("config", "commit.gpgsign", "false")
        self.commit("shared.txt", "base\n")
        self.base = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("branch", "upstream")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, text=True, capture_output=True, check=True)

    def commit(self, name, text):
        (self.root / name).write_text(text)
        self.git("add", name)
        self.git("commit", "-m", name)

    def sync(self):
        return subprocess.run([sys.executable, str(SCRIPT), "--ref", "upstream"], cwd=self.root, text=True, capture_output=True)

    def test_merge_preserves_both_histories(self):
        self.commit("custom.txt", "fork\n")
        fork = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("switch", "upstream")
        self.commit("fix.txt", "upstream fix\n")
        upstream = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("switch", "master")
        result = self.sync()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("changed=true", result.stdout)
        self.assertEqual((self.root / "custom.txt").read_text(), "fork\n")
        self.assertEqual((self.root / "fix.txt").read_text(), "upstream fix\n")
        self.git("merge-base", "--is-ancestor", fork, "HEAD")
        self.git("merge-base", "--is-ancestor", upstream, "HEAD")
        self.assertIn("changed=false", self.sync().stdout)

    def test_conflict_retains_fork(self):
        self.commit("shared.txt", "fork change\n")
        fork = self.git("rev-parse", "HEAD").stdout.strip()
        self.git("switch", "upstream")
        self.commit("shared.txt", "upstream change\n")
        self.git("switch", "master")
        self.assertNotEqual(self.sync().returncode, 0)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), fork)
        self.assertEqual((self.root / "shared.txt").read_text(), "fork change\n")
        self.assertEqual(self.git("status", "--porcelain").stdout, "")

    def test_dirty_checkout_is_untouched(self):
        (self.root / "shared.txt").write_text("local work\n")
        self.assertNotEqual(self.sync().returncode, 0)
        self.assertEqual((self.root / "shared.txt").read_text(), "local work\n")
        self.assertEqual(self.git("rev-parse", "HEAD").stdout.strip(), self.base)


if __name__ == "__main__":
    unittest.main()
