"""Verify real shared-context GPU pixels and preserve the earlier control."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib

from verify_sdl_shared_context import SHARED_BUILD,validate_shared_context


class SdlSharedContext(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/SDL-SHARED-CONTEXT-2026-10-20.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='2df5070b23be50b1a3c2784ba6ba2d9da5c2bb2298108541fd9c0818196adfc5'
        cls.value=json.loads(raw)
        ref=cls.value['baseEvidence'];raw=(cls.repo/ref['path']).read_bytes()
        assert hashlib.sha256(raw).hexdigest()==ref['sha256']
        cls.original=json.loads(raw)
        cls.inputs=[]
        for row in cls.value['runs']:
            frames={}
            for name,capture in row['rawFrames'].items():
                base=next(item for item in cls.original['runs'] if item['run']==capture['baseRun'])
                raw=zlib.decompress(base64.b64decode(base['rawFrames'][capture['baseFrame']]['zlibBase64'],validate=True))
                assert len(raw)==capture['bytes'] and hashlib.sha256(raw).hexdigest()==capture['sha256']
                frames[name]=raw
            cls.inputs.append(dict(source=row['source'],records=row['nativeRecords'],frames=frames))

    def test_two_shared_gpu_runs_and_original_mode_compatibility(self):
        self.assertEqual([row['run'] for row in self.value['runs']],[8,9,10])
        for index,(row,data) in enumerate(zip(self.value['runs'],self.inputs)):
            proof=validate_shared_context(**data)
            self.assertEqual(proof,row['audit'])
            self.assertIs(proof['sharedImportContextVerified'],index<2)
            self.assertEqual(len(proof['frames']),2)
            self.assertFalse(proof['crossProcessImportVerified'])
            self.assertFalse(proof['qemuFrontendVerified'])
            self.assertFalse(proof['guestFrameVerified'])
            self.assertFalse(proof['physicalInputVerified'])
            self.assertFalse(proof['allFramesDisplayedVerified'])
            self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_frozen_shared_build_and_original_evidence_remain_distinct(self):
        self.assertEqual(self.value['buildPins'],SHARED_BUILD)
        self.assertNotEqual(self.original['buildPins']['sourceSha256'],SHARED_BUILD['sourceSha256'])
        self.assertEqual(hashlib.sha256(self.value['source']['utf8'].encode()).hexdigest(),SHARED_BUILD['sourceSha256'])
        self.assertEqual(hashlib.sha256(self.value['runner']['utf8'].encode()).hexdigest(),
                         self.value['runner']['sha256'])
        for name,digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(),digest)
        data=dict(self.inputs[0],source=copy.deepcopy(self.inputs[0]['source']))
        data['source']['buildReceipt']=self.original['runs'][0]['source']['buildReceipt']
        with self.assertRaises(ValueError):
            validate_shared_context(**data)

    def test_missing_mismatched_or_unshared_child_cannot_pass(self):
        for index,key,value in ((1,'distinctContexts',False),(1,'renderer','llvmpipe'),
                                (1,'version','4.6.0 NVIDIA 617.14'),
                                (3,'sharedImportContext',False),(6,'sharedImportContext',False)):
            data=dict(self.inputs[0],records=copy.deepcopy(self.inputs[0]['records']))
            data['records'][index][key]=value
            with self.subTest(index=index,key=key),self.assertRaises(ValueError):
                validate_shared_context(**data)
        for records in (self.inputs[0]['records'][:1]+self.inputs[0]['records'][2:],
                        self.inputs[0]['records']+[self.inputs[0]['records'][1]]):
            with self.assertRaises(ValueError):
                validate_shared_context(**dict(self.inputs[0],records=records))

    def test_root_context_gpu_pixels_and_both_fences_are_mandatory(self):
        base=self.inputs[0]
        for name in base['frames']:
            frames=dict(base['frames']);frames[name]=frames[name][:-4]
            with self.subTest(name=name),self.assertRaises(ValueError):
                validate_shared_context(**dict(base,frames=frames))
        frames=dict(base['frames']);frames['interop-window-2.rgba']=frames['interop-window-1.rgba']
        with self.assertRaises(ValueError):
            validate_shared_context(**dict(base,frames=frames))
        for key,value in (('observedConsumerFence',0),('observedProducerFence',0),
                          ('independentFences',False),('cpuUpload',True),('gpuUsageBytes',1073741825)):
            data=dict(base,records=copy.deepcopy(base['records']));data['records'][4][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate_shared_context(**data)

    def test_mode_invocation_and_cleanup_cannot_be_mixed(self):
        base=self.inputs[0]
        for source in (dict(base['source'],mode='full'),dict(base['source'],timedOut=True),
                       dict(base['source'],exitCode=1),dict(base['source'],guestFrameVerified=True),
                       dict(base['source'],command=base['source']['command'][:-1])):
            with self.assertRaises(ValueError):
                validate_shared_context(**dict(base,source=source))
        for key,value in (('glObjectsReleased',False),('sdlClosed',False),('sharedHandlesClosed',2),
                          ('guestFrameVerified',True),('nearNativePerformanceVerified',True)):
            data=dict(base,records=copy.deepcopy(base['records']));data['records'][-1][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate_shared_context(**data)


if __name__=='__main__':
    unittest.main()
