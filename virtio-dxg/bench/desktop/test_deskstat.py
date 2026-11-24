"""Replay synthetic guest flips including a stall and an inter-phase gap."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class DesktopPacing(unittest.TestCase):
    def test_unusable_traces_fail(self):
        phase = " 1.000: tracing_mark_write: files-scroll\n"
        end = " 2.000: tracing_mark_write: end\n"
        for trace in (phase, "# entries-in-buffer/entries-written: 100/150\n" + phase + end,
                      phase + " 1.1: drm_vblank_event_delivered: crtc=0\n"
                      " 1.2: drm_vblank_event_delivered: crtc=1\n" + end):
            with self.subTest(trace=trace), tempfile.TemporaryDirectory() as directory:
                path = Path(directory, "trace")
                path.write_text(trace, encoding="ascii")
                result = subprocess.run([sys.executable, Path(__file__).with_name("deskstat.py"), path],
                                        capture_output=True)
                self.assertNotEqual(result.returncode, 0)

    def test_elapsed_phase_includes_stalls_and_excludes_teardown(self):
        lines = (
            " 1.000: tracing_mark_write: idle",
            " 1.010: drm_vblank_event_delivered: crtc=0",
            " 1.020: drm_vblank_event_delivered: crtc=0",
            " 1.200: tracing_mark_write: end",
            " 2.000: tracing_mark_write: cpu",
            " 2.100: tracing_mark_write: end",
        )
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory, "desktop.trace")
            trace.write_text("\n".join(lines), encoding="ascii")
            output = subprocess.check_output(
                [sys.executable, Path(__file__).with_name("deskstat.py"), trace, "100"],
                text=True,
            )
        rows = [row.split() for row in output.splitlines()[1:]]
        self.assertEqual(rows, [
            ["idle", "10.0", "18", "180.00", "180.00"],
            ["cpu", "0.0", "10", "100.00", "100.00"],
        ])


if __name__ == "__main__":
    unittest.main()
