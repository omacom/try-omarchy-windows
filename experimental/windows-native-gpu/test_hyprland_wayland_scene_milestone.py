"""Retained real Wayland scenes and failure controls; no claimed full desktop acceptance."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_hyprland_wayland_scene import validate
from verify_hyprland_queued_presentation import validate as queued
from verify_hyprland_compositor_capture import fnv


class WaylandScene(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.path=cls.root/'docs/evidence/HYPRLAND-WAYLAND-SCENE-DEVELOPMENT-2026-10-15.json'
        cls.evidence=json.loads(cls.path.read_text())
        cls.runs={run['run']:run for run in cls.evidence['runs']}
        cls.frames={number:{name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in run['rawFramesZlibBase64'].items()}
                    for number,run in cls.runs.items()}

    def audit(self,number=44,records=None,frames=None,guest=None,report=None,count=None):
        run=self.runs[number]
        return validate(run['report'] if report is None else report,run['guestReceipts'] if guest is None else guest,
            run['nativeRecords'] if records is None else records,self.frames[number] if frames is None else frames,
            expected_frames=run['expectedFrames'] if count is None else count,require_changed_content=number>=43)

    def change(self,key,field,value):
        rows=copy.deepcopy(self.runs[44]['nativeRecords'])
        next(row for row in reversed(rows) if key in row)[field]=value
        return rows

    def metadata(self,field,value):
        guest=self.runs[44]['guestReceipts']
        encoded=next(line.split(' ',1)[1] for line in guest.splitlines() if line.startswith('HYPRLAND_SCENE_CLIENTS_HEX '))
        clients=json.loads(bytes.fromhex(encoded)); clients[0][field]=value
        return guest.replace(encoded,json.dumps(clients).encode().hex())

    def redigest(self,data):
        frames=self.frames[44].copy(); frames['compositor-copy-2.raw']=bytes(data)
        rows=copy.deepcopy(self.runs[44]['nativeRecords'])
        next(row for row in rows if row.get('nativeCompositorRingAudited') and row['frame']==300)['fnv1a']=fnv(data)
        next(row for row in rows if row.get('nativeCompositorBinaryCapture') and row['frame']==2)['fnv1a']=fnv(data)
        return frames,rows

    def test_real_scene_and_two_fresh_300_frame_runs(self):
        for number in (42,43,44):
            result=self.audit(number)
            self.assertTrue(result['realWaylandSurfaceComposedVerified'])
            self.assertTrue(result['realWaylandClientClosedVerified'])
            self.assertEqual(result['queued']['framesSubmitted'],self.runs[number]['expectedFrames'])
            self.assertEqual([frame['knownBackgroundPixelsVerified'] for frame in result['queued']['frames']],[7168,7168])
            self.assertEqual([frame['terminalBackgroundPixelsVerified'] for frame in result['scopedScenePixelAudit']],[8960,8960])
            for key in ('applicationGpuAccelerationVerified','fullTextGlyphOracleVerified','fullOmarchySceneVerified',
                        'desktopVerified','normalLauncherIntegrationVerified','longRunStabilityVerified','compositor4kVerified','nearNativePerformanceVerified'):
                self.assertFalse(result[key])
        for number in (43,44):
            self.assertEqual(self.audit(number)['queued']['ordinaryFramesWithoutCpuReadback'],298)
            self.assertGreater(self.runs[number]['report']['disconnectCleanup']['completedNativeSubmissions'],900)

    def test_both_final_display_observations_remain_unverified(self):
        for number in (43,44): self.assertFalse(self.audit(number)['queued']['lastFrameDeferredDisplayVerified'])

    def test_font_only_control_is_not_an_app_scene(self):
        run=self.runs[37]
        self.assertTrue(queued(run['report'],run['guestReceipts'],run['nativeRecords'],self.frames[37],(17,68,119))['actualCompositorQueuedSubmissionVerified'])
        with self.assertRaises(ValueError): self.audit(37)

    def test_all_failed_controls_remain_rejected(self):
        for number in (38,40,41):
            with self.subTest(run=number),self.assertRaises(ValueError): self.audit(number)
        for number in (40,41):
            self.assertGreater(self.runs[number]['report']['disconnectCleanup']['completedNativeSubmissions'],1000)

    def test_exact_rolling_profile_required(self):
        for field,value in (('version',4),('frames',2),('commandLimit',256),('pendingCommands',17),
                            ('vendorObjectLimit',0),('objectLimit',2048),('requestLimit',50000),('openGlProfileNegotiated',1)):
            with self.subTest(field=field),self.assertRaises(ValueError): self.audit(records=self.change('openGlProfileNegotiated',field,value))

    def test_expected_frame_count_independent_of_report(self):
        with self.assertRaises(ValueError): self.audit(count=12)

    def test_hidden_foreign_or_wrong_geometry_client_rejected(self):
        for field,value in (('mapped',False),('hidden',True),('visible',False),('xwayland',True),
                            ('at',[0,0]),('size',[1,1]),('class','other'),('pid',0)):
            with self.subTest(field=field),self.assertRaises(ValueError): self.audit(guest=self.metadata(field,value))

    def test_malformed_or_duplicate_metadata_rejected(self):
        guest=self.runs[44]['guestReceipts']
        line=next(line for line in guest.splitlines() if line.startswith('HYPRLAND_SCENE_CLIENTS_HEX '))
        for changed in (guest+line+'\n',guest.replace(line,'HYPRLAND_SCENE_CLIENTS_HEX 7b')):
            with self.assertRaises(ValueError): self.audit(guest=changed)

    def test_readiness_fonts_and_child_close_required(self):
        guest=self.runs[44]['guestReceipts']
        for text in ('HYPRLAND_SCENE_READY ','HYPRLAND_SCENE_FONT index=2 ','HYPRLAND_SCENE_CLOSED '):
            with self.subTest(text=text),self.assertRaises(ValueError): self.audit(guest=guest.replace(text,'REMOVED '))
        with self.assertRaises(ValueError): self.audit(guest=guest+'HYPRLAND_SCENE_FAILURE test\n')

    def test_unretired_copy_or_missing_thread_join_rejected(self):
        for key,field,value in (('nativeCompositorRingInput','sourceCopyRetired',False),
                               ('nativeCompositorRingStopped','uiThreadJoined',False)):
            with self.assertRaises(ValueError): self.audit(records=self.change(key,field,value))

    def test_ordinary_frame_cpu_readback_rejected(self):
        rows=copy.deepcopy(self.runs[44]['nativeRecords'])
        next(row for row in rows if row.get('nativeCompositorRingFrame') and row['frame']==200)['cpuReadback']=True
        with self.assertRaises(ValueError): self.audit(records=rows)

    def test_modified_pixels_outside_scene_rejected(self):
        frames=self.frames[44].copy(); data=bytearray(frames['compositor-copy-2.raw']); data[0]^=1
        frames['compositor-copy-2.raw']=bytes(data)
        with self.assertRaises(ValueError): self.audit(frames=frames)

    def test_independent_terminal_color_not_just_matching_native_digest(self):
        data=bytearray(self.frames[44]['compositor-copy-2.raw']); data[(285*640+100)*4]^=1
        frames,rows=self.redigest(data)
        with self.assertRaisesRegex(ValueError,'terminal background'): self.audit(frames=frames,records=rows)

    def test_palette_missing_even_with_matching_native_digest_rejected(self):
        data=bytearray(self.frames[44]['compositor-copy-2.raw'])
        for y in range(120,320):
            for x in range(80,560):
                offset=(y*640+x)*4
                if tuple(data[offset+k] for k in (2,1,0))==(235,70,70): data[offset:offset+3]=bytes((32,24,16))
        frames,rows=self.redigest(data)
        with self.assertRaisesRegex(ValueError,'scene colors'): self.audit(frames=frames,records=rows)

    def test_unchanged_animated_capture_rejected(self):
        frames,rows=self.redigest(self.frames[44]['compositor-copy-1.raw'])
        with self.assertRaisesRegex(ValueError,'did not change'): self.audit(frames=frames,records=rows)

    def test_published_evidence_and_scene_assets_pinned(self):
        self.assertEqual(hashlib.sha256(self.path.read_bytes()).hexdigest(),'ba67b9fa015a5400f9b1529c2b70430c76f54181babf20dbd657c40c7b7d902f')
        self.assertEqual(hashlib.sha256((self.root/'docs/images/hyprland-wayland-scene-frame-300-2026-10-15.png').read_bytes()).hexdigest(),
                         '62d8661cf27a60a1fbd1e8e27f67b32c5480d938c66b35734bb09b87b92263cb')
        for name,digest in self.evidence['publicSceneAssetsSha256'].items():
            self.assertEqual(hashlib.sha256((self.root/'experimental/windows-native-gpu'/name).read_bytes()).hexdigest(),digest)
        for number in (43,44):
            inputs=self.runs[number]['inputs']
            for public,packed in (('wayland_scene.sh','desktop-scene.sh'),('wayland_scene.ini','foot.ini'),
                                  ('compositor-fonts.conf','etc/fonts/fonts.conf')):
                self.assertEqual(inputs['manifest']['replacedFiles'][packed]['sha256'],self.evidence['publicSceneAssetsSha256'][public])
            for name in ('driver_bridge.cpp','driver_wire.h','opengl_profile.h','linux_ioctl_bridge.cpp',
                         'hyprland_startup_probe.c','hyprland_queue_probe.h'):
                path='experimental/windows-native-gpu/'+name
                # Compare the retained build inputs to each other. Current
                # source can improve without rewriting historical evidence.
                self.assertEqual(inputs['repositorySourceSha256'][path],inputs['guestBuildInputs']['files'][name])
                if name.endswith(('.cpp','.h')):
                    self.assertEqual(inputs['repositorySourceSha256'][path],inputs['nativeBuildReceipt']['sources'][path])


if __name__=='__main__': unittest.main()
