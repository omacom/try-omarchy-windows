"""Corrupt actual fence-sharing captures to reject false graphics milestones."""
import copy
import json
from pathlib import Path
import unittest
from verify_sync_fd_capture import validate_sync_fds, validate_native_sync


class SyncFdMilestone(unittest.TestCase):
    def setUp(self):
        self.evidence = json.loads((Path(__file__).resolve().parents[2] / 'docs/evidence/SYNC-FD-2026-10-12.json').read_text())
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None):
        p = proof or self.proof
        return validate_sync_fds(p['report'], '\n'.join(p['guestLines']), p['records'])

    def test_both_actual_guests_pass_with_limited_scope(self):
        for run in self.evidence['runs']:
            result = self.check(run['proof'])
            self.assertEqual(result, run['audit'])
            self.assertEqual((result['kernelObjectContractChecks'], result['kernelContractChecks']), (561,581))
            for key in ('guestGpuTimelineVerified', 'crossProcessNativeImportVerified', 'mesaSemaphoreFdVerified',
                        'syncFileVerified', 'desktopVerified', 'nearNativePerformanceVerified'):
                self.assertIs(result[key], False)

    def test_missing_duplicate_or_wrong_scope_receipts_rejected(self):
        for prefix in ('OBJECT_CONTRACT_PASS', 'OBJECT_CONTRACT_UNPRIVILEGED_CREATE',
                       'SYNC_FD_TEST_COMPLETE', 'KERNEL_OBJECT_EXIT', 'LINUX_BRIDGE nativeSyncFdImported='):
            for action in ('missing','duplicate'):
                with self.subTest(prefix=prefix, action=action):
                    p = copy.deepcopy(self.proof)
                    index = next(i for i,line in enumerate(p['guestLines']) if line.startswith(prefix))
                    line = p['guestLines'].pop(index)
                    if action == 'duplicate': p['guestLines'][index:index] = [line,line]
                    with self.assertRaises(ValueError): self.check(p)
        for before,after in (('gpuSignals=false','gpuSignals=true'), ('sourceDestroyedBeforeImport=true','sourceDestroyedBeforeImport=false'),
                             ('KERNEL_OBJECT_EXIT held=0','KERNEL_OBJECT_EXIT held=1')):
            p = copy.deepcopy(self.proof)
            p['guestLines'] = [line.replace(before,after) for line in p['guestLines']]
            with self.assertRaises(ValueError): self.check(p)

    def test_reap_identity_and_lifetime_order_rejected(self):
        p = self.proof
        indices = [i for i,line in enumerate(p['guestLines']) if line.startswith('LINUX_BRIDGE kernelObject')]
        for action in ('missing','forged','reordered'):
            with self.subTest(action=action):
                changed = copy.deepcopy(p)
                if action == 'missing': changed['guestLines'].pop(indices[1])
                if action == 'forged': changed['guestLines'][indices[1]] = 'LINUX_BRIDGE kernelObjectReaped=true ticket=1'
                if action == 'reordered':
                    changed['guestLines'][indices[0]],changed['guestLines'][indices[1]] = changed['guestLines'][indices[1]],changed['guestLines'][indices[0]]
                with self.assertRaises(ValueError): self.check(changed)

    def test_native_references_must_match_host_receipts(self):
        for marker in ('nativeSyncHandleExported','nativeSyncHandleImported','nativeSyncHandleClosed'):
            changed = copy.deepcopy(self.proof)
            changed['records'].pop(next(i for i,r in enumerate(changed['records']) if marker in r))
            with self.assertRaises(ValueError): self.check(changed)
        for value in (0,True):
            changed = copy.deepcopy(self.proof)
            record = next(r for r in changed['records'] if 'nativeSyncHandleExported' in r)
            record['rawHandleExposed'] = value
            with self.assertRaises(ValueError): self.check(changed)
        self.proof['report']['disconnectCleanup']['closedSyncHandles'] = 1
        with self.assertRaises(ValueError): self.check()

    def test_real_native_gpu_signal_runs_remain_separate(self):
        for run in self.evidence['nativeGpuRuns']:
            result = validate_native_sync('\n'.join(run['lines']))
            self.assertEqual(result, run['audit'])
            self.assertEqual(result['gpuSignals'],16)
            self.assertIs(result['guestGpuTimelineVerified'],False)

    def test_native_signal_corruption_missing_retirement_or_failed_cleanup_rejected(self):
        original = '\n'.join(self.evidence['nativeGpuRuns'][0]['lines'])
        for before,after in (('recipient=16','recipient=15'), ('sourceAlive=0','sourceAlive=1'),
                             ('kind=fence status=00000000','kind=fence status=c0000001'),
                             ('NATIVE_SYNC_SOURCE_RELEASED','MISSING_SOURCE_RELEASED'),
                             ('NATIVE_SYNC_GPU_RETIRED target=9 ','MISSING_RETIREMENT target=9 ')):
            with self.subTest(after=after):
                with self.assertRaises(ValueError): validate_native_sync(original.replace(before,after))


if __name__ == '__main__':
    unittest.main()
