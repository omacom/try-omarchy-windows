"""Keep hardware-only 4K failures rejected and prove the established scene survives."""
import base64
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_normal_root_graphics import validate


class Desktop4kCapacity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.path=cls.root/'docs/evidence/NORMAL-OMARCHY-4K-CAPACITY-CONTROLS-2026-10-16.json'
        cls.value=json.loads(cls.path.read_text())
        cls.runs={row['run']:row for row in cls.value['runs']}
        for row in cls.runs.values():
            row['report']=cls.unpack(row,'reportZlibBase64')
            row['records']=cls.unpack(row,'nativeRecordsZlibBase64')
            row['guest']=zlib.decompress(base64.b64decode(row['guestReceiptsZlibBase64'],validate=True)).decode()
        cls.regression_path=cls.root/'docs/evidence/NORMAL-OMARCHY-PROFILE5-REGRESSION-2026-10-16.json'
        cls.regression=json.loads(cls.regression_path.read_text())

    @staticmethod
    def unpack(row,key):
        return json.loads(zlib.decompress(base64.b64decode(row[key],validate=True)))

    def test_real_4k_inputs_source_unchanged_and_software_override_absent(self):
        for row in self.runs.values():
            source=row['source'];manifest=source['manifest']
            self.assertEqual(manifest['desktopGeometryProbe'],[3840,2160])
            self.assertFalse(manifest['qtQuickSoftwareProbe'])
            self.assertTrue(manifest['mesaWaylandGpuProbe'])
            self.assertTrue(source['sourceImageUnchanged'])
            self.assertEqual(source['sourceImageSha256Before'],source['sourceImageSha256After'])
            self.assertNotIn('qtQuickSoftware=true',row['guest'])
            self.assertTrue(row['report']['liveNvidiaLinuxUmdLoaded'])

    def test_unverified_4k_buffer_commit_is_not_a_verified_frame(self):
        for row in self.runs.values():
            self.assertIn('index=1 width=3840 height=2160 format=875713112 stride=15360',row['guest'])
            self.assertEqual(row['auditedFrameCount'],0)
            self.assertFalse(any(record.get('nativeCompositorBinaryCapture') for record in row['records']))
            self.assertEqual(row['report']['disconnectCleanup']['completedCompositorFrames'],0)
            for key in ('compositor4kVerified','fullDesktopGpuVerified','quickshellGpuVerified'):
                self.assertFalse(row[key])

    def test_legacy_map_quota_failures_retained(self):
        for number in (15,16):
            row=self.runs[number];clean=row['report']['disconnectCleanup']
            self.assertIn('ioctlFailed nr=12 errno=24',row['guest'])
            self.assertEqual(clean['vendorGpuMappedByteLimit'],256*1024*1024)
            self.assertLessEqual(clean['peakVendorMappedPages']*4096,clean['vendorGpuMappedByteLimit'])

    def test_map_budget_then_reported_usage_guard_counterfactual(self):
        row=self.runs[17];clean=row['report']['disconnectCleanup']
        self.assertEqual(clean['vendorGpuMappedByteLimit'],512*1024*1024)
        self.assertGreater(clean['peakVendorMappedPages']*4096,256*1024*1024)
        self.assertGreater(clean['peakReportedGpuUsageBytes'],clean['reportedGpuUsageLimitBytes'])
        self.assertTrue(row['report']['ownedQemuControl']['qemuForcedStop'])
        self.assertEqual(row['source']['manifest']['initramfsSha256'],self.runs[18]['source']['manifest']['initramfsSha256'])

    def test_revised_gpu_guards_reach_the_unchanged_cpu_mapping_limit(self):
        row=self.runs[18];clean=row['report']['disconnectCleanup']
        self.assertEqual(clean['vendorGpuMappedByteLimit'],512*1024*1024)
        self.assertEqual(clean['reportedGpuUsageLimitBytes'],512*1024*1024)
        self.assertLessEqual(clean['peakReportedGpuUsageBytes'],clean['reportedGpuUsageLimitBytes'])
        self.assertIn('ioctlFailed nr=37 errno=24',row['guest'])
        self.assertIn('signal=6',row['guest'])
        self.assertEqual(clean['vendorCpuMappedByteLimit'],96*1024*1024)
        self.assertGreater(clean['peakVendorCpuBytes']+8112*4096,clean['vendorCpuMappedByteLimit'])
        self.assertEqual(clean['vendorCpuSlotLimit'],256)
        self.assertEqual(clean['syncObjectLimit'],192)
        self.assertEqual(clean['vendorAllocationObjectLimit'],512)
        # Native cleanup after a VM exits cannot substitute for clean guest
        # retirement, missing pixels or a working Quickshell renderer.
        self.assertTrue(clean['driverCleanupVerified'])
        self.assertGreater(row['report']['ownedQemuControl']['fenceMappingsReleasedAfterVmExit'],0)
        with self.assertRaises(ValueError):
            validate(row['report'],row['guest'],row['records'],{},row['source'],expected_frames=12)

    def test_300_frame_version5_hardware_regression_passes(self):
        row=self.regression
        report=self.unpack(row,'reportZlibBase64');records=self.unpack(row,'nativeRecordsZlibBase64')
        # Preserve original serial bytes in evidence; the CLI auditor reads
        # text with Python's universal-newline conversion on Windows.
        guest=zlib.decompress(base64.b64decode(row['guestReceiptsZlibBase64'],validate=True)).decode().replace('\r\n','\n')
        frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}
        value=validate(report,guest,records,frames,row['source'],expected_frames=300)
        self.assertEqual(value,row['audit'])
        self.assertEqual(value['scene']['queued']['framesSubmitted'],300)
        self.assertEqual(value['scene']['queued']['lifecycle']['completedNativeSubmissions'],948)
        self.assertEqual(report['disconnectCleanup']['vendorGpuMappedByteLimit'],256*1024*1024)
        self.assertEqual(report['disconnectCleanup']['reportedGpuUsageLimitBytes'],256*1024*1024)
        self.assertFalse(value['desktopVerified'])

    def test_both_evidence_files_remain_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'c5234b1f18ba698e5f8ecfdff0001a9328c7d8312366ed531a4acc516d7fa006')
        self.assertEqual(hashlib.sha256(self.regression_path.read_bytes()).hexdigest(),'c1bf563b98e1c965d5c3d46c8bb2bea52314b7d174e9b7f595824104f47a3a67')


if __name__=='__main__':unittest.main()
