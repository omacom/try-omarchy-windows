"""Retained real twelve-frame submissions and adversarial evidence mutations."""
import base64
import copy
import json
from pathlib import Path
import unittest
import zlib
from verify_hyprland_queued_presentation import validate


class QueuedPresentation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path=Path(__file__).resolve().parents[2]/'docs/evidence/HYPRLAND-QUEUED-PRESENTATION-DEVELOPMENT-2026-10-15.json'
        cls.evidence=json.loads(path.read_text())
        cls.runs={run['run']:run for run in cls.evidence['runs']}
        cls.frames={number:{name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in run['rawFramesZlibBase64'].items()}
                    for number,run in cls.runs.items()}

    def audit(self,number=36,records=None,frames=None,guest=None):
        run=self.runs[number]
        return validate(run['report'],run['guestReceipts'] if guest is None else guest,
            run['nativeRecords'] if records is None else records,self.frames[number] if frames is None else frames,(0x11,0x44,0x77))

    def change(self,key,field,value):
        rows=copy.deepcopy(self.runs[36]['nativeRecords'])
        next(row for row in reversed(rows) if key in row)[field]=value
        return rows

    def test_two_fresh_repeated_submissions(self):
        for number in (35,36):
            result=self.audit(number)
            self.assertTrue(result['actualCompositorQueuedSubmissionVerified'])
            self.assertEqual(result['ordinaryFramesWithoutCpuReadback'],10)
            self.assertEqual(result['auditCopies'],2)
            self.assertEqual([frame['knownBackgroundPixelsVerified'] for frame in result['frames']],[71680,71680])
            for key in ('allFramesDisplayedVerified','normalLauncherIntegrationVerified','sustainedRenderingVerified',
                        'fullScenePixelCorrectnessVerified','desktopVerified','compositor4kVerified','nearNativePerformanceVerified'):
                self.assertFalse(result[key])

    def test_last_frame_display_observations_differ(self):
        self.assertFalse(self.audit(35)['lastFrameDeferredDisplayVerified'])
        self.assertTrue(self.audit(36)['lastFrameDeferredDisplayVerified'])

    def test_failed_controls_remain_rejected(self):
        for number in (33,34):
            with self.subTest(run=number),self.assertRaises(ValueError): self.audit(number)

    def test_late_banner_invalidates_original_top_reference(self):
        for number in (35,36):
            pixels=self.frames[number]['compositor-copy-2.raw']
            self.assertTrue(self.runs[number]['originalTopRegionAuditRejected'])
            self.assertTrue(any(tuple(pixels[(y*640+x)*4+k] for k in (2,1,0))!=(0x11,0x44,0x77)
                                for y in range(32,128) for x in range(640)))

    def test_ordinary_frame_readback_rejected(self):
        rows=copy.deepcopy(self.runs[36]['nativeRecords'])
        next(row for row in rows if row.get('nativeCompositorRingFrame') and row['frame']==2)['cpuReadback']=True
        with self.assertRaises(ValueError): self.audit(records=rows)

    def test_unretired_input_copy_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorRingInput','inputCopyFenceObserved',1))

    def test_wrong_slot_or_ownership_rejected(self):
        for field,value in (('slot',3),('sourceTicket',0),('sourceReturnedToCommon',False)):
            with self.subTest(field=field),self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorRingInput',field,value))

    def test_false_frame_receipt_rejected(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorRingFrame','nativeCompositorRingFrame',False))

    def test_sequence_or_foreign_chain_rejected(self):
        for field,value in (('frame',11),('lastPresentCount',11),('presentSwapChain',1),('presentHresult',1)):
            with self.subTest(field=field),self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorWindowSubmitted',field,value))

    def test_bounded_slot_wait_required(self):
        with self.assertRaises(ValueError): self.audit(records=self.change('nativeCompositorFrameSlotReady','maximumFrameLatency',3))

    def test_thread_and_handle_cleanup_required(self):
        for key,field in (('nativeCompositorRingStopped','uiThreadJoined'),('nativeCompositorRingStopped','inputEventClosed'),
                          ('nativeCompositorRingStopped','workHandleClosed'),('nativeSharedTexturePresenterReleased','gpuWorkRetired')):
            with self.subTest(field=field),self.assertRaises(ValueError): self.audit(records=self.change(key,field,False))

    def test_pixel_mutation_outside_roi_rejected_by_digest(self):
        frames=self.frames[36].copy(); data=bytearray(frames['compositor-copy-2.raw']); data[0]^=1
        frames['compositor-copy-2.raw']=bytes(data)
        with self.assertRaises(ValueError): self.audit(frames=frames)

    def test_incomplete_guest_target_rejected(self):
        with self.assertRaises(ValueError): self.audit(guest=self.runs[36]['guestReceipts'].replace('frames=12 target=12 failed=0','frames=11 target=12 failed=1'))

    def test_stale_foreign_or_hidden_observation_does_not_accept_display(self):
        for field,value in (('statisticsPresentCount',4),('presentProcessId',1),('windowVisible',False),('syncQpc',1)):
            with self.subTest(field=field):
                result=self.audit(records=self.change('nativeCompositorRingDisplayObservation',field,value))
                self.assertTrue(result['actualCompositorQueuedSubmissionVerified'])
                self.assertFalse(result['lastFrameDeferredDisplayVerified'])


if __name__=='__main__': unittest.main()
