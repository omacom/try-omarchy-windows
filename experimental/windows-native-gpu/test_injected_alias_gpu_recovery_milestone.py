"""Retain full GPU recovery checks, nonaccepted observations and origin limits."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
import test_multiapp_cpu384_milestone as decoder
from verify_injected_alias_gpu_recovery import (injection_metadata,validate_injected_alias_gpu_recovery,
    validate_nonaccepted_injection,validate_probe_controls)
from verify_multiapp_desktop import validate_multiapp_desktop


class InjectedAliasGpuRecovery(unittest.TestCase):
    blob=decoder.MultiappCpu384.__dict__['blob']
    row=decoder.MultiappCpu384.__dict__['row']
    inputs=decoder.MultiappCpu384.__dict__['inputs']

    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-INJECTED-ALIAS-GPU-RECOVERY-2026-10-23.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='d54bc56bdce0f0be8f11fa2550b255758c7eacb404adcd9551062cfdf1b85bdf'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    def test_accepted_repeats_keep_complete_pixels_fences_input_and_cleanup(self):
        self.assertEqual(self.value['controlledRecoveryRuns'],[152,154])
        for number in (152,154):
            proof=validate_injected_alias_gpu_recovery(**self.inputs(number,True))
            self.assertEqual(proof,self.row(number)['proof'])
            self.assertEqual(proof['frameCount'],1800)
            self.assertEqual([f['maximumChannelError'] for f in proof['frames']],[1,1])
            self.assertTrue(proof['injectedRefusal']['sameResourceRetryVerified'])
            self.assertGreaterEqual(proof['injectedRefusal']['subsequentGpuHandoffs'],1600)
            self.assertFalse(proof['actualNativeQuotaRefusalRecoveryVerified'])
            self.assertFalse(proof['intermittentAliasLimitResolved'])
            self.assertFalse(proof['fullDesktopAccepted'])

    def test_cpu_texture_upload_after_refusal_cannot_pass(self):
        data=copy.deepcopy(self.inputs(154,True));frame=self.row(154)['proof']['injectedRefusal']['injectedAfterAcceptedFrame']+1
        next(r for r in data['records'] if r.get('nativeSdlCompositorQueued') and r['frame']==frame)['qemuState']['cpuTextureUpload']=True
        with self.assertRaises(ValueError):validate_injected_alias_gpu_recovery(**data)

    def test_deferred_vm_exit_cleanup_cannot_pass(self):
        data=copy.deepcopy(self.inputs(154,True));data['report']['ownedQemuControl']['allocationMappingsReleasedAfterVmExit']=1
        with self.assertRaises(ValueError):validate_injected_alias_gpu_recovery(**data)

    def test_early_or_unauthenticated_injection_cannot_pass(self):
        data=self.inputs(152)
        for old,new in (('completedFrames=100 threshold=100','completedFrames=99 threshold=100'),
                        ('REFUSAL_INJECTED actor=1 ','REFUSAL_INJECTED actor=2 ')):
            with self.assertRaises(ValueError):injection_metadata(data['guest'].replace(old,new,1),data['records'])

    def test_exact_refused_resource_must_be_retried(self):
        data=self.inputs(152);r=self.row(152)['proof']['injectedRefusal']
        marker=f"BROKER_COORDINATOR_ALIAS donor={r['donorActor']} recipient={r['primaryActor']} kind=2 sourceTicket={r['sourceTicket']} "
        self.assertEqual(data['guest'].count(marker),1)
        with self.assertRaises(ValueError):injection_metadata(data['guest'].replace(marker,'REMOVED_ALIAS ',1),data['records'])

    def test_guest_refusal_cannot_be_relabelled_native(self):
        data=self.inputs(152)
        with self.assertRaises(ValueError):injection_metadata(data['guest'].replace('nativeAliasCalled=false','nativeAliasCalled=true',1),data['records'])

    def test_extra_driver_or_egl_failures_are_rejected(self):
        data=self.inputs(152)
        with self.assertRaises(ValueError):injection_metadata(data['guest']+'\nEGL_BAD_ALLOC\n',data['records'])
        with self.assertRaises(ValueError):injection_metadata(data['guest'],data['records']+[dict(nativeSharedAliasAdmissionRejected=True)])

    def test_initial_ambiguous_origin_label_remains_nonaccepted(self):
        data=self.inputs(151,True)
        proof=validate_nonaccepted_injection(151,**data)
        self.assertEqual(proof,self.row(151)['proof'])
        self.assertFalse(proof['controlledRecoveryAccepted'])
        with self.assertRaises(ValueError):injection_metadata(data['guest'],data['records'])

    def test_unexpected_calendar_keeps_original_wallpaper_rejection(self):
        proof=validate_nonaccepted_injection(153,**self.inputs(153,True))
        self.assertEqual(proof,self.row(153)['proof'])
        self.assertTrue(proof['originalWallpaperGuardRejected'])
        self.assertEqual(proof['wallpaperSampleCount'],601)
        self.assertEqual(proof['wallpaperSamplesOver40'],20)
        self.assertFalse(proof['controlledRecoveryAccepted'])

    def test_frozen_normal_auditor_still_rejects_diagnostic_guest(self):
        with self.assertRaises(ValueError):validate_multiapp_desktop(**self.inputs(152,True))

    def test_ordinary_build_excludes_diagnostic_and_invalid_builds_fail(self):
        def get(name):return self.blob(self.value['controls']['diagnostic-alias-refusal-controls-v2/'+name])
        receipt=json.loads(get('receipt.json'));log=get('controls.log').decode('utf-8')
        proof=validate_probe_controls(receipt,log)
        self.assertEqual(proof,self.value['probeProof'])
        receipt['normalBinaryContainsInjection']=True
        with self.assertRaises(ValueError):validate_probe_controls(receipt,log)

    def test_exact_compile_sources_and_auditors_remain_available(self):
        for digest in self.value['blobs']:self.blob(digest)
        for item in self.value['guestSourceArchives'].values():
            manifest=json.loads(self.blob(item['manifest']))
            for name,digest in manifest['sources'].items():self.assertEqual(digest,item['sources']['source-frozen/'+name])
            for name,digest in item['sources'].items():
                self.assertFalse(name.endswith(('.exe','.so','.ko','.ext4','.cpio.gz')));self.blob(digest)
        for name,digest in self.value['auditSources'].items():
            self.assertEqual(hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest(),digest)


if __name__=='__main__':unittest.main()
