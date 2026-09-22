import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "nested_probe", Path(__file__).resolve().parents[1] / "guest/check-nested-virtualization.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class NestedProbeTests(unittest.TestCase):
    def test_unsupported_architecture_does_not_open_kvm(self):
        with patch.object(probe.platform, "machine", return_value="aarch64"), \
                patch.object(probe.os, "open") as opened:
            result = probe.probe()
        self.assertFalse(result["passed"])
        self.assertEqual(result["stage"], "architecture")
        opened.assert_not_called()

    def test_missing_or_inaccessible_kvm_is_not_a_pass(self):
        for failure in (FileNotFoundError("no KVM"), PermissionError("no access")):
            with self.subTest(failure=failure), \
                    patch.object(probe.platform, "machine", return_value="x86_64"), \
                    patch.object(probe.platform, "system", return_value="Linux"), \
                    patch.object(probe.os, "open", side_effect=failure):
                result = probe.probe()
            self.assertFalse(result["passed"])
            self.assertEqual(result["stage"], "open /dev/kvm")

    def test_api_mismatch_closes_descriptor(self):
        with patch.object(probe.platform, "machine", return_value="x86_64"), \
                patch.object(probe.platform, "system", return_value="Linux"), \
                patch.object(probe.os, "open", return_value=91), \
                patch.object(probe.os, "close") as closed, \
                patch.object(probe.fcntl, "ioctl", return_value=13):
            result = probe.probe()
        self.assertFalse(result["passed"])
        self.assertEqual(result["stage"], "KVM_GET_API_VERSION")
        closed.assert_called_once_with(91)


if __name__ == "__main__":
    unittest.main()
