"""Replay actual QEMU SDL GPU buffers and reject incomplete or broader claims."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib

from verify_qemu_sdl_relay import validate_qemu_sdl_relay


class QemuSdlRelay(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/QEMU-SDL-GPU-RELAY-2026-10-21.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='82132c94595ba806a20aceeb2ae21d82c937c445d7ed82c0874dac9e908013e3'
        cls.evidence=json.loads(raw)
        cls.buffers={}
        for digest,row in cls.evidence['rawBuffers'].items():
            raw=zlib.decompress(base64.b64decode(row['zlibBase64'],validate=True))
            assert len(raw)==row['bytes'] and hashlib.sha256(raw).hexdigest()==digest
            cls.buffers[digest]=raw
        cls.inputs=[dict(source=row['source'],result=row['result'],
                         frames={name:cls.buffers[digest] for name,digest in row['rawFrames'].items()},
                         native_stdout=row['nativeStdout']) for row in cls.evidence['runs']]

    def test_six_accepted_controls_include_rgba_bgra_and_two_300_frame_streams(self):
        self.assertEqual([row['run'] for row in self.evidence['runs']],list(range(1,8)))
        for row,data in zip(self.evidence['runs'],self.inputs):
            if row['run']==3:
                continue
            proof=validate_qemu_sdl_relay(**data)
            self.assertEqual(proof,row['audit'])
            self.assertEqual(proof['completePixelAudits'],2)
            self.assertEqual(proof['actualGpuHandoffs'],300 if row['run'] in (4,6) else 2)
            self.assertTrue(proof['qemuNvidiaRootVerified'])
            self.assertTrue(proof['crossProcessGpuImportVerified'])
            self.assertFalse(proof['guestGpuScanoutVerified'])
            self.assertFalse(proof['physicalInputVerified'])
            self.assertFalse(proof['physicalScanoutVerified'])
            self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_original_bgra_pixels_remain_rejected_without_rewriting_history(self):
        row=self.evidence['runs'][2]
        self.assertFalse(row['audit']['independentAuditPassed'])
        self.assertTrue(row['audit']['protocolPassed'])
        self.assertEqual(list(self.inputs[2]['frames']['relay-source-0000000000000001.rgba'][:4]),
                         [128,64,32,255])
        with self.assertRaisesRegex(ValueError,'independent D3D12 quadrant pattern'):
            validate_qemu_sdl_relay(**self.inputs[2])

    def test_frozen_build_runner_auditors_and_previous_desktop_evidence(self):
        value=self.evidence
        for name,digest in value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(),digest)
        for digest,row in value['sources'].items():
            self.assertEqual(hashlib.sha256(row['utf8'].encode()).hexdigest(),digest)
            self.assertEqual(row['sha256'],digest)
        for data in self.inputs:
            self.assertIn(data['source']['runnerSha256'],value['sources'])
            for digest in data['source']['frozenSources'].values():
                self.assertIn(digest,value['sources'])
        previous=value['previousDesktopEvidence']
        self.assertEqual(hashlib.sha256((self.repo/previous['path']).read_bytes()).hexdigest(),previous['sha256'])

    def test_every_complete_source_and_actual_window_buffer_is_required(self):
        base=self.inputs[4]
        for name,raw in base['frames'].items():
            for replacement in (raw[:-4],bytes([raw[0]^16])+raw[1:]):
                data=dict(base,frames=dict(base['frames']));data['frames'][name]=replacement
                with self.subTest(name=name,bytes=len(replacement)),self.assertRaises(ValueError):
                    validate_qemu_sdl_relay(**data)
        data=dict(base,frames=dict(base['frames']))
        data['frames']['relay-window-0000000000000002.rgba']=base['frames']['relay-window-0000000000000001.rgba']
        with self.assertRaises(ValueError):
            validate_qemu_sdl_relay(**data)

    def test_ordinary_gpu_only_handoffs_and_real_fence_retirement_are_mandatory(self):
        base=self.inputs[5]
        for index,key,value in ((12,'auditFrames',2),(12,'cpuTextureUpload',True),
                                (12,'convertedFrames',11),(12,'rootAdapterVerified',False)):
            data=dict(base,result=copy.deepcopy(base['result']))
            data['result']['states'][index][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate_qemu_sdl_relay(**data)
        data=dict(base,result=copy.deepcopy(base['result']))
        records=data['result']['producerRecords'];records[25]['consumerFence']=11
        data['native_stdout']='\n'.join(json.dumps(row) for row in records)
        with self.assertRaises(ValueError):
            validate_qemu_sdl_relay(**data)

    def test_adapter_compiled_input_build_and_controller_cannot_be_mixed(self):
        base=self.inputs[4]
        mutations=(lambda s:s.update(qemuSha256='0'*64),
                   lambda s:s.update(runnerSha256='0'*64),
                   lambda s:s['qemuProvenance']['inputFiles'].update({'ui/sdl2.c':'0'*64}),
                   lambda s:s['compiledSources']['ui/sdl2-gl.c'].update(utf8='changed'),
                   lambda s:s['artifactReceipt']['files'].update({'bin/SDL2.dll':'0'*64}),
                   lambda s:s['producerReceipt'].update(binarySha256='0'*64))
        for mutate in mutations:
            data=dict(base,source=copy.deepcopy(base['source']));mutate(data['source'])
            with self.assertRaises(ValueError):
                validate_qemu_sdl_relay(**data)

    def test_owned_qmp_acknowledgements_and_gpu_cleanup_are_required(self):
        base=self.inputs[4]
        for key,value in (('liveImportedObjects',1),('channelProgramLive',True),('localScanoutLive',True)):
            data=dict(base,result=copy.deepcopy(base['result']));data['result']['states'][-1][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate_qemu_sdl_relay(**data)
        data=dict(base,result=copy.deepcopy(base['result']));data['result']['qmp'].pop()
        with self.assertRaises(ValueError):
            validate_qemu_sdl_relay(**data)
        data=dict(base,source=copy.deepcopy(base['source']));data['source']['qemuCommand'][-1]='tcp:0.0.0.0:4444'
        with self.assertRaises(ValueError):
            validate_qemu_sdl_relay(**data)

    def test_host_control_cannot_claim_guest_physical_display_or_performance(self):
        base=self.inputs[4]
        for location in ('source','result'):
            for key in ('physicalInputVerified','physicalScanoutVerified',
                        'guestGpuScanoutVerified','nearNativePerformanceVerified'):
                data=dict(base);data[location]=copy.deepcopy(base[location]);data[location][key]=True
                with self.subTest(location=location,key=key),self.assertRaises(ValueError):
                    validate_qemu_sdl_relay(**data)


if __name__=='__main__':
    unittest.main()
