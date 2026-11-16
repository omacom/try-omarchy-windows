"""Immutable NVIDIA shell-frame proof and controls against overclaiming a desktop."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_coordinated_gpu_clients import validate_shell_frame,RENDERER


class CoordinatedShell(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        cls.value=json.loads((cls.repo/'docs/evidence/COORDINATED-GPU-SHELL-FRAME-2026-10-18.json').read_text())
        cls.inputs=[]
        for row in cls.value['runs']:
            unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
            cls.inputs.append(dict(report=json.loads(unpack('reportZlibBase64')),guest=unpack('guestZlibBase64').decode(),
                records=json.loads(unpack('nativeRecordsZlibBase64')),source=json.loads(unpack('sourceZlibBase64')),
                frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}))

    def test_two_fresh_nvidia_shell_frames_pass_scoped_audit(self):
        for data,row in zip(self.inputs,self.value['runs']):
            proof=validate_shell_frame(**data);self.assertEqual(proof,row['audit'])
            self.assertTrue(proof['gpuShellFrameVerified']);self.assertEqual(proof['sharedResourceAliases'],2)
            self.assertFalse(proof['fullOmarchyDesktopFrameVerified']);self.assertFalse(proof['desktopAcceptanceVerified'])
            self.assertFalse(proof['nearNativePerformanceVerified']);self.assertFalse(proof['allFramesDisplayedVerified'])
            self.assertGreater(proof['deferredFenceMappings'],0)
            self.assertEqual(proof['frames'][0]['barBrightPixels'],[0,20,0]) # Initial capture remains visibly incomplete.

    def test_fixed_native_kernel_raw_frame_and_published_image_provenance(self):
        expected=('0dfcf79af75d645baff265d8d9bd221cb86c32b74cdfb303501bbe175f475874','4998380eae426cb4506fd5db1439cfcbb1870d511b586492c64aae2086b4b8d7')
        for data,digest in zip(self.inputs,expected):
            self.assertEqual(data['report']['driverBridgeSha256'],'6c0338110409c6682313316fa7ed085432c85c373cf899e09a5b9ef8619e68b1')
            self.assertEqual(data['source']['manifest']['kernelCapacityBuildReceipt']['renderSha256'],'5eeccc2f6138308b40141d7632656c06312603c24c6d550af33d3c81e42dceec')
            self.assertEqual(hashlib.sha256(data['frames']['compositor-copy-2.raw']).hexdigest(),digest)
        image=self.value['image'];self.assertEqual(image['run'],45);self.assertEqual(image['frame'],12)
        self.assertEqual(hashlib.sha256((self.repo/image['path']).read_bytes()).hexdigest(),'b49c9361bd5404042dab6d7022e320055a4cd48da10dc7d26a9e8a96088add30')
        self.assertEqual(image['sourceRawSha256'],expected[-1])

    def test_software_foreign_pid_forged_alias_invisible_layer_and_missing_kernel_controls_rejected(self):
        for mutation in ('software','renderer','pid','alias','layer','prime'):
            data=copy.deepcopy(self.inputs[-1])
            if mutation=='software':data['source']['manifest']['qtQuickSoftwareProbe']=True
            if mutation=='renderer':data['guest']=data['guest'].replace(RENDERER,'OpenGL RENDERER: llvmpipe')
            if mutation=='pid':data['guest']=data['guest'].replace('omarchy-shell[831]','omarchy-shell[99999]')
            if mutation=='alias':data['guest']=data['guest'].replace('BROKER_COORDINATOR_ALIAS donor=2 recipient=1','BROKER_COORDINATOR_ALIAS donor=1 recipient=2')
            if mutation=='layer':data['guest']=data['guest'].replace('shellLayerObserved=true','shellLayerObserved=false')
            if mutation=='prime':data['guest']=data['guest'].replace('NATIVE_PRIME_CONTRACT_PASS checks=980','NATIVE_PRIME_CONTRACT_FAIL checks=980')
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate_shell_frame(**data)

    def test_unretired_waits_forced_vm_and_modified_frame_are_rejected(self):
        for mutation in ('wait','forced','bytes','budget','nativeAlias'):
            data=copy.deepcopy(self.inputs[-1])
            if mutation=='wait':data['report']['disconnectCleanup']['pendingGpuQueueWaits']=1
            if mutation=='forced':data['report']['ownedQemuControl']['qemuForcedStop']=True
            if mutation=='bytes':data['frames']['compositor-copy-2.raw']=data['frames']['compositor-copy-2.raw'][:-1]
            if mutation=='budget':next(r for r in data['records'] if 'nativeCoordinatedObjectCapacity' in r)['cpuBytes']*=2
            if mutation=='nativeAlias':next(r for r in data['records'] if 'nativeSharedReferenceAliased' in r)['independentNtReference']=False
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate_shell_frame(**data)

    def test_failures_and_actual_mock_test_scope_are_retained(self):
        self.assertEqual([r['run'] for r in self.value['retainedFailures']],list(range(32,44)))
        for row in self.value['retainedFailures']:
            report=zlib.decompress(base64.b64decode(row['reportZlibBase64'],validate=True))
            self.assertEqual(hashlib.sha256(report).hexdigest(),row['reportSha256']);self.assertTrue(row['sourceImageUnchanged'])
        for name,row in self.value['tests'].items():
            output=zlib.decompress(base64.b64decode(row['zlibBase64'],validate=True))
            self.assertEqual(hashlib.sha256(output).hexdigest(),row['sha256'])
            if name=='driver-coordinator-v8-test.log':self.assertIn(b'native=mock gpu=false',output)
            if name=='guest-coordinator-v5-test.log':self.assertIn(b'gpu=false',output)


if __name__=='__main__':unittest.main()
