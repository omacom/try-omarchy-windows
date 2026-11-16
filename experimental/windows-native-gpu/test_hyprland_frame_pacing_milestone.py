"""Real paced compositor captures, delayed display proof, and retained failed controls."""
import base64
import copy
import json
from pathlib import Path
import unittest
import zlib
from presentmon_evidence import correlate_presentmon
from verify_hyprland_compositor_submission import validate,validate_display
from verify_hyprland_native_window import validate as validate_instant


class CompositorFramePacing(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=Path(__file__).resolve().parents[2]/'docs/evidence/HYPRLAND-FRAME-PACING-DEVELOPMENT-2026-10-15.json'
        cls.evidence=json.loads(path.read_text())
        cls.runs={run['run']:run for run in cls.evidence['runs']}
        cls.frames={number:{name:zlib.decompress(base64.b64decode(value,validate=True)) for name,value in run['rawFramesZlibBase64'].items()}
            for number,run in cls.runs.items()}

    def audit(self,number=30,records=None,frames=None,display=True,verifier=None):
        run=self.runs[number]
        verifier=verifier or (validate_display if display else validate)
        return verifier(run['report'],run['guestReceipts'],records if records is not None else run['nativeRecords'],
            frames if frames is not None else self.frames[number],tuple(bytes.fromhex(run['expectedBackgroundRGB'])))

    def change(self,key,field,value):
        records=copy.deepcopy(self.runs[30]['nativeRecords'])
        next(row for row in reversed(records) if key in row)[field]=value
        return records

    def test_two_fresh_changed_background_runs(self):
        for number in (30,31):
            result=self.audit(number)
            self.assertTrue(result['actualCompositorDeferredDisplayVerified'])
            self.assertEqual([row['statisticsPresentCount'] for row in result['observations']],[1,2])
            self.assertFalse(result['instantDisplayStatisticsComplete'])
            self.assertLess(result['submission']['diagnosticInterPresentMilliseconds'],1000)
            for flag in ('normalLauncherIntegrationVerified','displayStabilityVerified','fullScenePixelCorrectnessVerified',
                         'desktopVerified','nearNativePerformanceVerified'):
                self.assertFalse(result[flag])

    def test_submission_does_not_claim_display(self):
        result=self.audit(display=False)
        self.assertTrue(result['actualCompositorSubmissionVerified'])
        self.assertFalse(result['windowsPresentationVerified'])
        self.assertFalse(result['perFrameDisplayStatisticsWaitUsed'])
        self.assertFalse(result['perFrameDiagnosticHoldUsed'])

    def test_instant_auditor_still_rejects_delayed_observations(self):
        with self.assertRaises(ValueError): self.audit(verifier=validate_instant)

    def test_old_controls_do_not_prove_new_submission_policy(self):
        for number in (25,26,27,28,29,32):
            with self.subTest(run=number),self.assertRaises(ValueError): self.audit(number)

    def test_stale_final_display_count_is_not_display_success(self):
        records=self.change('nativeCompositorShutdownStatistics','statisticsPresentCount',1)
        self.assertTrue(self.audit(records=records,display=False)['actualCompositorSubmissionVerified'])
        with self.assertRaises(ValueError): self.audit(records=records)

    def test_stale_first_display_count_rejected(self):
        records=self.change('nativeCompositorFrameSlotReady','statisticsPresentCount',0)
        with self.assertRaises(ValueError): self.audit(records=records)

    def test_nonadvancing_display_timing_rejected(self):
        snapshot=next(row for row in self.runs[30]['nativeRecords'] if row.get('frame')==2 and 'nativeCompositorFrameSlotReady' in row)
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorShutdownStatistics','syncQpc',snapshot['syncQpc']))

    def test_unretired_queue_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeSharedTexturePresented','presentQueueFenceObserved',1))

    def test_unclosed_frame_slot_handle_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeSharedTexturePresenterReleased','frameLatencyHandleClosed',False))

    def test_foreign_frame_slot_identity_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorFrameSlotReady','presentProcessId',1))

    def test_modified_pixel_outside_background_roi_rejected(self):
        frames=self.frames[30].copy(); pixels=bytearray(frames['compositor-copy-2.raw']); pixels[0]^=1
        frames['compositor-copy-2.raw']=bytes(pixels)
        with self.assertRaises(ValueError): self.audit(frames=frames)

    def test_hidden_final_window_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorShutdownStatistics','windowVisible',False))

    def test_missing_final_display_observation_rejected(self):
        records=[row for row in self.runs[30]['nativeRecords'] if 'nativeCompositorShutdownStatistics' not in row]
        with self.assertRaises(ValueError): self.audit(records=records)

    def test_actual_trace_correlates_second_frame_only(self):
        trace=self.evidence['trace']
        rows=[row for row in self.runs[29]['nativeRecords'] if 'nativeSharedTexturePresented' in row]
        result=correlate_presentmon(trace['csv'],rows)
        self.assertFalse(result['etwFramesDisplayedVerified'])
        self.assertFalse(result['frames'][0]['etwDisplayed'])
        self.assertTrue(result['frames'][1]['etwDisplayed'])
        self.assertFalse(rows[1]['nativeSharedTexturePresented'])


if __name__=='__main__': unittest.main()
