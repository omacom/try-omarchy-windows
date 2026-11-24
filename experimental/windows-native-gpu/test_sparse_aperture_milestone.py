"""Real sparse CPU mappings, retained 4K failure, and exact legacy regression."""
import base64
import hashlib
import json
from pathlib import Path
import re
import unittest
import zlib
from verify_normal_root_graphics import validate


def unpack(row,key):return zlib.decompress(base64.b64decode(row[key],validate=True))


class SparseAperture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path=Path(__file__).resolve().parents[2]/'docs/evidence/NORMAL-OMARCHY-SPARSE-APERTURE-2026-10-16.json'
        cls.value=json.loads(cls.path.read_text())
        for key in ('failed4k','legacy'):
            row=cls.value[key]
            row['report']=json.loads(unpack(row,'reportZlibBase64'))
            row['guest']=unpack(row,'guestReceiptsZlibBase64').decode().replace('\r\n','\n')
            row['records']=json.loads(unpack(row,'nativeRecordsZlibBase64'))

    def test_exact_kernel_native_and_qemu_sparse_capacity(self):
        row=self.value['failed4k'];report=row['report'];guest=row['guest']
        self.assertIn('APERTURE_CAPACITY version=1 flags=3 cpuByteLimit=201326592 exact=true\n',guest)
        self.assertIn('APERTURE_CONTRACT_PASS checks=1101 activeLeases=0 activeCpuBytes=0\n',guest)
        self.assertIn('kernelCpuCapacityVerified=true profile=8 cpuByteLimit=201326592 cpuSlots=512 fenceSlots=256\n',guest)
        profiles=[r for r in row['records'] if r.get('openGlProfileNegotiated')]
        self.assertEqual(len(profiles),1)
        self.assertEqual((profiles[0]['version'],profiles[0]['cpuMappedByteLimit'],profiles[0]['cpuApertureSlots']),(8,192*1024*1024,512))
        self.assertEqual(report['ownedQemuControl']['allocationApertureSlots'],512)
        self.assertEqual(row['source']['manifest']['desktopGeometryProbe'],[3840,2160])
        self.assertFalse(row['source']['manifest']['qtQuickSoftwareProbe'])
        self.assertTrue(report['liveNvidiaLinuxUmdLoaded'])

    def test_real_direct_cpu_mapping_crosses_the_legacy_aperture_boundary(self):
        row=self.value['failed4k'];guest=row['guest']
        maps=[tuple(map(int,m)) for m in re.findall(r'allocationCpuMapped=true direct=true loads=10000 bytes=(\d+) offset=(\d+) generation=(\d+)\n',guest)]
        self.assertTrue(any(offset<256*1024*1024<offset+size for size,offset,generation in maps))
        self.assertIn((33226752,241172480,231),maps)
        qemu=row['report']['ownedQemuControl']
        self.assertEqual(qemu['allocationSlotMappingsCreated'],265)
        self.assertGreater(qemu['allocationSlotMappingsCreated']-qemu['allocationSlotUnmapAcknowledgements'],256)
        self.assertIn('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true\n',guest)

    def test_next_gpu_map_quota_and_unretired_waits_remain_failed(self):
        row=self.value['failed4k'];clean=row['report']['disconnectCleanup']
        self.assertEqual(clean['vendorCpuSlotQuotaRejections'],0)
        self.assertEqual(clean['vendorCpuMappedByteLimit'],192*1024*1024)
        self.assertEqual(clean['peakVendorCpuBytes'],124198912)
        self.assertEqual(clean['vendorGpuMappedByteLimit'],512*1024*1024)
        self.assertEqual(clean['peakVendorMappedPages'],130090)
        self.assertGreater((clean['peakVendorMappedPages']+self.value['rejectedGpuMapPages'])*4096,clean['vendorGpuMappedByteLimit'])
        self.assertLess(clean['peakReportedGpuUsageBytes'],clean['reportedGpuUsageLimitBytes'])
        self.assertIn('ioctlFailed nr=12 errno=24\n',row['guest'])
        self.assertIn('NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=6',row['guest'])
        self.assertFalse(clean['driverCleanupVerified'])
        self.assertEqual(clean['pendingGpuQueueWaits'],3)
        self.assertEqual(clean['pendingHwQueueSignals'],2)
        self.assertEqual(row['report']['hostBridgeExit'],1)

    def test_no_4k_pixels_or_full_desktop_acceptance_is_inferred(self):
        row=self.value['failed4k']
        self.assertEqual(row['rawFramesZlibBase64'],{})
        self.assertEqual(row['report']['disconnectCleanup']['completedCompositorFrames'],0)
        self.assertFalse(any(r.get('nativeCompositorBinaryCapture') for r in row['records']))
        for key in ('compositor4kVerified','fullDesktopGpuVerified','quickshellGpuVerified','nearNativePerformanceVerified'):
            self.assertFalse(self.value[key])
        for key in ('failed4k','legacy'):
            source=self.value[key]['source']
            self.assertTrue(source['sourceImageUnchanged'])
            self.assertEqual(source['sourceImageSha256Before'],source['sourceImageSha256After'])
        with self.assertRaises(ValueError):validate(row['report'],row['guest'],row['records'],{},row['source'],expected_frames=12)

    def test_new_qemu_preserves_300_frame_legacy_profile_and_cleanup(self):
        row=self.value['legacy'];report=row['report']
        frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}
        value=validate(report,row['guest'],row['records'],frames,row['source'],expected_frames=300)
        self.assertEqual(value,row['audit'])
        self.assertEqual(value['scene']['queued']['framesSubmitted'],300)
        self.assertEqual(value['scene']['queued']['lifecycle']['completedNativeSubmissions'],945)
        self.assertEqual(report['ownedQemuControl']['allocationApertureSlots'],256)
        self.assertEqual(report['disconnectCleanup']['vendorCpuMappedByteLimit'],96*1024*1024)
        self.assertEqual(report['disconnectCleanup']['vendorGpuMappedByteLimit'],256*1024*1024)
        self.assertTrue(report['disconnectCleanup']['driverCleanupVerified'])
        self.assertFalse(value['desktopVerified'])

    def test_artifact_digest_and_corresponding_source_provenance(self):
        receipt=self.value['qemuBuildReceipt']
        self.assertEqual(receipt['revision'],'3e63de34d7c43aec5b48d94bc879fea8be40dedc')
        self.assertEqual(receipt['artifact'],12055250017)
        self.assertEqual(receipt['archiveSha256'],'0daf9f2494fbe32862877c2292be902fc681a4b534aeba22bf71868690c0be96')
        self.assertEqual(receipt['executableSha256'],'dcd422e6a7882af4ede4d6a79915dcfa6b344253b0b008cd6cebb55e62889111')
        self.assertEqual(receipt['correspondingSource']['include/hw/misc/wddm-allocation-map.h']['normalizedSha256'],'74c68499af28135a9d38ffcc26c666fcd7f74ecce35e8819a81098939b0aac5d')
        for key in ('failed4k','legacy'):self.assertEqual(self.value[key]['report']['qemuSha256'],receipt['executableSha256'])

    def test_evidence_is_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'d08ed51a4667d7458b7c95379dc48ed15a7e00dd57a2a71bf47b0fec6e60a7a7')


if __name__=='__main__':unittest.main()
