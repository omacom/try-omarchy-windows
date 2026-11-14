"""Replay real before/after teardown controls and retain their bounded scope."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_real_sdl_desktop import bgra_to_rgba,linear_blit
from verify_sdl_failure_teardown import validate_failure_teardown,validate_uninterrupted_regression


class SdlFailureTeardown(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gpu=Path(__file__).resolve().parent;cls.repo=cls.gpu.parents[1]
        raw=(cls.repo/'docs/evidence/SDL-FAILURE-TEARDOWN-2026-10-21.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='58c50e266c052450280718252a6f2952d1b6abfeae63d8da3a48312e16d74633'
        cls.value=json.loads(raw)
        def referenced(row):
            raw=(cls.repo/row['path']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256']
            return json.loads(raw)
        cls.base=referenced(cls.value['baseEvidence']);cls.older=referenced(cls.base['baseEvidence'])

    @staticmethod
    @lru_cache(maxsize=10)
    def blob(digest):
        cls=SdlFailureTeardown
        item=cls.value['blobs'].get(digest) or cls.base['blobs'].get(digest) or cls.older['blobs'][digest]
        raw=zlib.decompress(base64.b64decode(item['zlibBase64'],validate=True))
        if 'xorBase' in item:
            other=cls.blob(item['xorBase']);assert len(raw)==len(other)
            raw=(int.from_bytes(raw,'little')^int.from_bytes(other,'little')).to_bytes(len(raw),'little')
        assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==digest==item['sha256']
        return raw

    @classmethod
    def row(cls,number):return next(row for row in cls.value['runs'] if row['run']==number)

    @classmethod
    def inputs(cls,number):
        row=cls.row(number);records=[]
        for line in cls.blob(row['files']['driver.errors.log']).decode('utf-8').splitlines():
            try:records.append(json.loads(line))
            except ValueError:pass
        return dict(report=json.loads(cls.blob(row['files']['driver.json'])),records=records,
                    source=json.loads(cls.blob(row['files']['source.json'])))

    @classmethod
    @lru_cache(maxsize=1)
    def frames(cls,number):
        row=cls.row(number);frames={}
        def frame(name):
            if name not in frames:
                item=row['frames'][name]
                if 'blob' in item:raw=cls.blob(item['blob'])
                elif item['transform']=='bgra-to-rgba':raw=bgra_to_rgba(frame(item['native']))
                else:
                    assert item['transform']=='xor-linear-reference'
                    reference=linear_blit(frame(item['source']),3840,2160,item['width'],item['height'])
                    delta=cls.blob(item['deltaBlob']);assert len(reference)==len(delta)
                    raw=(int.from_bytes(reference,'little')^int.from_bytes(delta,'little')).to_bytes(len(delta),'little')
                assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==item['sha256']
                frames[name]=raw
            return frames[name]
        for name in row['frames']:frame(name)
        return frames

    def test_before_after_interruption_preserves_original_error_and_retires_output_live(self):
        for number in (112,113):
            row=self.row(number)
            self.assertEqual(validate_failure_teardown(**self.inputs(number),kind=row['kind']),row['proof'])
        self.assertEqual(self.inputs(112)['report']['hostBridgeExit'],3)
        self.assertEqual(self.inputs(113)['report']['hostBridgeExit'],1)
        self.assertTrue(self.row(113)['proof']['liveSdlDetachBeforeVmStop'])
        self.assertFalse(self.row(112)['proof']['normalNativeCleanupVerified'])

    def test_fatal_detach_failure_reaps_owned_vm_without_claiming_device_recovery(self):
        row=self.row(114);proof=validate_failure_teardown(**self.inputs(114),kind=row['kind'])
        self.assertEqual(proof,row['proof'])
        self.assertTrue(proof['ownedVmReapedBeforeHostFatalExit'])
        self.assertFalse(proof['normalNativeCleanupVerified'])
        self.assertFalse(proof['gpuDeviceLossRecoveryVerified'])

    def test_ordinary_300_frame_full_pixel_regression_and_live_retirement(self):
        proof=validate_uninterrupted_regression(**self.inputs(115),frames=self.frames(115))
        self.assertEqual(proof,self.row(115)['proof'])
        self.assertEqual([row['maximumChannelError'] for row in proof['frames']],[1,1])
        self.assertFalse(proof['fullDesktopAccepted'])
        self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_original_sources_reports_logs_and_failed_pixels_are_reconstructable(self):
        for archive in self.value['compiledSourceArchives'].values():
            self.assertEqual(archive['files'],archive['receipt']['sources'])
            self.assertEqual(archive['buildScript'],archive['receipt']['buildScriptSha256'])
            for digest in (*archive['files'].values(),archive['buildScript']):self.blob(digest)
        for row in self.value['runs']:
            source=self.inputs(row['run'])['source']
            self.assertEqual(source['nativeBuildReceipt'],self.value['compiledSourceArchives'][str(row['nativeVersion'])]['receipt'])
            self.assertEqual(source['sourceHarnessSha256'],row['runnerSource'])
            self.assertEqual(source['failureProbeHarnessSha256'],row['controllerSource'])
            for digest in (*row['files'].values(),row['runnerSource'],row['controllerSource']):self.blob(digest)
            self.frames(row['run'])
        self.assertEqual(hashlib.sha256((self.gpu/'verify_sdl_failure_teardown.py').read_bytes()).hexdigest(),self.value['auditorSha256'])
        self.assertEqual(hashlib.sha256((self.gpu/'test_owned_runtime_qemu.py').read_bytes()).hexdigest(),self.value['originalHarnessSource'])

    def test_missing_reap_first_failure_and_wrong_cleanup_order_are_rejected(self):
        original=self.inputs(114)
        for field in ('ownedQemuReaped','beforeHostFatalExit','driverObjectsRetained'):
            data=dict(original,records=copy.deepcopy(original['records']))
            next(row for row in data['records'] if row.get('nativeSdlFatalOwnerReaped'))[field]=False
            with self.subTest(field=field),self.assertRaises(ValueError):validate_failure_teardown(**data,kind='fixed-retirement')
        original=self.inputs(113);data=dict(original,records=copy.deepcopy(original['records']))
        next(row for row in data['records'] if row.get('nativeCoordinatorFailure'))['message']='Secondary teardown error'
        with self.assertRaises(ValueError):validate_failure_teardown(**data,kind='fixed-stream')
        data=dict(original,records=copy.deepcopy(original['records']))
        index=next(i for i,row in enumerate(data['records']) if row.get('nativeSdlCompositorDetached'))
        data['records'].append(data['records'].pop(index))
        with self.assertRaises(ValueError):validate_failure_teardown(**data,kind='fixed-stream')

    def test_leaks_unretired_fences_and_mixed_or_software_pixels_cannot_pass(self):
        original=self.inputs(113)
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['ownedQemuControl']['liveAllocationMappings']=1
        with self.assertRaises(ValueError):validate_failure_teardown(**data,kind='fixed-stream')
        data=dict(original,records=copy.deepcopy(original['records']))
        next(row for row in data['records'] if row.get('nativeSdlCompositorDetached'))['consumerFenceObserved']=63
        with self.assertRaises(ValueError):validate_failure_teardown(**data,kind='fixed-stream')
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['driverBridgeSha256']='0'*64
        with self.assertRaises(ValueError):validate_failure_teardown(**data,kind='fixed-stream')
        normal=self.inputs(115);data=dict(normal,records=copy.deepcopy(normal['records']))
        next(row for row in data['records'] if row.get('nativeSdlCompositorQueued'))['cpuUpload']=True
        with self.assertRaises(ValueError):validate_uninterrupted_regression(**data,frames=self.frames(115))


if __name__=='__main__':unittest.main()
