"""Actual native-window result and retained failed display controls."""
import base64
import copy
import json
from pathlib import Path
import unittest
import zlib
from verify_hyprland_native_window import validate


class NativeCompositorWindow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=Path(__file__).resolve().parents[2]/'docs/evidence/HYPRLAND-NATIVE-WINDOW-DEVELOPMENT-2026-10-15.json'
        cls.runs=json.loads(path.read_text())['runs']
        cls.frames=[{name:zlib.decompress(base64.b64decode(value,validate=True)) for name,value in run['rawFramesZlibBase64'].items()} for run in cls.runs]

    def audit(self,index=0,records=None,frames=None,report=None):
        run=self.runs[index]
        return validate(report if report is not None else run['report'],run['guestReceipts'],
            records if records is not None else run['nativeRecords'],frames if frames is not None else self.frames[index],
            tuple(bytes.fromhex(run['expectedBackgroundRGB'])))

    def change_present(self,field,value):
        records=copy.deepcopy(self.runs[0]['nativeRecords'])
        presented=[row for row in records if 'nativeSharedTexturePresented' in row]
        presented[1][field]=value
        return records

    def test_actual_window_pass_has_precise_scope(self):
        result=self.audit()
        self.assertTrue(result['actualCompositorNativeWindowPresentationVerified'])
        self.assertEqual(result['statisticsPresentCounts'],[1,2])
        self.assertEqual([row['knownBackgroundPixelsVerified'] for row in result['backBufferReadback']['frames']],[133120,133120])
        for flag in ('normalLauncherIntegrationVerified','displayStabilityVerified','fullScenePixelCorrectnessVerified',
                     'desktopVerified','nearNativePerformanceVerified'):
            self.assertFalse(result[flag])

    def test_actual_failed_repeat_stays_rejected(self):
        with self.assertRaises(ValueError): self.audit(index=1)

    def test_capture_only_control_cannot_prove_display(self):
        with self.assertRaises(ValueError): self.audit(index=2)

    def test_stale_dxgi_statistics_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change_present('statisticsPresentCount',1))

    def test_nonadvancing_display_timing_rejected(self):
        rows=self.runs[0]['nativeRecords']
        first=next(row for row in rows if row.get('nativeSharedTexturePresented') is True)
        with self.assertRaises(ValueError): self.audit(records=self.change_present('syncQpc',first['syncQpc']))

    def test_foreign_presenter_identity_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change_present('presentProcessId',1))

    def test_unretired_presentation_queue_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change_present('presentQueueFenceObserved',1))

    def test_wrong_backbuffer_hash_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change_present('backBufferFnv1a',0))

    def test_modified_backbuffer_bytes_rejected(self):
        frames=self.frames[0].copy()
        pixels=bytearray(frames['compositor-copy-2.raw']); pixels[0]^=1
        frames['compositor-copy-2.raw']=bytes(pixels)
        with self.assertRaises(ValueError): self.audit(frames=frames)

    def test_missing_window_release_rejected(self):
        records=[row for row in self.runs[0]['nativeRecords'] if 'nativeSharedTexturePresenterReleased' not in row]
        with self.assertRaises(ValueError): self.audit(records=records)

    def test_window_hidden_before_display_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change_present('windowVisible',False))


if __name__=='__main__': unittest.main()
