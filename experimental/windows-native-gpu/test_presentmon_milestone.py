"""Reject fabricated or unrelated ETW evidence; no GPU/admin access required."""
import copy
import unittest
from presentmon_evidence import correlate_presentmon


class PresentMonTests(unittest.TestCase):
    def setUp(self):
        self.receipts = [dict(round=i, presentProcessId=77, presentSwapChain=4096,
                             presentCallQpcBefore=i*100, presentCallQpcAfter=i*100+10,
                             qpcFrequency=10000000, presentHresult=0, lastPresentCount=i,
                             presentQueueFenceTarget=i, presentQueueFenceObserved=i,
                             windowVisible=True, cpuUpload=False, backBufferState='PRESENT') for i in (1,2)]
        self.header = 'Application,ProcessID,SwapChainAddress,Runtime,Dropped,PresentMode,msUntilDisplayed,QPCTime\n'
        self.rows = ['driver-bridge.exe,77,0x1000,DXGI,0,Composed: Flip,18.8,105\n',
                     'driver-bridge.exe,77,0x1000,DXGI,0,Composed: Flip,16.5,205\n']
    def test_exact_identity_and_interval(self):
        self.assertTrue(correlate_presentmon(self.header+''.join(self.rows),self.receipts)['etwFramesDisplayedVerified'])
    def test_missing_first_frame_is_not_a_full_pass(self):
        r = correlate_presentmon(self.header+self.rows[1],self.receipts)
        self.assertEqual(r['matchedRows'],1)
        self.assertFalse(r['etwFramesDisplayedVerified'])
        self.assertTrue(r['frames'][1]['etwDisplayed'])
    def test_wrong_identity_qpc_and_malformed_rows(self):
        for old,new in [('77','78'),('0x1000','0x1001'),('105','111'),('105','nan'),
                        ('18.8','NaN'),('18.8','-1'),('DXGI','D3D9'),(',0,Composed',',2,Composed')]:
            with self.subTest(new=new), self.assertRaises(ValueError):
                correlate_presentmon(self.header+self.rows[0].replace(old,new)+self.rows[1],self.receipts)
        with self.assertRaises(ValueError):
            correlate_presentmon(self.header+''.join(self.rows)+self.rows[0],self.receipts)
    def test_drop_zero_time_and_unknown_mode_not_display_evidence(self):
        for old,new in [(',0,Composed',',1,Composed'),('18.8','0'),('Composed: Flip','Unknown')]:
            self.assertFalse(correlate_presentmon(self.header+self.rows[0].replace(old,new)+self.rows[1],self.receipts)['etwFramesDisplayedVerified'])
    def test_bad_receipts(self):
        for key,value in [('round',True),('windowVisible',False),('presentHresult',1),
                          ('presentQueueFenceObserved',0),('presentCallQpcAfter',99)]:
            receipts=copy.deepcopy(self.receipts); receipts[0][key]=value
            with self.subTest(key=key), self.assertRaises(ValueError):
                correlate_presentmon(self.header+''.join(self.rows),receipts)
        receipts=copy.deepcopy(self.receipts); receipts[1]['presentCallQpcBefore']=110
        with self.assertRaises(ValueError):
            correlate_presentmon(self.header+''.join(self.rows),receipts)
    def test_duplicate_or_missing_columns(self):
        for header in [self.header.replace('QPCTime','QPCTime,QPCTime'),self.header.replace(',QPCTime','')]:
            with self.assertRaises(ValueError):
                correlate_presentmon(header+''.join(self.rows),self.receipts)


if __name__ == '__main__':
    unittest.main()
