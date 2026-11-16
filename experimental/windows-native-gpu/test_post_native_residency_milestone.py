"""Replay hardware rollback, all pixels, ordinary behavior and retained rejects."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
import test_multiapp_cpu384_milestone as decoder
from verify_post_native_residency_recovery import (rejection_metadata,validate_post_native_residency_recovery,
    validate_ordinary_residency_backend,validate_nonaccepted_ordinary_coverage,validate_nonaccepted_cursor_observation,validate_nonaccepted_device_loss)


class PostNativeResidency(unittest.TestCase):
    blob=decoder.MultiappCpu384.__dict__['blob']
    row=decoder.MultiappCpu384.__dict__['row']
    inputs=decoder.MultiappCpu384.__dict__['inputs']

    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-RESIDENCY-ROLLBACK-OBSERVATIONS-2026-10-24.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='88ff20413a4d9ef086dfb4d8388eb440ab05ad8788392a94e5499808b9038bef'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    def test_01_single_allocation_observation_keeps_full_pixels_and_live_cleanup(self):
        for number in (165,):
            proof=validate_post_native_residency_recovery(**self.inputs(number,True))
            self.assertEqual(proof,self.row(number)['proof'])
            self.assertEqual(proof['frameCount'],1800)
            self.assertEqual([f['maximumChannelError'] for f in proof['frames']],[1,1])
            self.assertEqual(proof['rejectedResidency']['rolledBackNewReferences'],1)
            self.assertGreaterEqual(proof['rejectedResidency']['subsequentGpuHandoffs'],1600)
            self.assertFalse(proof['actualNativeQuotaExhaustionVerified'])
            self.assertFalse(proof['memoryPressureRecoveryVerified'])
            self.assertFalse(proof['fullDesktopAccepted'])

    def test_02_ordinary_build_keeps_full_original_regression(self):
        proof=validate_ordinary_residency_backend(**self.inputs(168,True))
        self.assertEqual(proof,self.row(168)['proof'])
        self.assertTrue(proof['ordinaryResidencyBackendCompatibilityVerified'])
        self.assertGreater(proof['sharedBudget']['reusedReferences'],0)
        self.assertFalse(proof['diagnosticRejectionEnabled'])

    def test_03_missing_reuse_coverage_stays_nonaccepted(self):
        data=self.inputs(166,True)
        proof=validate_nonaccepted_ordinary_coverage(**data)
        self.assertEqual(proof,self.row(166)['proof'])
        self.assertFalse(proof['compatibilityAccepted'])
        self.assertFalse(proof['residencyReferenceReuseExercised'])
        with self.assertRaises(ValueError):validate_ordinary_residency_backend(**data)

    def test_04_actual_cursor_interference_keeps_wallpaper_rejection(self):
        data=self.inputs(167,True);proof=validate_nonaccepted_cursor_observation(**data)
        self.assertEqual(proof,self.row(167)['proof'])
        self.assertTrue(proof['originalWallpaperGuardRejected'])
        self.assertEqual(proof['rejectedProbe'],[1056,960])
        self.assertFalse(proof['controlledRecoveryAccepted'])

    def test_05_policy_refusal_cannot_be_relabelled_native_quota_exhaustion(self):
        data=copy.deepcopy(self.inputs(165));row=next(r for r in data['records'] if r.get('nativeDiagnosticResidencyRejection'))
        row['nativeQuotaExceeded']=True
        with self.assertRaises(ValueError):rejection_metadata(data['report'],data['guest'],data['records'])

    def test_06_failed_or_incomplete_native_rollback_cannot_pass(self):
        for field,value in (('rollbackNtstatus',-1),('rolledBackReferences',0),('rollbackVerified',False),('rollbackNativeCall',False)):
            data=copy.deepcopy(self.inputs(165));row=next(r for r in data['records'] if r.get('nativeDeferredResidencyBudgetRejected'))
            row[field]=value
            with self.assertRaises(ValueError):rejection_metadata(data['report'],data['guest'],data['records'])

    def test_07_releasing_prior_reused_ownership_cannot_pass(self):
        data=copy.deepcopy(self.inputs(165));row=next(r for r in data['records'] if r.get('nativeDeferredResidencyBudgetRejected'))
        row['reusedReferencesReleased']=True
        with self.assertRaises(ValueError):rejection_metadata(data['report'],data['guest'],data['records'])

    def test_08_extra_or_false_rejection_receipts_cannot_hide(self):
        data=self.inputs(165)
        with self.assertRaises(ValueError):rejection_metadata(data['report'],data['guest'],data['records']+[dict(nativeDeferredResidencyBudgetRejected=False)])

    def test_09_device_loss_paging_sentinel_cannot_pass(self):
        data=copy.deepcopy(self.inputs(165));row=next(r for r in data['records'] if r.get('nativeDiagnosticResidencyRejection'))
        row['pagingTarget']=2**64-1
        with self.assertRaises(ValueError):rejection_metadata(data['report'],data['guest'],data['records'])

    def test_10_lost_subsequent_gpu_handoff_cannot_pass(self):
        data=self.inputs(165)
        records=[r for r in data['records'] if not (r.get('nativeCompositorRingInput') and r['frame']==700)]
        with self.assertRaises(ValueError):rejection_metadata(data['report'],data['guest'],records)

    def test_11_other_native_failures_cannot_be_excused(self):
        data=copy.deepcopy(self.inputs(165,True));data['report']['disconnectCleanup']['failedHwQueues']=1
        with self.assertRaises(ValueError):validate_post_native_residency_recovery(**data)

    def test_12_corrupt_post_refusal_sdl_pixels_cannot_pass(self):
        data=copy.deepcopy(self.inputs(165,True));name='relay-window-0000000000001800.rgba'
        raw=bytearray(data['frames'][name]);raw[0]^=255;data['frames'][name]=bytes(raw)
        with self.assertRaises(ValueError):validate_post_native_residency_recovery(**data)

    def test_13_cleanup_deferred_until_vm_exit_cannot_pass(self):
        data=copy.deepcopy(self.inputs(165,True));data['report']['ownedQemuControl']['allocationMappingsReleasedAfterVmExit']=1
        with self.assertRaises(ValueError):validate_post_native_residency_recovery(**data)

    def test_14_diagnostic_build_cannot_pass_as_ordinary(self):
        with self.assertRaises(ValueError):validate_ordinary_residency_backend(**self.inputs(165,True))

    def test_16_four_allocation_device_loss_rejects_historical_candidate(self):
        row=self.row(169)
        self.assertFalse(row['compatibilityAccepted'])
        report=json.loads(self.blob(row['files']['driver.json']))
        source=json.loads(self.blob(row['files']['source.json']))
        raw=self.blob(row['files']['driver.errors.log']).decode('utf-8').splitlines()
        self.assertEqual([line for line in raw if not line.startswith('{')],['FAIL: Coordinated driver object cleanup failed'])
        records=[json.loads(line) for line in raw if line.startswith('{')]
        proof=validate_nonaccepted_device_loss(report,records,source)
        archived=dict(row['proof']);archived.pop('rawArtifactSha256')
        self.assertEqual(proof,archived)
        self.assertFalse(proof['accepted'])
        self.assertFalse(proof['historicalRollbackImplementationSafe'])
        self.assertNotIn('relay-window-0000000000001800.rgba',row['frames'])
        self.assertEqual(self.value['boundedSingleAllocationRecoveryObservations'],[165])
        self.assertFalse(self.value['historicalRollbackImplementationSafe'])
        for field,value in [('driverCleanupVerified',True),('completedCompositorFrames',1800)]:
            altered=copy.deepcopy(report);altered['disconnectCleanup'][field]=value
            with self.assertRaises(ValueError):validate_nonaccepted_device_loss(altered,records,source)
        altered=copy.deepcopy(records)
        next(r for r in altered if r.get('nativeDiagnosticResidencyRejection'))['nativeCount']=1
        with self.assertRaises(ValueError):validate_nonaccepted_device_loss(report,altered,source)

    def test_15_same_sources_controls_and_binary_origin_remain_auditable(self):
        for digest in self.value['blobs']:self.blob(digest)
        builds=self.value['nativeBuilds'];a=json.loads(self.blob(builds['89']['receipt']));b=json.loads(self.blob(builds['90']['receipt']))
        self.assertEqual(a['sources'],b['sources']);self.assertEqual(a['buildScriptSha256'],b['buildScriptSha256'])
        self.assertFalse(a['binaryContainsInjectionMarker']);self.assertTrue(b['binaryContainsInjectionMarker'])
        for key,item in builds.items():
            receipt=json.loads(self.blob(item['receipt']))
            for name,digest in receipt['sources'].items():
                self.assertEqual(digest,item['sources'][name]);self.blob(digest)
                self.assertFalse(name.endswith(('.exe','.so','.ko','.ext4','.cpio.gz')))
        controls=json.loads(self.blob(self.value['controls']['residency-rejection-controls-v1/receipt.json']))
        self.assertEqual(len(controls['cases']),6)
        self.assertEqual([r['expectedCompileSuccess'] for r in controls['cases']],[True,True,True,False,False,False])
        preflight=json.loads(self.blob(self.value['controls']['residency-build-preflight-v1/receipt.json']))
        self.assertEqual(preflight['buildScriptSha256'],a['buildScriptSha256'])
        self.assertTrue(all(r['exit']!=0 and not r['outputDirectoryCreated'] and not r['gpu'] for r in preflight['cases']))
        for name,digest in self.value['auditSources'].items():
            self.assertEqual(hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest(),digest)


if __name__=='__main__':unittest.main()
