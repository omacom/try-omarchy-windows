"""Check real retained compositor captures and reject invalid capture evidence."""
import base64
import copy
import json
from pathlib import Path
import unittest
import zlib
from verify_hyprland_compositor_capture import validate


class CompositorCapture(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=Path(__file__).resolve().parents[2]/'docs/evidence/HYPRLAND-COMPOSITOR-CAPTURE-DEVELOPMENT-2026-10-15.json'
        cls.runs=json.loads(path.read_text())['runs']
        cls.frames=[{name:zlib.decompress(base64.b64decode(value,validate=True)) for name,value in run['rawFramesZlibBase64'].items()} for run in cls.runs]

    def audit(self,run=None,frames=None,records=None,rgb=None):
        run=run or self.runs[0]
        return validate(run['report'],run['guestReceipts'],records if records is not None else run['nativeRecords'],
            frames if frames is not None else self.frames[0],rgb if rgb is not None else tuple(bytes.fromhex(run['expectedBackgroundRGB'])))

    def test_actual_changed_background_runs(self):
        for run,frames in zip(self.runs,self.frames):
            with self.subTest(run=run['run']):
                result=self.audit(run=run,frames=frames)
                self.assertTrue(result['actualCompositorReadbackVerified'])
                self.assertEqual([frame['knownBackgroundPixelsVerified'] for frame in result['frames']],[133120,133120])
                for flag in ('fullScenePixelCorrectnessVerified','windowsPresentationVerified','desktopVerified','nearNativePerformanceVerified'):
                    self.assertFalse(result[flag])

    def test_wrong_independent_reference_rejected(self):
        with self.assertRaisesRegex(ValueError,'background mismatch'):
            self.audit(rgb=(51,85,119))

    def test_stale_other_guest_pixels_rejected(self):
        with self.assertRaises(ValueError):
            self.audit(frames=self.frames[1])

    def test_changed_pixel_outside_reference_regions_rejected(self):
        frames=self.frames[0].copy()
        changed=bytearray(frames['compositor-copy-1.raw']); changed[0]^=1
        frames['compositor-copy-1.raw']=bytes(changed)
        with self.assertRaisesRegex(ValueError,'digest mismatch'):
            self.audit(frames=frames)

    def test_truncated_readback_rejected(self):
        frames=self.frames[0].copy(); frames['compositor-copy-2.raw']=frames['compositor-copy-2.raw'][:-4]
        with self.assertRaises(ValueError):
            self.audit(frames=frames)

    def test_unretired_producer_fence_rejected(self):
        records=copy.deepcopy(self.runs[0]['nativeRecords'])
        for row in records:
            if row.get('nativeCompositorCaptured') is True:
                row['producerFenceObserved']=row['producerFenceTarget']-1
                break
        with self.assertRaises(ValueError):
            self.audit(records=records)

    def test_missing_native_release_rejected(self):
        records=copy.deepcopy(self.runs[0]['nativeRecords'])
        records.remove(next(row for row in records if row.get('nativeCompositorCaptureReleased') is True))
        with self.assertRaises(ValueError):
            self.audit(records=records)

    def test_reordered_frames_rejected(self):
        records=copy.deepcopy(self.runs[0]['nativeRecords'])
        for row in records:
            if row.get('nativeCompositorCaptured') is True:
                row['frame']=2
                break
        with self.assertRaises(ValueError):
            self.audit(records=records)

    def test_presentation_claim_rejected(self):
        records=copy.deepcopy(self.runs[0]['nativeRecords'])
        for row in records:
            if row.get('nativeCompositorCaptured') is True:
                row['windowsPresentation']=True
                break
        with self.assertRaises(ValueError):
            self.audit(records=records)


if __name__=='__main__': unittest.main()
