"""Corrupt actual GPU captures to reject false process-exit lifetime claims."""
import copy
import json
from pathlib import Path
import unittest
from verify_exit_lifetime_capture import (
    ARMED, BEGIN, COMPLETE, validate_exit_lifetime, validate_exit_lifetime_control)
from verify_mesa_fence_fd_capture import validate_mesa_fence_fds
from verify_opengl_capture import validate_opengl


class ExitLifetimeMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((Path(__file__).resolve().parents[2]/
            'docs/evidence/EXIT-LIFETIME-2026-10-12.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None, validator=validate_exit_lifetime):
        p = self.proof if proof is None else proof
        return validator(p['report'], '\n'.join(p['guestLines']), p['records'])

    def test_three_actual_guests_prove_only_process_exit_lifetime(self):
        for run in self.evidence['runs']:
            result = self.check(run['proof'])
            self.assertEqual(result, run['audit'])
            self.assertTrue(result['lateNativeFenceFinalizerVerified'])
            self.assertEqual(result['nativeFenceValue'], 57)
            for key in ('desktopVerified', 'nearNativePerformanceVerified', 'openGlConformanceVerified',
                        'openGl4kVerified', 'generalDesktopStabilityVerified', 'drmDeviceVerified', 'dmaBufVerified'):
                self.assertIs(result[key], False)

    def test_real_crash_control_is_never_a_normal_success(self):
        control = self.evidence['expectedFailureControl']
        result = self.check(control['proof'], validate_exit_lifetime_control)
        self.assertEqual(result, control['audit'])
        self.assertEqual((result['signal'], result['guestExit']), (11, 128))
        self.assertIs(result['processExitMappingLifetimeVerified'], False)
        for validator in (validate_exit_lifetime, validate_opengl):
            with self.subTest(validator=validator.__name__), self.assertRaises(ValueError):
                self.check(control['proof'], validator)
        with self.assertRaises(ValueError):
            self.check(validator=validate_exit_lifetime_control)

    def test_existing_mesa_gpu_ordering_survives_the_fix(self):
        for run in self.evidence['mesaRegressions']:
            self.assertEqual(self.check(run['proof'], validate_mesa_fence_fds), run['audit'])
            self.assertTrue(run['audit']['mesaFutureGpuQueueOrderingVerified'])
        self.assertEqual(self.evidence['separateLegacyCapacityFailure']['guestExit'], 1)
        self.assertEqual(self.evidence['separateLegacyCapacityFailure']['quotaReceipt']['objectLimit'], 256)
        self.assertIn('does not conclusively', self.evidence['earlierIntermittentFailure']['attribution'])

    def test_missing_duplicate_and_reordered_finalizer_receipts_rejected(self):
        for prefix in (ARMED, BEGIN, COMPLETE, 'LINUX_BRIDGE summary ',
                       'LINUX_BRIDGE processExitViewsRetained=', 'KERNEL_OBJECT_EXIT'):
            for duplicate in (False, True):
                p = copy.deepcopy(self.proof)
                index = next(i for i, line in enumerate(p['guestLines']) if line.startswith(prefix))
                line = p['guestLines'].pop(index)
                if duplicate:
                    p['guestLines'][index:index] = [line, line]
                with self.subTest(prefix=prefix, duplicate=duplicate), self.assertRaises(ValueError):
                    self.check(p)
        p = copy.deepcopy(self.proof)
        p['guestLines'].remove(COMPLETE)
        p['guestLines'].insert(p['guestLines'].index(BEGIN), COMPLETE)
        with self.assertRaises(ValueError):
            self.check(p)

    def test_retained_counts_must_match_native_post_vm_cleanup(self):
        for before, after in (('syncFences=44', 'syncFences=43'),
                              ('allocationViews=46', 'allocationViews=45'),
                              ('release=kernel-process-exit', 'release=atexit'),
                              ('observed=57 mappingLive=true', 'observed=0 mappingLive=true')):
            p = copy.deepcopy(self.proof)
            p['guestLines'] = [line.replace(before, after) for line in p['guestLines']]
            with self.subTest(after=after), self.assertRaises(ValueError):
                self.check(p)

    def test_signals_and_wrong_failure_location_rejected(self):
        p = copy.deepcopy(self.proof)
        p['guestLines'].append('BRIDGE_RUNTIME_SIGNAL number=11 coreDumped=false')
        with self.assertRaises(ValueError):
            self.check(p)
        for before, after in (('number=11', 'number=9'), ('BRIDGE_RUNTIME_EXIT=128', 'BRIDGE_RUNTIME_EXIT=0')):
            p = copy.deepcopy(self.evidence['expectedFailureControl']['proof'])
            p['guestLines'] = [line.replace(before, after) for line in p['guestLines']]
            with self.subTest(after=after), self.assertRaises(ValueError):
                self.check(p, validate_exit_lifetime_control)
        p = copy.deepcopy(self.evidence['expectedFailureControl']['proof'])
        signal = next(line for line in p['guestLines'] if line.startswith('BRIDGE_RUNTIME_SIGNAL'))
        p['guestLines'].remove(signal)
        p['guestLines'].insert(p['guestLines'].index(BEGIN), signal)
        with self.assertRaises(ValueError):
            self.check(p, validate_exit_lifetime_control)

    def test_native_fence_provenance_and_cleanup_required(self):
        p = copy.deepcopy(self.proof)
        for record in p['records']:
            if record.get('nativeSynchronizationCreated') and record.get('flags') == 128:
                record['flags'] = 129
        with self.assertRaises(ValueError):
            self.check(p)
        for key, value in (('liveNvidiaLinuxUmdLoaded', False), ('capturedPrivateFixturesUsed', True)):
            p = copy.deepcopy(self.proof)
            p['report'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check(p)
        for section, marker, key, value in (
                ('disconnectCleanup', 'driverCleanupVerified', 'failedNativeSubmissions', 1),
                ('ownedQemuControl', 'ownedQemuExited', 'qemuForcedStop', True)):
            p = copy.deepcopy(self.proof)
            p['report'][section][key] = value
            next(r for r in p['records'] if marker in r)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check(p)

    def test_positive_and_negative_controls_require_real_pixels(self):
        for original, validator in ((self.proof, validate_exit_lifetime),
                (self.evidence['expectedFailureControl']['proof'], validate_exit_lifetime_control)):
            p = copy.deepcopy(original)
            index = next(i for i, line in enumerate(p['guestLines']) if line.startswith('OPENGL_PIXEL_ROW frame=1 y=0 '))
            p['guestLines'][index] = p['guestLines'][index][:-2] + '00'
            with self.subTest(validator=validator.__name__), self.assertRaises(ValueError):
                self.check(p, validator)


if __name__ == '__main__':
    unittest.main()
