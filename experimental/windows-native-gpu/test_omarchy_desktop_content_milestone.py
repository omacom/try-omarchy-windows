"""Audit actual 4K GPU desktop bytes while retaining the later live-work failure."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib
from verify_omarchy_desktop_content import APPLICATION, validate_content_frame


class ReadableGpuDesktop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        cls.value=json.loads((cls.repo/'docs/evidence/OMARCHY-4K-READABLE-GPU-DESKTOP-2026-10-19.json').read_text(encoding='utf-8'))
        cls.inputs=[]
        for row in cls.value['runs']+cls.value['retainedFailures']:
            unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
            cls.inputs.append(dict(report=json.loads(unpack('reportZlibBase64')),guest=unpack('guestZlibBase64').decode(),
                records=json.loads(unpack('nativeRecordsZlibBase64')),source=json.loads(unpack('sourceZlibBase64')),
                frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row.get('rawFramesZlibBase64',{}).items()},
                wallpaper_reference=cls.value['wallpaperReference']))

    def test_two_fresh_full_content_captures_pass_the_scoped_audit(self):
        self.assertEqual([row['run'] for row in self.value['runs']],[53,54])
        for data,row,commands in zip(self.inputs,self.value['runs'],(850,739)):
            proof=validate_content_frame(**data)
            self.assertEqual(proof,row['audit'])
            self.assertTrue(proof['fullOmarchyDesktopFrameVerified'])
            self.assertTrue(proof['applicationGpuVerified'])
            self.assertEqual(proof['completedNativeSubmissions'],commands)
            self.assertEqual(proof['applicationVisibleGlyphPixels'],19773)
            self.assertFalse(proof['desktopAcceptanceVerified'])
            self.assertFalse(proof['normalOmarchySessionVerified'])
            self.assertFalse(proof['allFramesDisplayedVerified'])
            self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_immutable_hardware_image_kernel_and_byte_budget_provenance(self):
        for data in self.inputs[:2]:
            self.assertEqual(data['report']['driverBridgeSha256'],'b3c541fb122241f3bb4b4d0f1e9adc3edaf429c488586474ec178916bce885a5')
            self.assertEqual(data['report']['qemuSha256'],'4691955c77cb33ba656a7ad2b3cc97c7c514f8705dc46c5d328d641f38c8ba46')
            self.assertEqual(data['source']['manifest']['kernelCapacityBuildReceipt']['moduleSha256'],'220d40f63c72c15e9c02c4ec9043e835a5a89a546e36d8e3b2fcc5e7bf65e6bb')
            self.assertEqual(data['source']['manifest']['sources']['gpu-terminal-scene.sh'],'ecb5c8149c31acf1254e9d6b6ab36bc811f7dbe1a3f5015fdeb70caa08237b58')
            self.assertGreaterEqual(max(map(int,re.findall(r'allocationCpuMapped=true[^\n]*offset=(\d+)',data['guest']))),512*1048576)
        image=self.value['image'];png=(self.repo/image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(),'d71bda155cb28558689e660818707a7f215e79aa449de1b227eb1a5aaa2e6ec0')
        self.assertEqual(png[:8],b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>II',png[16:24]),(3840,2160))
        self.assertEqual((image['run'],image['frame'],image['pixelsEdited']),(53,12,False))
        self.assertEqual(image['sourceRawSha256'],hashlib.sha256(self.inputs[0]['frames']['compositor-copy-2.raw']).hexdigest())
        self.assertEqual(self.value['auditSourceSha256'],hashlib.sha256(Path(__file__).with_name('verify_omarchy_desktop_content.py').read_bytes()).hexdigest())

    def test_software_missing_content_failed_lifetimes_and_changed_pixels_rejected(self):
        for mutation in ('software','applicationRenderer','pty','windowClose','reset','slots','source','quota','bytes','forced','pending','pixels','wallpaper'):
            data=copy.deepcopy(self.inputs[1])
            if mutation=='software':data['source']['manifest']['qtQuickSoftwareProbe']=True
            if mutation=='applicationRenderer':data['guest']=data['guest'].replace(APPLICATION,'Running on llvmpipe')
            if mutation=='pty':data['guest']=data['guest'].replace('ptyMarkerObserved=true','ptyMarkerObserved=false')
            if mutation=='windowClose':data['guest']=data['guest'].replace('clientUnmapped=true','clientUnmapped=false')
            if mutation=='reset':data['guest']+='\nGL_OUT_OF_MEMORY\n'
            if mutation=='slots':data['guest']=data['guest'].replace('DESKTOP_SLOT_CONTRACT enabled=1','DESKTOP_SLOT_CONTRACT enabled=0')
            if mutation=='source':data['source']['nativeBuildReceipt']['sources']['experimental/windows-native-gpu/driver_wire.h']='0'*64
            if mutation=='quota':data['report']['disconnectCleanup']['vendorCpuSlotQuotaRejections']=1
            if mutation=='bytes':data['report']['disconnectCleanup']['peakVendorCpuBytes']=201326593
            if mutation=='forced':data['report']['ownedQemuControl']['qemuForcedStop']=True
            if mutation=='pending':data['report']['disconnectCleanup']['pendingGpuQueueWaits']=1
            if mutation=='pixels':
                pixels=data['frames']['compositor-copy-2.raw'];data['frames']['compositor-copy-2.raw']=bytes([pixels[0]^1])+pixels[1:]
            if mutation=='wallpaper':data['wallpaper_reference']['sourceJpegSha256']='0'*64
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate_content_frame(**data)

    def test_failed_readiness_and_current_alias_rejection_remain_unaccepted(self):
        self.assertEqual([row['run'] for row in self.value['retainedFailures']],[52,55])
        for row,data in zip(self.value['retainedFailures'],self.inputs[2:]):
            self.assertFalse(row['fullDesktopCaptureAccepted'])
            self.assertTrue(data['source']['sourceImageUnchanged'])
            self.assertEqual(data['source']['sourceImageSha256Before'],data['source']['sourceImageSha256After'])
            self.assertFalse(data['report']['ownedQemuControl']['qemuForcedStop'])
            with self.assertRaises(ValueError):validate_content_frame(**data)
        self.assertNotIn('OMARCHY_GPU_APPLICATION_CONTENT_READY',self.inputs[2]['guest'])
        self.assertIn('reason=Native recipient alias failed',self.inputs[3]['guest'])
        self.assertIn('NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=6',self.inputs[3]['guest'])
        self.assertEqual((self.value['verificationStages'],self.value['desktopGates']),(4,0))
        self.assertFalse(self.value['desktopAcceptanceVerified'])


if __name__=='__main__':
    unittest.main()
