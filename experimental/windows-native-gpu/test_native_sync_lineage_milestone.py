"""Corrupt real retained lineage/GPU captures; CI runs no vendor GPU workload."""
import copy
import json
from pathlib import Path
import unittest
from verify_native_sync_lineage_capture import validate_native_sync_lineage


class NativeSyncLineageMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/NATIVE-SYNC-LINEAGE-2026-10-13.json').read_text())
    def setUp(self): self.proof=copy.deepcopy(self.evidence['runs'][0]['proof'])
    def check(self,p=None):
        p=self.proof if p is None else p
        return validate_native_sync_lineage(p['report'],'\n'.join(p['guestLines']),p['records'])
    def change(self,before,after):
        lines=self.proof['guestLines']; self.assertTrue(any(before in l for l in lines))
        self.proof['guestLines']=[l.replace(before,after) for l in lines]
    def test_real_runs_keep_owned_import_scope_separate_from_desktop(self):
        self.assertEqual(len(self.evidence['runs']),2)
        for run in self.evidence['runs']:
            audit=self.check(run['proof']); self.assertEqual(audit,run['audit'])
            self.assertEqual(audit['nativeGpuSignalTarget'],4294967367)
            for key in ('syncFileImportVerified','nativeGpuWaitThroughSyncFileVerified','completedSyncFileNativeLifetimeVerified',
                        'zeroTargetSyncFileVerified','canceledSyncFileImportRejected'):
                self.assertIs(audit[key],True)
            for key in ('eglNativeFenceVerified','foreignSyncFileImportVerified','mergedSyncFileImportVerified',
                        'gbmVerified','dmaBufVerified','desktopVerified','desktopStabilityVerified','nearNativePerformanceVerified'):
                self.assertIs(audit[key],False)
    def test_disabled_feature_and_build_failure_stay_failures(self):
        self.assertTrue(self.evidence['enabledImageManifest']['nativeSyncFilesEnabled'])
        self.assertFalse(self.evidence['disabledImageManifest']['nativeSyncFilesEnabled'])
        self.assertEqual(len(self.evidence['buildFailures']),1)
        for run in self.evidence['failures']:
            with self.assertRaises(ValueError): self.check(run['proof'])
            self.assertIn('SYNC_LINEAGE_FAILURE stage=ForeignSyncFile errno=95',run['proof']['guestLines'])
            self.assertIn('KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=0 signaled=0 errors=0 released=0 cleanup=true',run['proof']['guestLines'])
    def test_missing_duplicate_or_reordered_lifecycle_cannot_pass(self):
        for prefix in ('SYNC_LINEAGE_BEGIN ','SYNC_LINEAGE_GPU ','SYNC_LINEAGE_LIFETIME ','SYNC_LINEAGE_REJECT ',
                       'SYNC_LINEAGE_COMPLETE ','SYNC_LINEAGE_CONTRACT_PASS ','SYNC_LINEAGE_CONTRACT_UNPRIVILEGED ',
                       'KERNEL_SYNC_FILE_EXIT '):
            for duplicate in (False,True):
                self.setUp(); lines=self.proof['guestLines']; line=next(l for l in lines if l.startswith(prefix))
                lines.append(line) if duplicate else lines.remove(line)
                with self.subTest(prefix=prefix,duplicate=duplicate),self.assertRaises(ValueError): self.check()
        self.setUp(); lines=self.proof['guestLines']; line=next(l for l in lines if l.startswith('SYNC_LINEAGE_LIFETIME '))
        lines.remove(line); lines.insert(0,line)
        with self.assertRaises(ValueError): self.check()
    def test_pending_ordering_and_native_gpu_receipts_are_required(self):
        for marker,key,value in (('nativeGpuQueueWaitAccepted','cpuWait',True),
                                 ('nativeGpuQueueWaitAccepted','firstTarget',71),
                                 ('nativeGpuQueueWaitAccepted','nativeGpuQueueWaitAccepted',1),
                                 ('nativeAsyncContextSignalRetired','observed',0),
                                 ('nativeAsyncContextSignalRetired','cpuValueWrittenByBridge',True),
                                 ('nativeHwQueueSignalAccepted','observed',1)):
            self.setUp()
            for record in self.proof['records']:
                if marker in record: record[key]=value
            for record in self.proof['report'].get('nativeHwQueueSignalAcceptances',[]):
                if marker in record: record[key]=value
            with self.subTest(marker=marker,key=key),self.assertRaises(ValueError): self.check()
    def test_native_capability_must_survive_completed_import(self):
        for marker,key,value in (('nativeSyncHandleExported','rawHandleExposed',True),
                                 ('nativeSyncHandleImported','gpuMapped',True),
                                 ('nativeSyncHandleImported','nativeSyncHandleImported',1)):
            self.setUp(); next(r for r in self.proof['records'] if marker in r)[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check()
        self.setUp(); records=self.proof['records']; close=records.pop(next(i for i,r in enumerate(records) if 'nativeSyncHandleClosed' in r))
        records.insert(0,close)
        with self.assertRaises(ValueError): self.check()
    def test_privileges_source_lifetime_errors_and_cleanup_are_required(self):
        for before,after in (('acknowledgementBefore=0','acknowledgementBefore=1'),('originalSourceReleased=true','originalSourceReleased=false'),
                             ('completedSyncFileRetainsNative=true','completedSyncFileRetainsNative=false'),
                             ('canceledImportErrno=19','canceledImportErrno=0'),('deniedResolve=true','deniedResolve=false'),
                             ('held=0 pending=0','held=1 pending=0'),('effectiveCapabilities=0','effectiveCapabilities=1')):
            self.setUp(); self.change(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
    def test_actual_shader_pixels_still_require_an_independent_audit(self):
        rows=self.proof['guestLines']; n=next(i for i,l in enumerate(rows) if l.startswith('OPENGL_PIXEL_ROW frame=1 y=0 '))
        prefix,rgba=rows[n].split('rgba='); rows[n]=prefix+'rgba=ff'+rgba[2:]
        with self.assertRaises(ValueError): self.check()

if __name__=='__main__': unittest.main()
