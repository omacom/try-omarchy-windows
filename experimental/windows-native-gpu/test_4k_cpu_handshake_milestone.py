"""Exact hardware capacity receipt, retained failure, and unchanged legacy rendering."""
import base64
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_normal_root_graphics import validate


def unpack(row,key):
    return zlib.decompress(base64.b64decode(row[key],validate=True))


class DesktopCpuHandshake(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root=Path(__file__).resolve().parents[2]
        cls.path=root/'docs/evidence/NORMAL-OMARCHY-4K-CPU-CAPACITY-2026-10-16.json'
        cls.value=json.loads(cls.path.read_text())
        cls.report=json.loads(unpack(cls.value,'reportZlibBase64'))
        cls.records=json.loads(unpack(cls.value,'nativeRecordsZlibBase64'))
        cls.guest=unpack(cls.value,'guestReceiptsZlibBase64').decode().replace('\r\n','\n')
        cls.regression_path=root/'docs/evidence/NORMAL-OMARCHY-CPU-HANDSHAKE-REGRESSION-2026-10-16.json'
        cls.regression=json.loads(cls.regression_path.read_text())

    def test_exact_kernel_and_native_capacity_receipts(self):
        self.assertIn('APERTURE_CAPACITY version=1 flags=1 cpuByteLimit=201326592 exact=true\n',self.guest)
        self.assertIn('kernelCpuCapacityVerified=true profile=7 cpuByteLimit=201326592 cpuSlots=256 fenceSlots=256\n',self.guest)
        profiles=[r for r in self.records if r.get('openGlProfileNegotiated')]
        self.assertEqual(len(profiles),1)
        self.assertEqual(profiles[0]['version'],7)
        self.assertEqual(profiles[0]['cpuMappedByteLimit'],192*1024*1024)
        self.assertTrue(profiles[0]['kernelCapacityRequired'])
        self.assertEqual(self.report['disconnectCleanup']['vendorCpuMappedByteLimit'],192*1024*1024)

    def test_real_kernel_boundary_and_malformed_contract_controls(self):
        self.assertIn('APERTURE_CONTRACT_PASS checks=1101 activeLeases=0 activeCpuBytes=0\n',self.guest)
        logs=self.value['validationLogs']
        policy=zlib.decompress(base64.b64decode(logs['profile7-controls-v2/build.log'],validate=True)).decode()
        self.assertIn('BROKER_CPU_CAPACITY exact192MiB=true ownKernelQuery=true missingAndMalformedRejected=true legacyUnchanged=true kernel=mock gpu=false',policy)
        self.assertIn('PASS: WDDM wire ownership',policy)

    def test_hardware_attempt_has_real_4k_inputs_and_no_software_qt_override(self):
        source=self.value['source'];manifest=source['manifest']
        self.assertEqual(manifest['desktopGeometryProbe'],[3840,2160])
        self.assertEqual(manifest['kernelCpuCapacityProfile'],7)
        self.assertFalse(manifest['qtQuickSoftwareProbe'])
        self.assertTrue(manifest['mesaWaylandGpuProbe'])
        self.assertNotIn('qtQuickSoftware=true',self.guest)
        self.assertTrue(self.report['liveNvidiaLinuxUmdLoaded'])
        self.assertTrue(source['sourceImageUnchanged'])
        self.assertEqual(source['sourceImageSha256Before'],source['sourceImageSha256After'])
        self.assertEqual(source['kernelCapacityBuildReceipt']['moduleSha256'],manifest['files']['kernel/wddm-aperture.ko']['sha256'])

    def test_separate_slot_capacity_rejects_after_cpu_byte_guard_passes(self):
        clean=self.report['disconnectCleanup'];qemu=self.report['ownedQemuControl']
        self.assertEqual(clean['peakVendorCpuBytes'],91758592)
        self.assertLess(clean['peakVendorCpuBytes']+self.value['requestedCpuMappingBytes'],clean['vendorCpuMappedByteLimit'])
        self.assertEqual(clean['vendorCpuSlotQuotaRejections'],1)
        self.assertEqual(clean['vendorCpuSlotLimit'],256)
        occupied=qemu['allocationSlotMappingsCreated']-qemu['allocationSlotUnmapAcknowledgements']
        self.assertEqual(occupied,self.value['occupiedSlotsAtFailure'])
        self.assertEqual(occupied,226)
        self.assertLess(256-occupied,self.value['requestedApertureSlots'])
        self.assertIn('ioctlFailed nr=37 errno=12\n',self.guest)
        self.assertIn('NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=6',self.guest)

    def test_native_cleanup_after_abort_is_not_a_verified_desktop(self):
        self.assertEqual(self.value['auditedFrameCount'],0)
        self.assertEqual(self.report['disconnectCleanup']['completedCompositorFrames'],0)
        self.assertFalse(any(r.get('nativeCompositorBinaryCapture') for r in self.records))
        for key in ('compositor4kVerified','fullDesktopGpuVerified','quickshellGpuVerified','nearNativePerformanceVerified'):
            self.assertFalse(self.value[key])
        self.assertTrue(self.report['disconnectCleanup']['driverCleanupVerified'])
        self.assertGreater(self.report['ownedQemuControl']['fenceMappingsReleasedAfterVmExit'],0)
        with self.assertRaises(ValueError):
            validate(self.report,self.guest,self.records,{},self.value['source'],expected_frames=12)

    def test_default_kernel_budget_and_300_frame_version5_regression(self):
        row=self.regression;report=json.loads(unpack(row,'reportZlibBase64'))
        guest=unpack(row,'guestReceiptsZlibBase64').decode().replace('\r\n','\n')
        records=json.loads(unpack(row,'nativeRecordsZlibBase64'))
        frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()}
        value=validate(report,guest,records,frames,row['source'],expected_frames=300)
        self.assertEqual(value,row['audit'])
        self.assertIn('APERTURE_CAPACITY version=1 flags=0 cpuByteLimit=100663296 exact=true\n',guest)
        self.assertEqual(report['disconnectCleanup']['vendorCpuMappedByteLimit'],96*1024*1024)
        self.assertEqual(value['scene']['queued']['framesSubmitted'],300)
        self.assertEqual(value['scene']['queued']['lifecycle']['completedNativeSubmissions'],948)
        self.assertFalse(value['desktopVerified'])

    def test_retained_failed_control_is_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'49442755f2648e5e85cde9acf65cc11c724bbb9ccce539b13b0262386b02c428')


if __name__=='__main__':unittest.main()
