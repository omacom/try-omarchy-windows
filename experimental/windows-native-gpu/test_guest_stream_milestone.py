"""Reject false stream milestones using retained real-run receipts as a baseline."""
import copy
import json
from pathlib import Path
import unittest
from verify_guest_stream import validate_receipts
from test_owned_runtime_qemu import async_submissions_complete, hw_queue_signals_complete


class GuestStreamMilestone(unittest.TestCase):
    def setUp(self):
        fixture = Path(__file__).resolve().parents[2]/'docs/evidence/GUEST-STREAM-2026-10-11.json'
        proof = json.loads(fixture.read_text())['proof']
        self.d, self.guest, self.r, self.frames = (proof['report'], '\n'.join(proof['guestLines']),
                                                  proof['receipts'], proof['oracleFrames'])

    def check(self):
        return validate_receipts(self.d,self.guest,self.r,self.frames)

    def test_actual_receipts_pass_with_scoped_present_acceptance(self):
        self.assertTrue(self.check())

    def test_old_diagnostics_keep_their_original_lifetime_limits(self):
        d=self.d; c=d['disconnectCleanup']
        self.assertFalse(async_submissions_complete(d['guestCommandEnqueues'],d['nativeAsyncSubmissionAcceptances'],d['nativeAsyncSubmissionRetirements'],c))
        self.assertFalse(hw_queue_signals_complete(d['guestHwQueueSignals'],d['nativeHwQueueSignalReturns'],d['nativeHwQueueSignalAcceptances'],d['nativeHwQueueSignalRetirements'],c))

    def test_corrupted_native_retirement_rejected(self):
        for key in ('nativeAsyncSubmissionRetirements','nativeHwQueueSignalRetirements'):
            with self.subTest(key=key):
                saved=copy.deepcopy(self.d[key]); self.d[key][-1]['observed']=0
                with self.assertRaises(ValueError): self.check()
                self.d[key]=saved

    def test_missing_or_reordered_frame_rejected(self):
        for action in ('missing','reordered','duplicate'):
            with self.subTest(action=action):
                saved=copy.deepcopy(self.r); copies=[r for r in self.r if 'guestStreamCopy' in r]
                if action=='missing': self.r.remove(copies[-1])
                elif action=='reordered': copies[8]['frame']=10
                else: self.r.append(copy.deepcopy(copies[-1]))
                with self.assertRaises(ValueError): self.check()
                self.r=saved

    def test_intermediate_readback_or_false_audit_rejected(self):
        for key,value in (('cpuReadback',True),('auditRound',1),('fnv1a',42)):
            with self.subTest(key=key):
                r=next(r for r in self.r if r.get('guestStreamCopy') is True and r['frame']==2)
                old=r[key]; r[key]=value
                with self.assertRaises(ValueError): self.check()
                r[key]=old

    def test_unsafe_handoff_rejected(self):
        r=next(r for r in self.r if 'nativeSharedTextureHandoff' in r)
        for key,value in (('pendingGuestCommands',1),('returnedState','RENDER_TARGET'),('observed',0),('target',True)):
            with self.subTest(key=key):
                old=r[key]; r[key]=value
                with self.assertRaises(ValueError): self.check()
                r[key]=old

    def test_peak_limit_and_counter_types_rejected(self):
        for key,value in (('peakPendingNativeSubmissions',17),('peakPendingHwQueueSignals',17),('nativeSubmissionAttempts',True),
                          ('peakVendorCpuBytes',96*1024*1024+1),('peakVendorMappedPages',65537),('peakVendorAllocationObjects',129)):
            with self.subTest(key=key):
                old=self.d['disconnectCleanup'][key]; self.d['disconnectCleanup'][key]=value
                with self.assertRaises(ValueError): self.check()
                self.d['disconnectCleanup'][key]=old

    def test_present_acceptance_never_proves_all_frames_displayed(self):
        r=next(r for r in self.r if 'guestStreamPresent' in r)
        for key,value in (('presentHresult',1),('allFramesDisplayedVerified',True),('lastPresentCount',0),('presentCallQpcBefore',True)):
            with self.subTest(key=key):
                old=r[key]; r[key]=value
                with self.assertRaises(ValueError): self.check()
                r[key]=old

    def test_missing_completion_or_bad_pixel_oracle_rejected(self):
        self.guest=self.guest.replace('GPU_STREAM_TEST_COMPLETE verified=true','GPU_STREAM_TEST_COMPLETE verified=false')
        with self.assertRaises(ValueError): self.check()
        self.setUp(); self.frames[-1]['fnv1a'] ^= 1
        with self.assertRaises(ValueError): self.check()


if __name__=='__main__': unittest.main()
