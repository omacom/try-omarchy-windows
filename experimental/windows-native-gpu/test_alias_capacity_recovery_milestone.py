"""Replay refusal controls separately from the corrected guest's GPU regression."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
import test_multiapp_cpu384_milestone as base
from verify_alias_capacity_recovery import validate_alias_capacity_desktop,validate_refusal_controls
from verify_multiapp_desktop import validate_multiapp_desktop


class AliasCapacityRecovery(unittest.TestCase):
    # Reuse the exact lossless blob/frame decoder, without inheriting its tests.
    blob = base.MultiappCpu384.__dict__['blob']
    inputs = base.MultiappCpu384.__dict__['inputs']

    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-ALIAS-CAPACITY-RECOVERY-2026-10-23.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='6ddebb125afe9def6d5d61fd46efb03d154901ace65be4c9fd1878eede410a59'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    @classmethod
    def row(cls,number):
        assert number==cls.value['run']==150
        return cls.value

    @classmethod
    def controls(cls):
        return tuple(cls.blob(cls.value['controls'][name]).decode('utf-8') for name in
                     ('resolve-refusal-before-v1/controls.log','resolve-refusal-after-v3/controls.log'))

    def test_capacity_refusal_preserves_real_rpc_and_live_fd_ownership(self):
        proof=validate_refusal_controls(*self.controls())
        self.assertEqual(proof,self.value['transportProof'])
        self.assertTrue(proof['sameConnectionCapacityRetryVerified'])
        self.assertTrue(proof['protocolAndUnexpectedDriverErrorsRemainFatal'])
        self.assertFalse(proof['gpuAdmissionFailureRecoveryVerified'])

    def test_corrected_guest_keeps_complete_two_window_gpu_rendering(self):
        proof=validate_alias_capacity_desktop(**self.inputs(150,True))
        self.assertEqual(proof,self.value['desktopProof'])
        self.assertEqual(proof['frameCount'],1800)
        self.assertEqual([r['maximumChannelError'] for r in proof['frames']],[1,1])
        self.assertTrue(proof['allQuotasUnchanged'])
        self.assertFalse(proof['gpuAdmissionFailureRecoveryVerified'])
        self.assertFalse(proof['intermittentAliasLimitResolved'])
        self.assertFalse(proof['fullDesktopAccepted'])

    def test_retry_receipt_cannot_be_relabelled_from_disconnection(self):
        before,after=self.controls()
        with self.assertRaises(ValueError): validate_refusal_controls(before,after.replace('sameConnectionRetry=1','sameConnectionRetry=0',1))

    def test_malformed_requests_cannot_be_relabelled_recoverable(self):
        before,after=self.controls()
        with self.assertRaises(ValueError): validate_refusal_controls(before,after.replace('errno=71 recoverable=0','errno=71 recoverable=1',1))

    def test_old_auditor_keeps_its_original_guest_pin(self):
        with self.assertRaises(ValueError): validate_multiapp_desktop(**self.inputs(150,True))

    def test_new_gpu_or_cpu_quotas_cannot_hide_capacity_failure(self):
        for key,value in (('cpuMappingBytes',805306368),('residentUsageGuardBytes',2147483648)):
            data=copy.deepcopy(self.inputs(150));data['source']['manifest']['managedGpuVaPolicy'][key]=value
            with self.assertRaises(ValueError): validate_alias_capacity_desktop(**data)

    def test_both_recovery_endpoints_must_be_requested(self):
        data=copy.deepcopy(self.inputs(150));data['source']['aliasCapacityRecoveryEnabled']=False
        with self.assertRaises(ValueError): validate_alias_capacity_desktop(**data)

    def test_control_and_guest_sources_keep_exact_compile_receipts(self):
        for digest in self.value['blobs']: self.blob(digest)
        receipt=json.loads(self.blob(self.value['controlSourceReceipt']))
        self.assertEqual(receipt,self.value['controlSourceArchives'])
        manifest=json.loads(self.blob(self.value['guestManifest']))
        for name,digest in manifest['sources'].items():
            self.assertEqual(digest,self.value['guestSourceArchive']['source-frozen/'+name])
        for source in (self.value['controlSourceArchives'],self.value['guestSourceArchive']):
            for name,digest in source.items():
                self.assertFalse(name.endswith(('.exe','.ko','.so','.ext4','.cpio.gz')));self.blob(digest)
        for name,digest in self.value['auditSources'].items():
            self.assertEqual(hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest(),digest)


if __name__=='__main__':unittest.main()
