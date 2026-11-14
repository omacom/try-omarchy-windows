"""Corrupt actual guest GPU captures to reject false ordering milestones."""
import copy
import json
from pathlib import Path
import unittest
from verify_gpu_timeline_capture import validate_gpu_timeline
from verify_sync_fd_capture import validate_sync_fds


class GpuTimelineMilestone(unittest.TestCase):
    def setUp(self):
        self.evidence = json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/GPU-TIMELINE-2026-10-12.json').read_text())
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None):
        p = self.proof if proof is None else proof
        return validate_gpu_timeline(p['report'],'\n'.join(p['guestLines']),p['records'])

    def test_both_actual_guests_prove_only_the_tested_scope(self):
        for run in self.evidence['runs']:
            result = self.check(run['proof'])
            self.assertEqual(result,run['audit'])
            self.assertEqual(result['totalRetiredGpuSignals'],8)
            for key in ('desktopVerified','nearNativePerformanceVerified','mesaSemaphoreFdVerified',
                        'syncFileVerified','gpuAccessibleSharedFenceVerified'):
                self.assertIs(result[key],False)

    def test_actual_opengl_regressions_keep_their_independent_scope(self):
        for run in self.evidence['openGlRegressionRuns']:
            p = run['proof']
            result = validate_sync_fds(p['report'],'\n'.join(p['guestLines']),p['records'])
            self.assertEqual(result,run['audit'])
            self.assertEqual(result['retiredNativeCommands'],18)
            self.assertIs(result['guestGpuTimelineVerified'],False)

    def test_missing_duplicate_and_failed_guest_receipts_rejected(self):
        for prefix in ('GPU_TIMELINE_SOURCE_RELEASED','GPU_TIMELINE_GATED','GPU_TIMELINE_ORDERING_VERIFIED',
                       'GPU_COPY_TEST_COMPLETE','KERNEL_OBJECT_EXIT','KERNEL_APERTURE_EXIT','BRIDGE_RUNTIME_EXIT='):
            for duplicate in (False,True):
                p = copy.deepcopy(self.proof)
                i = next(i for i,line in enumerate(p['guestLines']) if line.startswith(prefix))
                line = p['guestLines'].pop(i)
                if duplicate: p['guestLines'][i:i] = [line,line]
                with self.subTest(prefix=prefix,duplicate=duplicate),self.assertRaises(ValueError): self.check(p)
        for before,after in (('cpuFenceWrites=false','cpuFenceWrites=true'),('acknowledgement=0','acknowledgement=1'),
                             ('stage=FutureQueueWait status=00000000','stage=FutureQueueWait status=80004001'),
                             ('original=true exportedFd=true','original=false exportedFd=false'),
                             ('flags=1155 affinity=0','flags=128 affinity=0')):
            p = copy.deepcopy(self.proof)
            p['guestLines'] = [line.replace(before,after) for line in p['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check(p)

    def test_wait_return_and_second_queue_release_must_have_correct_order(self):
        p = self.proof
        a = next(i for i,line in enumerate(p['guestLines']) if line.startswith('GPU_TIMELINE_GATED '))
        b = next(i for i,line in enumerate(p['guestLines']) if 'stage=ReleaseFromSecondQueue ' in line)
        p['guestLines'][a],p['guestLines'][b] = p['guestLines'][b],p['guestLines'][a]
        with self.assertRaises(ValueError): self.check()

    def test_native_ownership_type_query_and_lifetime_must_be_real(self):
        for marker,key,value in (('nativeSyncResourceInfoQueried','ntstatus',-1073741811),
                                 ('nativeSyncHandleExported','rawHandleExposed',0),
                                 ('nativeSyncHandleImported','gpuMapped',True)):
            p = copy.deepcopy(self.proof)
            next(r for r in p['records'] if marker in r)[key] = value
            with self.subTest(marker=marker),self.assertRaises(ValueError): self.check(p)
        p = copy.deepcopy(self.proof)
        close = p['records'].pop(next(i for i,r in enumerate(p['records']) if 'nativeSyncHandleClosed' in r))
        p['records'].append(close)
        with self.assertRaises(ValueError): self.check(p)

    def test_native_wait_release_and_signal_completion_cannot_be_faked(self):
        for marker in ('nativeGpuQueueWaitAccepted','nativeGpuQueueWaitDependenciesSatisfied',
                       'nativeHwQueueSignalAccepted','nativeHwQueueSignalRetired','nativeAsyncContextSignalRetired',
                       'nativeAsyncSubmissionRetired'):
            p = copy.deepcopy(self.proof)
            p['records'].pop(next(i for i,r in enumerate(p['records']) if marker in r))
            with self.subTest(marker=marker),self.assertRaises(ValueError): self.check(p)
        for marker,key,value in (('nativeGpuQueueWaitAccepted','cpuWait',True),
                                 ('nativeAsyncContextSignalRetired','cpuValueWrittenByBridge',True),
                                 ('nativeHwQueueSignalRetired','observed',4),
                                 ('nativeAsyncSubmissionRetired','observed',0)):
            p = copy.deepcopy(self.proof)
            next(r for r in p['records'] if marker in r)[key] = value
            with self.subTest(marker=marker,key=key),self.assertRaises(ValueError): self.check(p)
        p = copy.deepcopy(self.proof)
        accepted = [r for r in p['records'] if 'nativeAsyncContextSignalAccepted' in r]
        accepted[4]['observed'] = 1
        with self.assertRaises(ValueError): self.check(p)

    def test_copy_readback_must_match_independent_pattern(self):
        p = self.proof
        p['guestLines'] = [line.replace('observedHash=28e3f2dd58641325','observedHash=0000000000000000') for line in p['guestLines']]
        with self.assertRaises(ValueError): self.check()

    def test_pending_work_failures_and_forced_teardown_rejected(self):
        for key,value in (('pendingGpuQueueWaits',1),('pendingAsyncContextSignals',1),
                          ('failedNativeContextSignals',1),('retiredAsyncContextSignals',6)):
            p = copy.deepcopy(self.proof)
            p['report']['disconnectCleanup'][key] = value
            next(r for r in p['records'] if 'driverCleanupVerified' in r)[key] = value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check(p)
        p = copy.deepcopy(self.proof)
        p['report']['ownedQemuControl']['qemuForcedStop'] = True
        next(r for r in p['records'] if 'ownedQemuExited' in r)['qemuForcedStop'] = True
        with self.assertRaises(ValueError): self.check(p)

    def test_wrong_runtime_or_fixture_scope_rejected(self):
        for key,value in (('liveNvidiaLinuxUmdLoaded',1),('realWslDxgForwarding',True),
                          ('capturedPrivateFixturesUsed',True),('d3d12DeviceHresult','80004005')):
            p = copy.deepcopy(self.proof)
            p['report'][key] = value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check(p)


if __name__ == '__main__':
    unittest.main()
