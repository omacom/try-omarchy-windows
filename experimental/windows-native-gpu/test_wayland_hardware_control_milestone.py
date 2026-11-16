"""Preserve the hardware-only shell failure without relabeling composition as a GPU desktop."""
import base64
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_hyprland_compositor_capture import fnv


class WaylandHardwareControl(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path=Path(__file__).resolve().parents[2]/'docs/evidence/NORMAL-OMARCHY-WAYLAND-HARDWARE-CONTROL-2026-10-16.json'
        cls.value=json.loads(cls.path.read_text())
        unpack=lambda key:zlib.decompress(base64.b64decode(cls.value[key],validate=True))
        cls.report=json.loads(unpack('reportZlibBase64'))
        cls.records=json.loads(unpack('nativeRecordsZlibBase64'))
        cls.guest=unpack('guestReceiptsZlibBase64').decode()

    def test_wayland_build_has_only_the_d3d12_gallium_driver(self):
        options={row['name']:row['value'] for row in self.value['mesaBuild']['options']}
        self.assertEqual(options['gallium-drivers'],['d3d12'])
        self.assertEqual(options['platforms'],['wayland'])
        self.assertFalse(self.value['mesaBuild']['optimizationFlagsChanged'])
        self.assertTrue(self.value['source']['manifest']['mesaWaylandGpuProbe'])
        self.assertFalse(self.value['source']['manifest']['qtQuickSoftwareProbe'])
        self.assertNotIn('qtQuickSoftware=true',self.guest)

    def test_shell_hardware_initialization_failure_remains_a_failure(self):
        self.assertIn('libEGL debug: using driver d3d12',self.guest)
        self.assertIn('LINUX_BRIDGE openFailed errno=5',self.guest)
        self.assertIn('Failed to initialize graphics backend for OpenGL.',self.guest)
        line=next(line for line in self.guest.splitlines() if line.startswith('OMARCHY_DESKTOP_LAYERS_HEX '))
        layers=json.loads(bytes.fromhex(line.split()[1]))['WDDM-1']['levels']
        self.assertTrue(all(not entries for entries in layers.values()))
        for key in ('quickshellGpuVerified','fullDesktopGpuVerified','desktop4kVerified',
                    'normalLauncherIntegrationVerified','nearNativePerformanceVerified'):
            self.assertFalse(self.value[key])

    def test_compositor_captures_and_cleanup_do_not_prove_a_4k_desktop(self):
        self.assertTrue(self.report['liveNvidiaLinuxUmdLoaded'])
        self.assertIn('HYPRLAND_HOST_QUEUE_END frames=12 target=12 failed=0',self.guest)
        self.assertIn('NORMAL_ROOT_GRAPHICS_EXIT result=0 normalOmarchySession=false',self.guest)
        self.assertEqual(self.report['disconnectCleanup']['completedNativeSubmissions'],264)
        vm=self.report['ownedQemuControl']
        for kind in ('fence','allocation','allocationSlot'):
            self.assertGreater(vm[kind+'MappingsCreated'],0)
            self.assertEqual(vm[kind+'MappingsCreated'],vm[kind+'UnmapAcknowledgements'])
            self.assertEqual(vm[kind+'MappingsReleasedAfterVmExit'],0)
        captures=[row for row in self.records if row.get('nativeCompositorBinaryCapture')]
        self.assertEqual(len(captures),2)
        for row in captures:
            self.assertEqual((row['width'],row['height']),(1280,720))
            data=zlib.decompress(base64.b64decode(self.value['rawFramesZlibBase64'][row['file']],validate=True))
            self.assertEqual(len(data),1280*720*4)
            self.assertEqual(fnv(data),row['fnv1a'])

    def test_control_evidence_remains_immutable(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'aa8c6df754496b253b02353a0c2ac1018dc2ad66c532ead92f2d3bd41e79fedb')


if __name__=='__main__':unittest.main()
