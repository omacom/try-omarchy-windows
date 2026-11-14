"""Keep first 4K GPU backbuffer proof separate from missing desktop/stability gates."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import struct
import unittest
import zlib
from verify_4k_compositor_readback import validate
from verify_normal_root_graphics import validate as legacy_validate


class Compositor4kReadback(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.path=cls.root/'docs/evidence/NORMAL-OMARCHY-4K-GPU-READBACK-2026-10-16.json'
        cls.value=json.loads(cls.path.read_text())
        unpack=lambda key:zlib.decompress(base64.b64decode(cls.value[key],validate=True))
        cls.report=json.loads(unpack('reportZlibBase64'));cls.records=json.loads(unpack('nativeRecordsZlibBase64'))
        cls.guest=unpack('guestReceiptsZlibBase64').decode().replace('\r\n','\n')
        cls.frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in cls.value['rawFramesZlibBase64'].items()}
        cls.audit=validate(cls.report,cls.guest,cls.records,cls.frames,cls.value['source'])

    def _audit(self,**changes):
        args=dict(report=self.report,guest=self.guest,records=self.records,frames=self.frames,source=self.value['source']);args.update(changes)
        return validate(**args)

    def test_two_full_resolution_native_backbuffers_pass_gpu_readback_scope(self):
        self.assertEqual(self.audit,self.value['audit'])
        self.assertTrue(self.audit['compositor4kReadbackVerified'])
        self.assertEqual(self.audit['framesSubmitted'],12)
        self.assertEqual(self.audit['ordinaryFramesWithoutCpuReadback'],10)
        self.assertEqual([r['frame'] for r in self.audit['frames']],[1,12])
        for r in self.audit['frames']:self.assertEqual((r['width'],r['height'],r['bytes']),(3840,2160,33177600))

    def test_exact_unscaled_png_from_terminal_backbuffer_is_retained(self):
        image=self.root/self.value['image']['file'];data=image.read_bytes()
        self.assertEqual(data[:8],b'\x89PNG\r\n\x1a\n')
        self.assertEqual(struct.unpack('>IIBB',data[16:26]),(3840,2160,8,2))
        self.assertEqual(hashlib.sha256(data).hexdigest(),'20ef42801c34059c6d928f55f4505b870e1046065e36dd73491d93cad3aa69f7')
        self.assertEqual(self.value['image']['sourceFrame'],12)
        self.assertEqual(self.audit['frames'][1]['rawSha256'],'d59a2b4f1064ca48d1b01c41c993b3c38cc81ac2e7a6124a9e335a07bc9fcab8')

    def test_software_missing_shell_budget_failure_and_deferred_cleanup_are_not_hidden(self):
        for key in ('desktopVerified','quickshellGpuVerified','normalOmarchySessionVerified','fullScenePixelCorrectnessVerified','allFramesDisplayedVerified','nearNativePerformanceVerified'):
            self.assertFalse(self.audit[key])
        self.assertTrue(self.audit['reportedGpuUsageGuardExceeded'])
        self.assertTrue(self.audit['diagnosticChildSignaled'])
        self.assertEqual(self.audit['deferredMappings'],88)
        self.assertEqual(self.audit['observedPresentCount'],8)
        self.assertTrue(self.audit['nativeCleanupVerified'])
        self.assertIn('ioctlFailed nr=6 errno=12',self.guest)
        layers=[line.split(' ',1)[1] for line in self.guest.splitlines() if line.startswith('OMARCHY_DESKTOP_LAYERS_HEX ')]
        self.assertEqual(len(layers),1)
        values=json.loads(bytes.fromhex(layers[0]))
        self.assertTrue(all(not layer for monitor in values.values() for layer in monitor['levels'].values()))

    def test_unretired_source_copy_and_cpu_upload_are_rejected(self):
        for field,value in [('sourceCopyRetired',False),('cpuUpload',True),('producerFenceObserved',0)]:
            records=copy.deepcopy(self.records);row=next(r for r in records if r.get('nativeCompositorRingInput'));row[field]=value
            with self.assertRaises(ValueError):self._audit(records=records)

    def test_wrong_geometry_stale_bytes_and_missing_frames_are_rejected(self):
        records=copy.deepcopy(self.records);next(r for r in records if r.get('nativeCompositorBinaryCapture'))['width']=1920
        with self.assertRaises(ValueError):self._audit(records=records)
        frames=dict(self.frames);frames['compositor-copy-1.raw']=frames['compositor-copy-1.raw'][:-1]
        with self.assertRaises(ValueError):self._audit(frames=frames)
        frames=dict(self.frames);frames['compositor-copy-1.raw']=self.frames['compositor-copy-2.raw']
        with self.assertRaises(ValueError):self._audit(frames=frames)

    def test_budget_or_provenance_cannot_be_rewritten_to_clear_failure(self):
        report=copy.deepcopy(self.report);report['disconnectCleanup']['reportedGpuUsageLimitBytes']=1024*1024*1024
        with self.assertRaises(ValueError):self._audit(report=report)
        report=copy.deepcopy(self.report);report['driverBridgeSha256']='0'*64
        with self.assertRaises(ValueError):self._audit(report=report)
        source=copy.deepcopy(self.value['source']);source['manifest']['qtQuickSoftwareProbe']=True
        with self.assertRaises(ValueError):self._audit(source=source)

    def test_evidence_is_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'57d7f8efe6a982b9c9fa78462346f9f93518758b1f1943e7177419dd58930720')

    def test_new_native_build_preserves_300_frame_legacy_rendering(self):
        path=self.root/'docs/evidence/NORMAL-OMARCHY-LARGE-MAP-LEGACY-REGRESSION-2026-10-16.json'
        row=json.loads(path.read_text());unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
        report=json.loads(unpack('reportZlibBase64'));records=json.loads(unpack('nativeRecordsZlibBase64'))
        guest=unpack('guestReceiptsZlibBase64').decode().replace('\r\n','\n')
        frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}
        proof=legacy_validate(report,guest,records,frames,row['source'],expected_frames=300)
        self.assertEqual(proof,row['audit'])
        self.assertEqual(proof['scene']['queued']['framesSubmitted'],300)
        self.assertEqual(proof['scene']['queued']['lifecycle']['completedNativeSubmissions'],948)
        clean=report['disconnectCleanup']
        self.assertEqual(clean['vendorGpuMappedByteLimit'],256*1024*1024)
        self.assertEqual(clean['reportedGpuUsageLimitBytes'],256*1024*1024)
        self.assertEqual(clean['vendorCpuMappedByteLimit'],96*1024*1024)
        self.assertTrue(clean['driverCleanupVerified'])
        self.assertFalse(proof['desktopVerified'])

    def test_explicit_large_map_transport_mutations_and_default_rejections_pass(self):
        logs=self.value['validationLogs'];decode=lambda key:zlib.decompress(base64.b64decode(logs[key],validate=True)).decode()
        controls=decode('profile9-controls-v1/build.log')
        self.assertIn('BROKER_LARGE_MAP exact1GiB=true fixed40ByteReceipt=true malformedRejected=true oldProfilesUnchanged=true kernel=mock gpu=false',controls)
        self.assertIn('PASS: 22 explicit OpenGL transport cases',controls)
        self.assertIn('PASS: 21 explicit OpenGL transport cases',controls)
        self.assertIn('PASS: 105 native capacity/control rejection cases',decode('profile9-default-cli.log'))


if __name__=='__main__':unittest.main()
