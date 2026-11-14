"""Corrupt retained real GPU captures; CI never loads a vendor driver."""
import copy
import json
from pathlib import Path
import unittest
from verify_native_buffer_capture import validate_native_buffers
from verify_egl_native_sync_capture import validate_egl_native_sync


class NativeBufferMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((Path(__file__).resolve().parents[2] /
            'docs/evidence/NATIVE-BUFFER-2026-10-14.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None, enabled=True):
        p = self.proof if proof is None else proof
        return validate_native_buffers(p['report'], '\n'.join(p['guestLines']), p['records'], enabled=enabled)

    def test_final_hardware_runs_and_exact_artifacts(self):
        self.assertEqual(len(self.evidence['runs']), 2)
        for run in self.evidence['runs']:
            audit = self.check(run['proof'])
            self.assertEqual(audit, run['audit'])
            self.assertTrue(audit['nativeDmaBufLifetimeVerified'])
            self.assertTrue(audit['sharedStorageVerified'])
            self.assertEqual(audit['nativeBuffersCreated'], audit['nativeBuffersReleased'])
            self.assertEqual(audit['nativeBufferContractChecks'], 200)
            self.assertEqual(audit['retiredNativeCommands'], 49)
            for key in ('generalDmaBufInteropVerified', 'eglImageImportVerified', 'gbmVerified',
                        'drmDeviceVerified', 'implicitBufferSynchronizationVerified', 'desktopVerified',
                        'desktopStabilityVerified', 'nearNativePerformanceVerified'):
                self.assertFalse(audit[key])
            p = run['proof']; pins = self.evidence['privateArtifactSha256']
            self.assertEqual(p['artifactSource']['revision'], self.evidence['guestSourceRevision'])
            self.assertEqual(p['artifactSource']['dirty'], [])
            for key, artifact in (('driverBridgeSha256', 'nativeHost'), ('qemuSha256', 'qemu'),
                                  ('kernelSha256', 'guestKernel'), ('initramfsSha256', 'enabledImage')):
                self.assertEqual(p['report'][key], pins[artifact])

    def test_disabled_control_and_egl_regression(self):
        disabled = self.evidence['disabledControl']
        self.assertEqual(self.check(disabled['proof'], False), disabled['audit'])
        with self.assertRaises(ValueError): self.check(disabled['proof'])
        with self.assertRaises(ValueError): self.check(enabled=False)
        self.assertTrue(self.evidence['enabledImageManifest']['nativeBuffersEnabled'])
        self.assertFalse(self.evidence['disabledImageManifest']['nativeBuffersEnabled'])
        run = self.evidence['eglRegression']; p = run['proof']
        self.assertEqual(validate_egl_native_sync(p['report'], '\n'.join(p['guestLines']), p['records']), run['audit'])
        self.assertEqual(p['guestLines'].count('KERNEL_NATIVE_BUFFER_EXIT held=0 bytes=0 created=71 released=71 cleanup=true'), 1)

    def test_kernel_version_failure_cannot_be_accepted(self):
        self.assertEqual(len(self.evidence['configurationFailures']), 1)
        p = self.evidence['configurationFailures'][0]['proof']
        with self.assertRaises(ValueError): self.check(p)
        self.assertNotIn('MESA_RESOURCE_FD_BEGIN', '\n'.join(p['guestLines']))
        self.assertNotEqual(p['report']['kernelSha256'], self.evidence['privateArtifactSha256']['guestKernel'])

    def test_every_buffer_lifetime_receipt_is_unique(self):
        for prefix in ('NATIVE_BUFFER_CONTRACT_PASS ', 'NATIVE_BUFFER_GPU_EXPORT ',
                       'NATIVE_BUFFER_GPU_LIFETIME ', 'NATIVE_BUFFER_GPU_COMPLETE ', 'KERNEL_NATIVE_BUFFER_EXIT '):
            for duplicate in (False, True):
                p = copy.deepcopy(self.proof)
                receipt = next(l for l in p['guestLines'] if l.startswith(prefix))
                if duplicate: p['guestLines'].append(receipt)
                else: p['guestLines'].remove(receipt)
                with self.subTest(prefix=prefix, duplicate=duplicate), self.assertRaises(ValueError): self.check(p)

    def test_identity_cleanup_and_order_corruption(self):
        for before, after in (('checks=200 dmaBuf=true', 'checks=199 dmaBuf=true'),
                              ('logicalBytes=33177600', 'logicalBytes=4096'),
                              ('duplicateLifetime=true', 'duplicateLifetime=false'),
                              ('originalNativeFdClosed=true', 'originalNativeFdClosed=false'),
                              ('created=72 released=72', 'created=72 released=71'),
                              ('rawHostHandle=false', 'rawHostHandle=true')):
            p = copy.deepcopy(self.proof)
            self.assertTrue(any(before in l for l in p['guestLines']))
            p['guestLines'] = [l.replace(before, after) for l in p['guestLines']]
            with self.subTest(before=before), self.assertRaises(ValueError): self.check(p)
        p = copy.deepcopy(self.proof)
        receipt = next(l for l in p['guestLines'] if l.startswith('NATIVE_BUFFER_GPU_LIFETIME '))
        p['guestLines'].remove(receipt); p['guestLines'].insert(0, receipt)
        with self.assertRaises(ValueError): self.check(p)

    def test_full_4k_storage_and_native_import_are_required(self):
        for before, after in (('rgba=ff0000ff', 'rgba=ff000000'), ('sourceReleased=true fdClosed=true', 'sourceReleased=false fdClosed=true'),
                              ('fnv1a=842d8f16d3d2a325', 'fnv1a=0000000000000000')):
            p = copy.deepcopy(self.proof)
            p['guestLines'] = [l.replace(before, after) for l in p['guestLines']]
            with self.subTest(before=before), self.assertRaises(ValueError): self.check(p)
        for marker in ('nativeResourceFdImported', 'nativeResourceHandleClosed', 'nativeResourceFdOpenReturned'):
            p = copy.deepcopy(self.proof); p['records'] = [r for r in p['records'] if marker not in r]
            with self.subTest(marker=marker), self.assertRaises(ValueError): self.check(p)

    def test_native_retirement_and_broker_cleanup_are_required(self):
        for marker in ('nativeAsyncSubmissionRetired', 'nativeHwQueueSignalRetired', 'nativeGpuQueueWaitDependenciesSatisfied'):
            p = copy.deepcopy(self.proof)
            p['records'].remove(next(r for r in p['records'] if marker in r))
            with self.subTest(marker=marker), self.assertRaises(ValueError): self.check(p)
        for before, after in (('retainedViews=0', 'retainedViews=1'), ('reaped=2 closed=2 retained=0', 'reaped=2 closed=1 retained=1'),
                              ('effectiveCapabilities=0', 'effectiveCapabilities=1'), ('recoverableErrors=1', 'recoverableErrors=2')):
            p = copy.deepcopy(self.proof)
            p['guestLines'] = [l.replace(before, after) for l in p['guestLines']]
            with self.subTest(before=before), self.assertRaises(ValueError): self.check(p)


if __name__ == '__main__':
    unittest.main()
