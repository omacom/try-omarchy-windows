"""Exercise lifecycle auditing with retained real runs and incomplete-run controls."""
import copy
import json
from pathlib import Path
import unittest

from verify_hyprland_lifecycle_capture import validate


class HyprlandLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        evidence = Path(__file__).resolve().parents[2] / 'docs/evidence/HYPRLAND-LOGGING-LIFETIME-DEVELOPMENT-2026-10-15.json'
        cls.runs = json.loads(evidence.read_text())['runs']

    def test_retained_failed_finalizers_rejected(self):
        for run in self.runs[:2]:
            with self.subTest(run=run['run']), self.assertRaises(ValueError):
                validate(run['report'], run['guestReceipts'])

    def test_actual_clean_runs_accept_only_lifecycle(self):
        for run in self.runs[2:]:
            with self.subTest(run=run['run']):
                result = validate(run['report'], run['guestReceipts'])
                self.assertTrue(result['compositorLifecycleVerified'])
                for field in ('compositorPixelsVerified', 'windowsPresentationVerified', 'desktopVerified', 'nearNativePerformanceVerified'):
                    self.assertFalse(result[field])

    def test_main_return_without_process_exit_rejected(self):
        run = self.runs[-1]
        with self.assertRaises(ValueError):
            validate(run['report'], run['guestReceipts'].split('BRIDGE_RUNTIME_EXIT=0')[0])

    def test_truncated_buffer_metadata_rejected(self):
        run = self.runs[-1]
        with self.assertRaises(ValueError):
            validate(run['report'], run['guestReceipts'].replace(' stride=2560', '', 1))

    def test_duplicate_commit_rejected(self):
        run = self.runs[-1]
        with self.assertRaises(ValueError):
            validate(run['report'], run['guestReceipts'].replace('COMMIT index=2', 'COMMIT index=1', 1))

    def test_fence_reclaimed_only_after_vm_exit_rejected(self):
        run = self.runs[-1]
        report = copy.deepcopy(run['report'])
        report['ownedQemuControl']['fenceMappingsReleasedAfterVmExit'] = 1
        with self.assertRaises(ValueError):
            validate(report, run['guestReceipts'])

    def test_missing_allocation_unmap_rejected(self):
        run = self.runs[-1]
        report = copy.deepcopy(run['report'])
        report['ownedQemuControl']['allocationUnmapAcknowledgements'] -= 1
        with self.assertRaises(ValueError):
            validate(report, run['guestReceipts'])

    def test_failed_gpu_work_rejected(self):
        run = self.runs[-1]
        report = copy.deepcopy(run['report'])
        report['disconnectCleanup']['failedNativeSubmissions'] = 1
        with self.assertRaises(ValueError):
            validate(report, run['guestReceipts'])

    def test_live_gpu_object_rejected(self):
        run = self.runs[-1]
        report = copy.deepcopy(run['report'])
        report['disconnectCleanup']['liveDevices'] = 1
        with self.assertRaises(ValueError):
            validate(report, run['guestReceipts'])

    def test_forced_vm_stop_rejected(self):
        run = self.runs[-1]
        report = copy.deepcopy(run['report'])
        report['ownedQemuControl']['qemuForcedStop'] = True
        with self.assertRaises(ValueError):
            validate(report, run['guestReceipts'])


if __name__ == '__main__':
    unittest.main()
