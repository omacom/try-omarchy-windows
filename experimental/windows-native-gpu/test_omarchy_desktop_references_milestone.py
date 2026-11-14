"""Real 4K reference/readback fixes, with failed predecessors kept unaccepted."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_omarchy_desktop_references import APPLICATION, validate_content_frame


class ReferenceFixedGpuDesktop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        cls.value=json.loads((cls.repo/'docs/evidence/OMARCHY-4K-REFERENCE-FIX-GPU-DESKTOP-2026-10-19.json').read_text(encoding='utf-8'))
        base_receipt=cls.value['rawBase'];base_bytes=(cls.repo/base_receipt['evidencePath']).read_bytes()
        assert hashlib.sha256(base_bytes).hexdigest()==base_receipt['evidenceSha256']
        old=json.loads(base_bytes)
        base=zlib.decompress(base64.b64decode(next(r for r in old['runs'] if r['run']==base_receipt['run'])['rawFramesZlibBase64'][base_receipt['frameFile']],validate=True))
        assert hashlib.sha256(base).hexdigest()==base_receipt['rawSha256']
        base_int=int.from_bytes(base,'little');cls.inputs=[]
        assert cls.value['rawFrameEncoding']=='xor-base-then-zlib-base64'
        for row in cls.value['runs']+cls.value['retainedFailures']:
            unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
            frames={}
            for name,receipt in row['rawFrames'].items():
                delta=zlib.decompress(base64.b64decode(receipt['xorZlibBase64'],validate=True))
                assert len(delta)==len(base)==receipt['bytes']
                pixels=(base_int^int.from_bytes(delta,'little')).to_bytes(len(base),'little')
                assert hashlib.sha256(pixels).hexdigest()==receipt['rawSha256']
                frames[name]=pixels
            cls.inputs.append(dict(report=json.loads(unpack('reportZlibBase64')),guest=unpack('guestZlibBase64').decode(),
                records=json.loads(unpack('nativeRecordsZlibBase64')),source=json.loads(unpack('sourceZlibBase64')),
                frames=frames,wallpaper_reference=cls.value['wallpaperReference']))

    def test_two_fresh_gpu_captures_pass_with_all_tiles_and_unchanged_byte_limits(self):
        self.assertEqual([r['run'] for r in self.value['runs']],[59,60])
        for row,data in zip(self.value['runs'],self.inputs):
            proof=validate_content_frame(**data)
            self.assertEqual(proof,row['audit'])
            self.assertTrue(proof['wallpaperGpuVerified'] and proof['applicationGpuVerified'])
            self.assertGreater(proof['applicationVisibleGlyphPixels'],1000)
            clean=data['report']['disconnectCleanup']
            self.assertGreater(clean['peakSharedResourceHandles'],16)
            self.assertEqual(clean['sharedResourceReferenceLimit'],64)
            self.assertEqual(clean['reportedGpuUsageLimitBytes'],1073741824)
            self.assertFalse(proof['desktopAcceptanceVerified'])
            self.assertFalse(proof['allFramesDisplayedVerified'])
            self.assertFalse(proof['normalOmarchySessionVerified'])
            self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_exact_source_image_kernel_binary_and_capture_provenance(self):
        for data in self.inputs[:2]:
            self.assertEqual(data['report']['driverBridgeSha256'],'e9428143eadc8a4bee61d4bdbf8625ec135c71c12493485a0e7824126fd04bd7')
            self.assertEqual(data['report']['qemuSha256'],'4691955c77cb33ba656a7ad2b3cc97c7c514f8705dc46c5d328d641f38c8ba46')
            self.assertEqual(data['report']['initramfsSha256'],'06ebcb812f55f7054838d8de5218f8f0735c035bda849e9d057a9f9d9b80125a')
            self.assertEqual(data['source']['manifest']['kernelCapacityBuildReceipt']['moduleSha256'],'220d40f63c72c15e9c02c4ec9043e835a5a89a546e36d8e3b2fcc5e7bf65e6bb')
        image=self.value['image'];png=(self.repo/image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(),image['pngSha256'])
        self.assertEqual((image['run'],image['frame'],image['pixelsEdited']),(59,12,False))
        self.assertEqual(image['sourceRawSha256'],hashlib.sha256(self.inputs[0]['frames']['compositor-copy-2.raw']).hexdigest())
        self.assertEqual(self.value['auditSourceSha256'],hashlib.sha256(Path(__file__).with_name('verify_omarchy_desktop_references.py').read_bytes()).hexdigest())

    def test_missing_identity_tiles_fences_reference_limits_and_modified_pixels_rejected(self):
        for mutation in ('software','renderer','identity','agreement','references','tiles','tileBytes','tileFence','backbuffer','gpuBytes','cpuBytes','forced','pending','pixels'):
            data=copy.deepcopy(self.inputs[0]);clean=data['report']['disconnectCleanup']
            audits=[r for r in data['records'] if r.get('nativeCompositorRingAudited') is True]
            if mutation=='software':data['source']['manifest']['qtQuickSoftwareProbe']=True
            if mutation=='renderer':data['guest']=data['guest'].replace(APPLICATION,'Running on llvmpipe')
            if mutation=='identity':data['guest']=data['guest'].replace('\nBROKER_COORDINATOR_PEER','unterminatedBROKER_COORDINATOR_PEER')
            if mutation=='agreement':data['guest']=data['guest'].replace('objectsVersion=5 resources=64','objectsVersion=4 resources=16')
            if mutation=='references':clean['sharedResourceReferenceLimit']=16
            if mutation=='tiles':audits[1]['readbackTiles']=8
            if mutation=='tileBytes':audits[0]['readbackTileBytes']=33177600
            if mutation=='tileFence':audits[1]['auditFenceObserved']=17
            if mutation=='backbuffer':audits[0]['backbufferSource']=False
            if mutation=='gpuBytes':clean['peakReportedGpuUsageBytes']=1073741825
            if mutation=='cpuBytes':clean['peakVendorCpuBytes']=201326593
            if mutation=='forced':data['report']['ownedQemuControl']['qemuForcedStop']=True
            if mutation=='pending':clean['pendingGpuQueueWaits']=1
            if mutation=='pixels':
                pixels=data['frames']['compositor-copy-2.raw'];data['frames']['compositor-copy-2.raw']=bytes([pixels[0]^1])+pixels[1:]
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate_content_frame(**data)

    def test_original_admission_memory_and_framing_failures_remain_unaccepted(self):
        self.assertEqual([r['run'] for r in self.value['retainedFailures']],[56,57,58])
        for row,data in zip(self.value['retainedFailures'],self.inputs[2:]):
            self.assertFalse(row['fullDesktopCaptureAccepted'])
            self.assertTrue(data['source']['sourceImageUnchanged'])
            with self.assertRaises(ValueError):validate_content_frame(**data)
        admission=[r for r in self.inputs[2]['records'] if r.get('nativeSharedAliasAdmissionRejected') is True]
        self.assertTrue(any(r['liveReferences']==r['referenceLimit']==16 for r in admission))
        self.assertEqual(self.inputs[3]['report']['disconnectCleanup']['failedVendorResidencyRequests'],1)
        self.assertTrue(self.inputs[3]['report']['ownedQemuControl']['qemuForcedStop'])
        self.assertIn('hasPrimariesBROKER_COORDINATOR_PEER',self.inputs[4]['guest'])
        self.assertEqual((self.value['verificationStages'],self.value['desktopGates']),(4,0))


if __name__=='__main__':
    unittest.main()
