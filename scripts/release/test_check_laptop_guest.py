"""Regression checks for accepting corrupted or incomplete laptop evidence."""
import copy
from contextlib import redirect_stderr
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("laptop", Path(__file__).with_name("check-laptop-guest.py"))
laptop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(laptop)


class AcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.baseline = {
            "identity": {"uid": 1000, "machine_id_sha256": "original"},
            "files": {"unicode.txt": "original", "preferences.json": "original"},
            "directories": ["empty-directory"],
            "checks": [{"name": "desktop_output", "passed": True}],
            "facts": {"failed_user_units": [], "root_free_bytes": 1024**3,
                      "compat_version": "48:kernel", "kernel": "kernel"},
        }

    def test_intact_data_and_exact_pin_pass(self):
        self.assertEqual(laptop.evaluate(copy.deepcopy(self.baseline), self.baseline,
                                         {"compat_version": "48:kernel"}), [])

    def test_missing_or_changed_files_settings_identity_and_directories_fail(self):
        for field in ("files", "identity", "directories"):
            with self.subTest(field=field):
                report = copy.deepcopy(self.baseline)
                report[field] = {} if field != "directories" else []
                self.assertTrue(laptop.evaluate(report, self.baseline))
        report = copy.deepcopy(self.baseline)
        report["files"]["preferences.json"] = "changed"
        self.assertTrue(laptop.evaluate(report, self.baseline))

    def test_unusable_desktop_cannot_pass(self):
        report = copy.deepcopy(self.baseline)
        report["checks"] = [{"name": "desktop_output", "passed": False, "error": "no output"}]
        self.assertIn("desktop_output: no output", laptop.evaluate(report))

    def test_wrong_candidate_cannot_pass(self):
        self.assertTrue(laptop.evaluate(self.baseline, expected={"compat_version": "49:kernel"}))

    def test_old_running_kernel_cannot_pass_matching_installed_pins(self):
        report = copy.deepcopy(self.baseline)
        report["facts"]["kernel"] = "old-kernel"
        self.assertIn("running kernel differs from the pinned candidate",
                      laptop.evaluate(report, expected={"compat_version": "48:kernel"}))

    def test_invalid_baseline_is_rejected_before_ssh(self):
        passing = dict(self.baseline, phase="seed", passed=True, run_id="test")
        missing = dict(passing)
        del missing["files"]
        for baseline in ({}, dict(passing, passed=False), dict(passing, phase="verify"), missing):
            with self.subTest(baseline=baseline), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "key").touch()
                (root / "hosts").touch()
                (root / "baseline.json").write_text(json.dumps(baseline))
                argv = ["check", "verify", "--host", "omarchy@127.0.0.1", "--port", "2222",
                        "--key", str(root/"key"), "--known-hosts", str(root/"hosts"), "--run-id", "test",
                        "--output", str(root/"result.json"), "--baseline", str(root/"baseline.json")]
                with mock.patch.object(laptop.sys, "argv", argv), mock.patch.object(laptop.subprocess, "run") as ssh:
                    with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as failure:
                        laptop.main()
                    self.assertEqual(failure.exception.code, 2)
                    ssh.assert_not_called()

    def test_failed_services_and_low_disk_space_fail(self):
        report = copy.deepcopy(self.baseline)
        report["facts"]["failed_user_units"] = ["clipboard-bridge.service failed"]
        report["facts"]["root_free_bytes"] = 0
        self.assertEqual(len(laptop.evaluate(report)), 2)


if __name__ == "__main__":
    unittest.main()
