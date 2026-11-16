"""Retained physical NVIDIA lifecycles, exact 4K bytes and unsafe-case rejection."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib
from verify_omarchy_live_gpu_lifecycles import APPLICATION, validate_content_frame


class LiveGpuLifecycles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-4K-LIVE-GPU-LIFECYCLES-2026-10-19.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='c6d529d70f59ddcdb60036ebb20cb82c7de80a85ffbb46d87dd04c0861719bdc'
        cls.value=json.loads(raw);receipt=cls.value['rawBase']
        old_bytes=(cls.repo/receipt['evidencePath']).read_bytes()
        assert hashlib.sha256(old_bytes).hexdigest()==receipt['evidenceSha256']
        old=json.loads(old_bytes)
        base=zlib.decompress(base64.b64decode(next(r for r in old['runs'] if r['run']==receipt['run'])['rawFramesZlibBase64'][receipt['frameFile']],validate=True))
        assert len(base)==33177600 and hashlib.sha256(base).hexdigest()==receipt['rawSha256']
        base_int=int.from_bytes(base,'little');cls.inputs=[]
        for row in cls.value['runs']+cls.value['retainedFailures']:
            unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
            frames={}
            # Failures reject before frame verification; retain their raw bytes
            # in the artifact without keeping every failed image resident here.
            if row['fullDesktopCaptureAccepted']:
                for name,r in row['rawFrames'].items():
                    delta=zlib.decompress(base64.b64decode(r['xorZlibBase64'],validate=True))
                    assert len(delta)==len(base)==r['bytes']
                    pixels=(base_int^int.from_bytes(delta,'little')).to_bytes(len(base),'little')
                    assert hashlib.sha256(pixels).hexdigest()==r['rawSha256']
                    frames[name]=pixels
            cls.inputs.append(dict(report=json.loads(unpack('reportZlibBase64')),guest=unpack('guestZlibBase64').decode(),
                records=json.loads(unpack('nativeRecordsZlibBase64')),source=json.loads(unpack('sourceZlibBase64')),
                frames=frames,wallpaper_reference=cls.value['wallpaperReference']))

    def test_two_fresh_full_gpu_lifecycle_audits(self):
        self.assertEqual([r['run'] for r in self.value['runs']],[67,68])
        for row,data in zip(self.value['runs'],self.inputs):
            proof=validate_content_frame(**data)
            self.assertEqual(proof,row['audit'])
            self.assertEqual((proof['gpuApplicationCycles'],proof['liveRetiredActors'],proof['peakConcurrentActors']),(5,6,3))
            self.assertTrue(proof['gpuLiveRetirementVerified'] and proof['wallpaperGpuVerified'] and proof['applicationGpuVerified'])
            self.assertEqual(proof['sharedResourceAliases'],21)
            self.assertEqual(proof['deferredApertureMappings'],0)
            for key in ('desktopAcceptanceVerified','normalOmarchySessionVerified','allFramesDisplayedVerified','nearNativePerformanceVerified'):
                self.assertFalse(proof[key])

    def test_exact_artifact_binary_and_source_provenance(self):
        for data in self.inputs[:2]:
            self.assertEqual(data['report']['driverBridgeSha256'],'d46a88f3a33eed07d5fe584d0a7b0dda10106364c9b3dabb8aa645f37c04f6c4')
            self.assertEqual(data['report']['initramfsSha256'],'fa8aeefc3d3aee66c0d162f20a850dbdd353d335de6739e9e0568cde4ca352f7')
            self.assertEqual(data['report']['qemuSha256'],'4691955c77cb33ba656a7ad2b3cc97c7c514f8705dc46c5d328d641f38c8ba46')
            self.assertEqual(data['source']['sourceImageSha256Before'],'3954123192111ce2d28cafb9dd108e1ca14c288bfc2429ed899b0f36aeccf942')
        image=self.value['image'];png=(self.repo/image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(),image['pngSha256'])
        self.assertEqual(struct.unpack('>II',png[16:24]),(3840,2160))
        self.assertEqual((image['run'],image['frame'],image['pixelsEdited']),(67,12,False))
        self.assertEqual(hashlib.sha256(self.inputs[0]['frames']['compositor-copy-2.raw']).hexdigest(),image['sourceRawSha256'])
        self.assertEqual(self.value['auditSourceSha256'],hashlib.sha256(Path(__file__).with_name('verify_omarchy_live_gpu_lifecycles.py').read_bytes()).hexdigest())

    def test_unsafe_lifetimes_missing_gpu_handoffs_and_cpu_scans_rejected(self):
        for mutation in ('donor','witness','batch','quiescence','timing','deferred','cpuScan','scope','software','pixels'):
            data=copy.deepcopy(self.inputs[0]);clean=data['report']['disconnectCleanup']
            steps=[r for r in data['records'] if r.get('nativeLiveRetirementStep') is True and r['attempted']]
            if mutation=='donor':data['guest']=re.sub(r'^BROKER_COORDINATOR_ALIAS donor=3 .+\n','',data['guest'],flags=re.M)
            if mutation=='witness':data['guest']=data['guest'].replace('compositorStillRunning=true rootOwnedWitness=true','compositorStillRunning=false rootOwnedWitness=false')
            if mutation=='batch':steps[0]['attempted']=17
            if mutation=='quiescence':steps[0]['gpuQuiescenceVerified']=False
            if mutation=='timing':steps[0]['nativeElapsedUs']=10**12
            if mutation=='deferred':data['report']['ownedQemuControl']['fenceMappingsReleasedAfterVmExit']=1
            if mutation=='cpuScan':clean['cpuAllocationFingerprintCalls']=1
            if mutation=='scope':next(r for r in data['records'] if r.get('retirementScope')=='owningDevice')['retirementScope']='pagingFence'
            if mutation=='software':data['guest']=data['guest'].replace(APPLICATION,'Running on llvmpipe')
            if mutation=='pixels':
                pixels=data['frames']['compositor-copy-2.raw'];data['frames']['compositor-copy-2.raw']=bytes([pixels[0]^1])+pixels[1:]
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate_content_frame(**data)

    def test_original_failures_remain_unaccepted(self):
        self.assertEqual([r['run'] for r in self.value['retainedFailures']],[61,62,63,64,65,66])
        for row,data in zip(self.value['retainedFailures'],self.inputs[2:]):
            self.assertFalse(row['fullDesktopCaptureAccepted'])
            self.assertTrue(data['source']['sourceImageUnchanged'])
            with self.subTest(run=row['run']),self.assertRaises((ValueError,KeyError)):validate_content_frame(**data)
        old=self.inputs[-1]
        self.assertIn('HYPRLAND_STARTUP_STOP reason=bounded-diagnostic',old['guest'])
        self.assertTrue(any(r.get('nativeGpuStateRetirementWait') is True and r.get('busyQueues') and
            r['busyQueues'][0]['device']!=r['mappingDevice'] for r in self.inputs[-3]['records']))

    def test_scan_removal_comparison_preserves_other_native_sources(self):
        old=self.inputs[-1];new=self.inputs[0]
        before=old['source']['nativeBuildReceipt']['sources'];after=new['source']['nativeBuildReceipt']['sources']
        self.assertEqual(set(before),set(after))
        self.assertEqual([k for k in before if before[k]!=after[k]],['experimental/windows-native-gpu/driver_bridge.cpp'])
        self.assertEqual(old['report']['initramfsSha256'],new['report']['initramfsSha256'])
        for data in self.inputs[:2]:
            self.assertEqual(data['report']['disconnectCleanup']['cpuAllocationFingerprintBytes'],0)
            times=[int(ms) for actor,ms in re.findall(r'^BROKER_COORDINATOR_LIVE_RETIREMENT_TIMING actor=(\d+) elapsedMs=(\d+)',data['guest'],re.M) if int(actor)>=3]
            self.assertEqual(len(times),5);self.assertLess(max(times),1000)
        self.assertGreater(min(int(ms) for actor,ms in re.findall(r'^BROKER_COORDINATOR_LIVE_RETIREMENT_TIMING actor=(\d+) elapsedMs=(\d+)',old['guest'],re.M) if int(actor) in (3,4,5,6)),5000)
        self.assertEqual((self.value['verificationStages'],self.value['desktopGates']),(4,0))


if __name__=='__main__':unittest.main()
