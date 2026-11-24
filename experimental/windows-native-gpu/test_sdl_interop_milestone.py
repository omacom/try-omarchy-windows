"""Replay complete host GPU captures without claiming guest/SDL integration."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib

from verify_sdl_interop import BUILD_PINS, SDL_LIBRARY, validate_sdl_interop


class SdlD3D12Interop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/SDL-D3D12-INTEROP-2026-10-20.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='244324236762d4f554f49e639b78d1f413260e0c390da80a780a3cb9aaf8d665'
        cls.value=json.loads(raw)
        cls.inputs=[]
        for row in cls.value['runs']:
            frames={}
            for name,capture in row['rawFrames'].items():
                raw=zlib.decompress(base64.b64decode(capture['zlibBase64'],validate=True))
                assert len(raw)==capture['bytes']
                assert hashlib.sha256(raw).hexdigest()==capture['sha256']
                frames[name]=raw
            cls.inputs.append(dict(source=row['source'],records=row['nativeRecords'],frames=frames))

    def reject_record(self,index,key,value):
        data=dict(self.inputs[0],records=copy.deepcopy(self.inputs[0]['records']))
        data['records'][index][key]=value
        with self.assertRaises(ValueError):
            validate_sdl_interop(**data)

    def test_two_complete_gpu_pixel_and_independent_fence_runs(self):
        self.assertEqual([row['run'] for row in self.value['runs']],[6,7])
        self.assertTrue(self.value['hostSdlD3D12InteropRepeated'])
        for row,data in zip(self.value['runs'],self.inputs):
            proof=validate_sdl_interop(**data)
            self.assertEqual(proof,row['audit'])
            self.assertTrue(proof['namedObjectsVerified'])
            self.assertTrue(proof['independentGpuFencesVerified'])
            self.assertEqual(proof['diagnosticReadbacks'],4)
            self.assertEqual(proof['originalSharedHandlesClosed'],3)
            self.assertEqual([frame['producerFence'] for frame in proof['frames']],[1,2])
            self.assertTrue(proof['windowHidden'])
            for key in ('qemuFrontendVerified','guestFrameVerified','physicalInputVerified',
                        'allFramesDisplayedVerified','nearNativePerformanceVerified'):
                self.assertFalse(proof[key])

    def test_captured_build_dependency_and_auditor_provenance(self):
        self.assertEqual(self.value['buildPins'],BUILD_PINS)
        self.assertEqual(self.value['sdlLibrarySha256'],SDL_LIBRARY)
        keys={'sdl_d3d12_interop.cpp':'sourceSha256','gl_external_api.h':'bindingHeaderSha256',
              'build.ps1':'buildScriptSha256'}
        for path,capture in self.value['buildSources'].items():
            digest=hashlib.sha256(capture['utf8'].encode()).hexdigest()
            self.assertEqual(digest,capture['sha256'])
            self.assertEqual(digest,BUILD_PINS[keys[Path(path).name]])
        for name,digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(),digest)
        self.assertEqual(hashlib.sha256(self.value['runner']['utf8'].encode()).hexdigest(),
                         self.value['runner']['sha256'])
        prior=self.value['previousDesktopEvidence']
        self.assertEqual(hashlib.sha256((self.repo/prior['path']).read_bytes()).hexdigest(),prior['sha256'])

    def test_each_complete_source_and_window_buffer_is_required(self):
        base=self.inputs[0]
        for name in base['frames']:
            with self.subTest(name=name):
                frames=dict(base['frames'])
                frames[name]=frames[name][:-4]
                with self.assertRaises(ValueError):
                    validate_sdl_interop(**dict(base,frames=frames))
                frames=dict(base['frames'])
                pixels=bytearray(frames[name]);pixels[len(pixels)//3]^=128
                frames[name]=bytes(pixels)
                with self.assertRaises(ValueError):
                    validate_sdl_interop(**dict(base,frames=frames))
        frames=dict(base['frames'])
        frames['interop-source-2.rgba']=frames['interop-source-1.rgba']
        with self.assertRaises(ValueError):
            validate_sdl_interop(**dict(base,frames=frames))

    def test_fences_are_distinct_retired_and_in_order(self):
        for index in (3,4):
            for key in ('producerFence','consumerFence','observedProducerFence','observedConsumerFence'):
                with self.subTest(index=index,key=key):
                    self.reject_record(index,key,0)
                    self.reject_record(index,key,index)
            self.reject_record(index,'independentFences',False)
        self.reject_record(2,'independentFences',False)
        for records in (self.inputs[0]['records'][:-1],
                        self.inputs[0]['records']+[self.inputs[0]['records'][3]],
                        self.inputs[0]['records'][:3]+list(reversed(self.inputs[0]['records'][3:5]))+
                        self.inputs[0]['records'][5:]):
            with self.assertRaises(ValueError):
                validate_sdl_interop(**dict(self.inputs[0],records=records))

    def test_matching_hardware_adapter_and_unchanged_usage_guard(self):
        for index,key,value in ((1,'sameAdapterLuid',False),(1,'vendorId',0x1414),
                                (0,'renderer','llvmpipe'),(1,'windowHidden',False),
                                (1,'gpuUsageBytes',1073741825),(3,'gpuUsageBytes',1073741825),
                                (4,'gpuUsageBytes',0)):
            with self.subTest(index=index,key=key):
                self.reject_record(index,key,value)

    def test_no_cpu_upload_or_unsupported_display_guest_performance_claims(self):
        for index,key,value in ((3,'cpuUpload',True),(4,'diagnosticReadbacks',0),
                                (3,'displayVerified',True),(1,'qemuFrontendVerified',True),
                                (5,'qemuFrontendVerified',True),(5,'guestFrameVerified',True),
                                (5,'nearNativePerformanceVerified',True)):
            with self.subTest(index=index,key=key):
                self.reject_record(index,key,value)
        for key in ('glObjectsReleased','d3dObjectsReleased','sdlClosed','namedObjects','independentFences'):
            self.reject_record(5,key,False)
        self.reject_record(5,'sharedHandlesClosed',2)
        self.reject_record(2,'originalNtHandlesClosed',False)

    def test_failed_partial_mixed_and_relative_invocations_are_rejected(self):
        for key,value in (('mode','capabilities-only'),('exitCode',1),('timedOut',True),
                          ('sourcesUnchanged',False),('sdlLibrarySha256','0'*64),
                          ('qemuArtifactRevision','0'*40),('guestFrameVerified',True)):
            source=copy.deepcopy(self.inputs[0]['source']);source[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate_sdl_interop(**dict(self.inputs[0],source=source))
        for key in BUILD_PINS:
            source=copy.deepcopy(self.inputs[0]['source']);source['buildReceipt'][key]='0'*64
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate_sdl_interop(**dict(self.inputs[0],source=source))
        for index in range(3):
            source=copy.deepcopy(self.inputs[0]['source'])
            source['command'][index]=Path(source['command'][index].replace('\\','/')).name
            with self.subTest(command=index),self.assertRaises(ValueError):
                validate_sdl_interop(**dict(self.inputs[0],source=source))

    def test_official_input_patch_controls_have_separate_build_scope(self):
        value=self.value['qemuCandidateControls']
        self.assertTrue(value['officialInputHashesUnchanged'])
        self.assertEqual(value['inputFiles'],value['patchPreparation']['inputFiles'])
        self.assertEqual(len(value['patchPreparation']['officialRuntimePatches']),24)
        self.assertEqual(value['patchPreparation']['qemuBaseCommit'],
                         '2ce303cfbbc8b0e4a7a3c66e27a094a980426d73')
        self.assertEqual([row['script'] for row in value['controls']],
                         ['runtime-build/test-sdl-'+name+'.py' for name in
                          ('input','cursor','keyboard-hook','scanout')])
        self.assertTrue(all(row['exitCode']==0 and 'ok - ' in row['stdout'] and not row['stderr']
                            for row in value['controls']))
        self.assertEqual(value['shellSyntax']['exitCode'],0)
        self.assertFalse(value['qemuBinaryCompiled'])
        self.assertFalse(value['guestFrontendVerified'])


if __name__=='__main__':
    unittest.main()
