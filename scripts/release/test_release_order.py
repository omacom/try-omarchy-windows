import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("release_order", Path(__file__).with_name("release-order.py"))
release_order = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_order)


class ReleaseOrderTests(unittest.TestCase):
    def test_transitions(self):
        for candidate, current, expected in [
            ("v0.0.12-preview", "v0.0.11-preview", True),
            ("v1.0.0", "v0.0.12-preview", True),
            ("v1.0.0", "v1.0.0-preview", True),
            ("v1.1.0-preview", "v1.0.0", False),
            ("v1.0.0", "v1.0.0", False),
            ("v1.0.0", "v1.0.1", False),
            ("v1.0.1", "v1.0.0", True),
        ]:
            with self.subTest(candidate=candidate, current=current):
                self.assertEqual(release_order.should_promote(candidate, current), expected)

    def test_rejects_invalid_tags(self):
        for tag in ["latest", "v01.0.0", "v1.0.0-rc.1", "v1.0.0+build", "v1.0.0\n"]:
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release_order.should_promote(tag, "v0.0.11-preview")
