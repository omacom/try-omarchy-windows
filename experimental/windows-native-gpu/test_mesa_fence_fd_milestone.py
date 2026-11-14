"""Corrupt actual Mesa captures to reject false FD/queue-ordering milestones."""
import copy
import json
from pathlib import Path
import unittest
from verify_mesa_fence_fd_capture import validate_mesa_fence_fds, validate_disabled_mesa
from verify_sync_fd_capture import validate_sync_fds


class MesaFenceFdMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/MESA-FENCE-FD-2026-10-12.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None, validator=validate_mesa_fence_fds):
        p = self.proof if proof is None else proof
        return validator(p['report'], '\n'.join(p['guestLines']), p['records'])

    def test_actual_guests_prove_only_the_tested_scope(self):
        for run in self.evidence['runs']:
            result = self.check(run['proof'])
            self.assertEqual(result, run['audit'])
            self.assertTrue(result['mesaFutureGpuQueueOrderingVerified'])
            self.assertEqual(result['retiredNativeCommands'], 19)
            for key in ('desktopVerified', 'nearNativePerformanceVerified', 'openGlConformanceVerified',
                        'openGl4kVerified', 'syncFileVerified', 'binarySemaphoreInteroperabilityVerified',
                        'compositorSynchronizationVerified', 'dmaBufVerified', 'drmDeviceVerified'):
                self.assertIs(result[key], False)

    def test_actual_disabled_and_legacy_runs_keep_their_scope(self):
        for name, validator in (('disabledMesaRun', validate_disabled_mesa), ('legacyOpenGlFdRun', validate_sync_fds)):
            run = self.evidence[name]
            self.assertEqual(self.check(run['proof'], validator), run['audit'])
        self.assertEqual(self.evidence['knownFailure']['guestExit'], 128)
        self.assertNotIn(self.evidence['knownFailure']['label'], [r['label'] for r in self.evidence['runs']])

    def test_missing_duplicate_and_failed_gl_receipts_rejected(self):
        for prefix in ('MESA_FENCE_FD_BEGIN', 'MESA_FENCE_FD_IMPORT', 'MESA_FENCE_FD_REJECT kind=binary',
                       'MESA_FENCE_FD_SIGNAL', 'MESA_FENCE_FD_WAIT', 'MESA_FENCE_FD_RELEASE',
                       'MESA_FENCE_FD_COMPLETE', 'KERNEL_OBJECT_EXIT', 'BRIDGE_RUNTIME_EXIT='):
            for duplicate in (False, True):
                p = copy.deepcopy(self.proof)
                index = next(i for i, line in enumerate(p['guestLines']) if line.startswith(prefix))
                line = p['guestLines'].pop(index)
                if duplicate:
                    p['guestLines'][index:index] = [line, line]
                with self.subTest(prefix=prefix, duplicate=duplicate), self.assertRaises(ValueError):
                    self.check(p)
        for before, after in (('fdRetained=true', 'fdRetained=false'), ('fdOwnership=GL', 'fdOwnership=caller'),
                              ('observedBeforeRelease=1', 'observedBeforeRelease=3'),
                              ('dependentObserved=3', 'dependentObserved=2'),
                              ('type=COPY status=00000000', 'type=DIRECT status=80004005')):
            p = copy.deepcopy(self.proof)
            p['guestLines'] = [line.replace(before, after) for line in p['guestLines']]
            with self.subTest(after=after), self.assertRaises(ValueError):
                self.check(p)

    def test_native_ownership_type_query_and_lifetime_required(self):
        for marker, key, value in (('nativeSyncHandleExported', 'rawHandleExposed', True),
                                  ('nativeSyncResourceInfoQueried', 'ntstatus', 0),
                                  ('nativeSyncHandleImported', 'gpuMapped', True)):
            p = copy.deepcopy(self.proof)
            next(r for r in p['records'] if marker in r)[key] = value
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                self.check(p)
        p = copy.deepcopy(self.proof)
        closing = p['records'].pop(next(i for i, r in enumerate(p['records']) if 'nativeSyncHandleClosed' in r))
        p['records'].append(closing)
        with self.assertRaises(ValueError):
            self.check(p)

    def test_wait_and_signal_must_be_accepted_before_external_release(self):
        for marker in ('nativeGpuQueueWaitAccepted', 'nativeHwQueueSignalAccepted'):
            p = copy.deepcopy(self.proof)
            index = next(i for i, r in enumerate(p['records']) if marker in r and r['index'] == (3 if 'Wait' in marker else 2))
            record = p['records'].pop(index)
            release = next(i for i, r in enumerate(p['records']) if 'nativeAsyncContextSignalAccepted' in r)
            p['records'].insert(release+1, record)
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                self.check(p)

    def test_native_wait_and_signal_completion_cannot_be_faked(self):
        for marker in ('nativeGpuQueueWaitDependenciesSatisfied', 'nativeHwQueueSignalRetired',
                       'nativeAsyncContextSignalRetired', 'nativeAsyncSubmissionRetired'):
            p = copy.deepcopy(self.proof)
            p['records'].pop(next(i for i, r in enumerate(p['records']) if marker in r))
            with self.subTest(marker=marker), self.assertRaises(ValueError):
                self.check(p)
        for marker, key, value in (('nativeGpuQueueWaitAccepted', 'cpuWait', True),
                                  ('nativeHwQueueSignalAccepted', 'observed', 3),
                                  ('nativeHwQueueSignalRetired', 'cpuValueWrittenByBridge', True),
                                  ('nativeAsyncContextSignalRetired', 'observed', 1),
                                  ('nativeGpuQueueWaitDependenciesSatisfied', 'queueRetirementVerified', True)):
            p = copy.deepcopy(self.proof)
            next(r for r in p['records'] if marker in r)[key] = value
            with self.subTest(marker=marker, key=key), self.assertRaises(ValueError):
                self.check(p)

    def test_pending_work_failures_and_forced_teardown_rejected(self):
        for key, value in (('pendingGpuQueueWaits', 1), ('pendingAsyncContextSignals', 1),
                           ('failedNativeContextSignals', 1), ('vendorCpuSlotQuotaRejections', 1),
                           ('acceptedHwQueueSignals', 3)):
            p = copy.deepcopy(self.proof)
            p['report']['disconnectCleanup'][key] = value
            next(r for r in p['records'] if 'driverCleanupVerified' in r)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check(p)
        p = copy.deepcopy(self.proof)
        p['report']['ownedQemuControl']['qemuForcedStop'] = True
        next(r for r in p['records'] if 'ownedQemuExited' in r)['qemuForcedStop'] = True
        with self.assertRaises(ValueError):
            self.check(p)

    def test_wrong_profile_renderer_or_pixel_data_rejected(self):
        for key, value in (('version', 1), ('objectLimit', 385), ('hwQueueLimit', 17), ('vendorObjectLimit', 193)):
            p = copy.deepcopy(self.proof)
            next(r for r in p['records'] if 'openGlProfileNegotiated' in r)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check(p)
        p = copy.deepcopy(self.proof)
        index = next(i for i, line in enumerate(p['guestLines']) if line.startswith('OPENGL_PIXEL_ROW frame=1 y=0 '))
        line = p['guestLines'][index]
        p['guestLines'][index] = line[:-2] + '00'
        with self.assertRaises(ValueError):
            self.check(p)

    def test_disabled_mode_cannot_advertise_experimental_extensions(self):
        for extension in ('GL_EXT_semaphore', 'GL_EXT_semaphore_fd', 'GL_NV_timeline_semaphore'):
            p = copy.deepcopy(self.evidence['disabledMesaRun']['proof'])
            p['guestLines'].append('OPENGL_INTEROP_EXTENSION ' + extension)
            with self.subTest(extension=extension), self.assertRaises(ValueError):
                self.check(p, validate_disabled_mesa)


if __name__ == '__main__':
    unittest.main()
