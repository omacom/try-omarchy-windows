"""Corrupt retained hardware evidence; CI never loads a vendor driver."""
import copy
import json
from pathlib import Path
import unittest
from verify_egl_native_sync_capture import validate_egl_native_sync
from verify_mesa_fence_fd_capture import validate_mesa_fence_fds


class EglNativeSyncMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((Path(__file__).resolve().parents[2] /
            'docs/evidence/EGL-NATIVE-SYNC-2026-10-13.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None, enabled=True):
        p = self.proof if proof is None else proof
        return validate_egl_native_sync(p['report'], '\n'.join(p['guestLines']), p['records'], enabled=enabled)

    def change(self, before, after):
        lines = self.proof['guestLines']
        self.assertTrue(any(before in l for l in lines))
        self.proof['guestLines'] = [l.replace(before, after) for l in lines]

    def test_real_runs_prove_callbacks_without_claiming_a_desktop(self):
        self.assertEqual(len(self.evidence['runs']), 2)
        for run in self.evidence['runs']:
            audit = self.check(run['proof'])
            self.assertEqual(audit, run['audit'])
            pins = self.evidence['privateArtifactSha256']; report = run['proof']['report']
            for key, artifact in (('driverBridgeSha256', 'nativeHost'), ('qemuSha256', 'qemu'),
                                  ('kernelSha256', 'guestKernel'), ('initramfsSha256', 'enabledImage')):
                self.assertEqual(report[key], pins[artifact])
            source = run['proof']['artifactSource']
            self.assertEqual(source['revision'], self.evidence['guestSourceRevision'])
            self.assertEqual(source['bridgeSha256'], pins['nativeHost'])
            self.assertEqual(source['initramfsSha256'], pins['enabledImage'])
            for key in ('eglNativeFenceVerified', 'nativeGpuWaitThroughEglVerified',
                        'completedEglNativeLifetimeVerified', 'zeroTargetEglImportVerified',
                        'canceledEglImportRejected', 'synchronizationOnlyResidencySignalsVerified',
                        'nativeEvictionCycleVerified'):
                self.assertIs(audit[key], True)
            for key in ('generalEglConformanceVerified', 'foreignSyncFileImportVerified',
                        'mergedSyncFileImportVerified', 'forcedPhysicalPageOutVerified',
                        'desktopVerified', 'gbmVerified', 'dmaBufVerified',
                        'desktopStabilityVerified', 'nearNativePerformanceVerified'):
                self.assertIs(audit[key], False)
            self.assertEqual(audit['syncFilesCreated'], audit['syncFilesReleased'])
            self.assertEqual(audit['nativeGpuSignalTarget'], 4294967387)

    def test_disabled_control_and_legacy_timeline_regression(self):
        disabled = self.evidence['disabledControl']
        self.assertEqual(self.check(disabled['proof'], False), disabled['audit'])
        with self.assertRaises(ValueError): self.check(disabled['proof'])
        self.assertTrue(self.evidence['enabledImageManifest']['nativeSyncFilesEnabled'])
        self.assertFalse(self.evidence['disabledImageManifest']['nativeSyncFilesEnabled'])
        run = self.evidence['timelineRegression']; p = run['proof']
        self.assertEqual(validate_mesa_fence_fds(p['report'], '\n'.join(p['guestLines']),
            p['records'], transport='authenticated-broker'), run['audit'])

    def test_old_hardware_build_and_launch_failures_remain_failures(self):
        self.assertEqual(len(self.evidence['failures']), 3)
        self.assertEqual(len(self.evidence['buildFailures']), 1)
        self.assertEqual(len(self.evidence['launchFailures']), 1)
        for run in self.evidence['failures'] + self.evidence['launchFailures']:
            with self.assertRaises(ValueError): self.check(run['proof'])

    def test_callback_receipts_must_be_unique_and_ordered(self):
        for prefix in ('EGL_NATIVE_SYNC_BEGIN ', 'EGL_NATIVE_SYNC_EXPORT ', 'EGL_NATIVE_SYNC_GPU ',
                       'EGL_NATIVE_SYNC_LIFETIME ', 'EGL_NATIVE_SYNC_RESIDENCY ', 'EGL_NATIVE_SYNC_REJECT ',
                       'EGL_NATIVE_SYNC_COUNTS ', 'EGL_NATIVE_SYNC_COMPLETE ', 'KERNEL_SYNC_FILE_EXIT '):
            for duplicate in (False, True):
                self.setUp(); lines = self.proof['guestLines']
                line = next(l for l in lines if l.startswith(prefix))
                lines.append(line) if duplicate else lines.remove(line)
                with self.subTest(prefix=prefix, duplicate=duplicate), self.assertRaises(ValueError): self.check()
        self.setUp(); lines = self.proof['guestLines']
        line = next(l for l in lines if l.startswith('EGL_NATIVE_SYNC_LIFETIME '))
        lines.remove(line); lines.insert(0, line)
        with self.assertRaises(ValueError): self.check()

    def test_pending_egl_fence_and_error_status_cannot_be_faked(self):
        for before, after in (('dependentEglExportBefore=pending', 'dependentEglExportBefore=signaled'),
                              ('acknowledgementBefore=0', 'acknowledgementBefore=1'),
                              ('target=4294967387', 'target=91'),
                              ('canceledClientWait=timeout', 'canceledClientWait=satisfied'),
                              ('firstEglSyncDestroyed=true', 'firstEglSyncDestroyed=false'),
                              ('failedInputFdRetained=true', 'failedInputFdRetained=false')):
            self.setUp(); self.change(before, after)
            with self.subTest(after=after), self.assertRaises(ValueError): self.check()

    def test_native_queue_signals_and_residency_context_are_required(self):
        for marker, key, value in (('nativeGpuQueueWaitAccepted', 'cpuWait', True),
                                  ('nativeGpuQueueWaitAccepted', 'firstTarget', 91),
                                  ('nativeContextSignalStatus', 'contextFlags', 16),
                                  ('nativeAsyncContextSignalRetired', 'cpuValueWrittenByBridge', True),
                                  ('nativeAsyncContextSignalRetired', 'observed', 0),
                                  ('nativeHwQueueSignalAccepted', 'observed', 6)):
            self.setUp(); records = self.proof['records']
            record = next(r for r in records if marker in r and
                (marker != 'nativeHwQueueSignalAccepted' or r['index'] == 6))
            record[key] = value
            with self.subTest(marker=marker, key=key), self.assertRaises(ValueError): self.check()
        self.setUp(); records = self.proof['records']
        n = next(i for i, r in enumerate(records) if 'nativeAsyncContextSignalAccepted' in r and r['index'] == 3)
        release = records.pop(n); records.insert(0, release)
        with self.assertRaises(ValueError): self.check()

    def test_real_budget_and_eviction_receipts_cannot_be_skipped(self):
        for marker, key, value in (('nativeMemoryBudgetQueried', 'currentProcessOnly', False),
                                  ('nativeMemoryBudgetQueried', 'budget', 1073741824),
                                  ('nativeVendorEviction', 'nativeCall', False),
                                  ('nativeVendorEviction', 'pendingCommands', 1),
                                  ('nativeVendorEviction', 'ntstatus', -1)):
            self.setUp(); next(r for r in self.proof['records'] if marker in r)[key] = value
            with self.subTest(marker=marker, key=key), self.assertRaises(ValueError): self.check()
        self.setUp()
        line = 'LINUX_BRIDGE nativeVendorResident=true count=1 status=259 fence=7010 bytesToTrim=0'
        n = next(i for i, l in enumerate(self.proof['guestLines']) if l.startswith('LINUX_BRIDGE nativeVendorEviction=true'))
        index = self.proof['guestLines'].index(line, n)
        self.proof['guestLines'].pop(index)
        with self.assertRaises(ValueError): self.check()

    def test_lifetime_privileges_and_kernel_cleanup_are_required(self):
        for before, after in (('deniedResolve=true', 'deniedResolve=false'),
                              ('effectiveCapabilities=0', 'effectiveCapabilities=1'),
                              ('released=10 cleanup=true', 'released=9 cleanup=true'),
                              ('retainedViews=0', 'retainedViews=1')):
            self.setUp(); self.change(before, after)
            with self.subTest(after=after), self.assertRaises(ValueError): self.check()
        self.setUp(); records = self.proof['records']
        close = records.pop(next(i for i, r in enumerate(records) if 'nativeSyncHandleClosed' in r))
        records.insert(0, close)
        with self.assertRaises(ValueError): self.check()

    def test_shader_pixels_still_need_an_independent_audit(self):
        lines = self.proof['guestLines']
        n = next(i for i, l in enumerate(lines) if l.startswith('OPENGL_PIXEL_ROW frame=1 y=0 '))
        prefix, rgba = lines[n].split('rgba=')
        lines[n] = prefix + 'rgba=ff' + rgba[2:]
        with self.assertRaises(ValueError): self.check()


if __name__ == '__main__': unittest.main()
