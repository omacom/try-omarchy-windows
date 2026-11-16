"""Corrupt retained GPU sync-file captures; CI executes no GPU workload."""
import copy
import json
from pathlib import Path
import unittest
from verify_native_sync_file_capture import validate_native_sync_files
from verify_kernel_aperture_capture import validate_kernel_aperture


class NativeSyncFileMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/NATIVE-SYNC-FILE-2026-10-13.json').read_text())
    def setUp(self): self.proof=copy.deepcopy(self.evidence['runs'][0]['proof'])
    def check(self,p=None):
        p=self.proof if p is None else p
        return validate_native_sync_files(p['report'],'\n'.join(p['guestLines']),p['records'])
    def change(self,before,after):
        lines=self.proof['guestLines']; self.assertTrue(any(before in l for l in lines))
        self.proof['guestLines']=[l.replace(before,after) for l in lines]
    def test_real_gpu_runs_keep_export_scope_and_pixels_separate_from_desktop(self):
        self.assertEqual(len(self.evidence['runs']),2)
        for run in self.evidence['runs']:
            audit=self.check(run['proof']); self.assertEqual(audit,run['audit'])
            self.assertTrue(audit['nativeGpuSyncFileExportVerified']); self.assertEqual(audit['nativeGpuSignalTarget'],4294967354)
            for key in ('eglNativeFenceVerified','syncFileImportVerified','dmaBufVerified','gbmVerified','desktopVerified',
                        'desktopStabilityVerified','nearNativePerformanceVerified','multiClientGpuRoutingVerified',
                        'requiredApiMatrixVerified','allNvidiaFeaturesVerified','concurrentWindowsGuestWorkloadsVerified'):
                self.assertIs(audit[key],False)
    def test_actual_failures_including_disabled_feature_stay_rejected(self):
        self.assertFalse(self.evidence['disabledImageManifest']['nativeSyncFilesEnabled'])
        self.assertTrue(self.evidence['enabledImageManifest']['nativeSyncFilesEnabled'])
        for run in self.evidence['failures']:
            with self.subTest(run=run['label']),self.assertRaises(ValueError): self.check(run['proof'])
    def test_lifecycle_cannot_be_missing_duplicated_or_reordered(self):
        for prefix in ('NATIVE_SYNC_FILE_BEGIN ','NATIVE_SYNC_FILE_INFO ','NATIVE_SYNC_FILE_GPU ',
                       'NATIVE_SYNC_FILE_REVOKE ','NATIVE_SYNC_FILE_COMPLETE ','KERNEL_SYNC_FILE_EXIT '):
            for duplicate in (False,True):
                self.setUp(); lines=self.proof['guestLines']; line=next(l for l in lines if l.startswith(prefix))
                lines.append(line) if duplicate else lines.remove(line)
                with self.subTest(prefix=prefix,duplicate=duplicate),self.assertRaises(ValueError): self.check()
        self.setUp(); lines=self.proof['guestLines']; line=next(l for l in lines if l.startswith('NATIVE_SYNC_FILE_GPU '))
        lines.remove(line); lines.insert(0,line)
        with self.assertRaises(ValueError): self.check()
    def test_cpu_written_wrong_target_and_unretired_gpu_signals_cannot_pass(self):
        for marker,key,value in (('nativeHwQueueSignalAccepted','cpuValueWrittenByBridge',True),
                                 ('nativeHwQueueSignalAccepted','noGpuAccess',False),
                                 ('nativeHwQueueSignalRetired','target',58),('nativeHwQueueSignalRetired','observed',57),
                                 ('nativeHwQueueSignalRetired','index',2)):
            self.setUp()
            for record in self.proof['records']:
                if marker in record: record[key]=value
            for record in self.proof['report']['nativeHwQueueSignalAcceptances' if marker.endswith('Accepted') else 'nativeHwQueueSignalRetirements']:
                record[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check()
    def test_privileges_error_results_and_retained_kernel_objects_are_required(self):
        for before,after in (('recipientUid=65534','recipientUid=0'),('pendingBeforeRelease=true','pendingBeforeRelease=false'),
                             ('parentFdClosed=true','parentFdClosed=false'),('status=-19','status=1'),
                             ('timeoutStatus=-110','timeoutStatus=1'),('held=0 pending=0','held=1 pending=0'),
                             ('effectiveCapabilities=0','effectiveCapabilities=1'),('nativePortOpen=denied','nativePortOpen=allowed'),
                             ('SYNC_FILE_CONTRACT_UNPRIVILEGED_CREATE denied=true','SYNC_FILE_CONTRACT_UNPRIVILEGED_CREATE denied=false')):
            self.setUp(); self.change(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
    def test_extra_kernel_checks_require_the_explicit_profile(self):
        p=self.proof
        with self.assertRaises(ValueError): validate_kernel_aperture(p['report'],'\n'.join(p['guestLines']),p['records'],transport='authenticated-broker')
        for value in (True,0,582,584):
            with self.subTest(value=value),self.assertRaises(ValueError):
                validate_kernel_aperture(p['report'],'\n'.join(p['guestLines']),p['records'],contract_checks=value)
        self.change('APERTURE_CONTRACT_PASS checks=583','APERTURE_CONTRACT_PASS checks=581')
        with self.assertRaises(ValueError): self.check()

if __name__=='__main__': unittest.main()
