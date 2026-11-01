"""CPU-only CDP lifecycle controls with a virtual clock and explicit protocol fake."""
import unittest
from browser_target_retirement import close_owned_page


class Protocol:
    def __init__(self, snapshots, close_reply=None, delay=0):
        self.snapshots=list(snapshots);self.close_reply={'success':True} if close_reply is None else close_reply
        self.delay=delay;self.now=0.;self.calls=[];self.sleeps=[]
    def clock(self):return self.now
    def sleep(self,seconds):self.sleeps.append(seconds);self.now+=seconds
    def call(self,method,params=None,seconds=20):
        self.calls.append((method,params,seconds));self.now+=self.delay
        if method=='Target.closeTarget':return self.close_reply
        if method=='Target.activateTarget':return {}
        if method=='Target.getTargets':
            value=self.snapshots.pop(0) if len(self.snapshots)>1 else self.snapshots[0]
            if isinstance(value,dict):return value
            return {'targetInfos':[dict(type='page',targetId=value) for value in value]}
        raise AssertionError(method)
    def close(self,**kwargs):
        return close_owned_page(self.call,'original','temporary',{'original','other'},clock=self.clock,sleep=self.sleep,**kwargs)


class TabRetirement(unittest.TestCase):
    def test_immediate_exact_retirement(self):
        protocol=Protocol([{'original','other'}]);result=protocol.close()
        self.assertEqual(result['queries'],1);self.assertTrue(result['exactBaselineRestored'])
        self.assertTrue(result['ownedTemporaryAbsent']);self.assertFalse(protocol.sleeps)
        self.assertEqual([r[0] for r in protocol.calls],['Target.closeTarget','Target.activateTarget','Target.getTargets'])
        self.assertTrue(all(0<r[2]<=2 for r in protocol.calls))
    def test_delayed_owned_tab_only(self):
        protocol=Protocol([{'original','other','temporary'}]*3+[{'original','other'}]);result=protocol.close()
        self.assertEqual(result['queries'],4);self.assertEqual(len(protocol.sleeps),3)
        self.assertLess(result['elapsedSeconds'],2);self.assertEqual(result['pageTargetCount'],2)
    def test_never_retired_times_out_without_resetting_deadline(self):
        protocol=Protocol([{'original','other','temporary'}])
        with self.assertRaises(TimeoutError):protocol.close()
        self.assertLessEqual(protocol.now,2.000001)
        self.assertTrue(all(0<r[2]<=2 for r in protocol.calls))
        self.assertLessEqual(len(protocol.calls),83)
    def test_slow_valid_reply_cannot_pass_after_deadline(self):
        protocol=Protocol([{'original','other'}],delay=.8)
        with self.assertRaises(TimeoutError):protocol.close()
        self.assertEqual(len(protocol.calls),3)
    def test_original_other_page_and_unrelated_popup_are_not_tolerated(self):
        for pages in ({'other','temporary'},{'original','temporary'},{'original','other','popup'},{'original','other','temporary','popup'}):
            with self.subTest(pages=pages):
                protocol=Protocol([pages,{'original','other'}])
                with self.assertRaisesRegex(RuntimeError,'beyond the owned'):protocol.close()
                self.assertFalse(protocol.sleeps)
    def test_close_acknowledgement_required(self):
        for reply in ({'success':False},{},True,{'success':1}):
            with self.subTest(reply=reply):
                protocol=Protocol([{'original','other'}],close_reply=reply)
                with self.assertRaisesRegex(RuntimeError,'not acknowledged'):protocol.close()
                self.assertEqual(len(protocol.calls),1)
    def test_malformed_or_duplicate_target_list_rejected(self):
        values=[{}, {'targetInfos':None},{'targetInfos':[{}]}, {'targetInfos':[dict(type='worker')]*257},
                {'targetInfos':[dict(type='page',targetId=1)]},
                {'targetInfos':[dict(type='page',targetId='original')]*2}]
        for value in values:
            with self.subTest(value=value):
                with self.assertRaises(RuntimeError):Protocol([value]).close()
    def test_identity_and_deadline_rejected_before_rpc(self):
        for seconds in (0,-1,2.01,float('inf'),float('nan'),True):
            protocol=Protocol([{'original','other'}])
            with self.subTest(seconds=seconds):
                with self.assertRaises(ValueError):protocol.close(timeout_seconds=seconds)
                self.assertFalse(protocol.calls)
        protocol=Protocol([{'original','other'}])
        with self.assertRaises(ValueError):
            close_owned_page(protocol.call,'original','other',{'original','other'})
        self.assertFalse(protocol.calls)


if __name__=='__main__':unittest.main(verbosity=2)
