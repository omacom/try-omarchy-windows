"""Corrupt the actual guest broker contract and direct GPU regression capture."""
import copy
import json
from pathlib import Path
import unittest
from verify_broker_peer_capture import validate_broker_peer


class BrokerPeerMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/BROKER-PEER-2026-10-12.json').read_text())

    def setUp(self): self.proof=copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None):
        p=self.proof if proof is None else proof
        return validate_broker_peer(p['report'],'\n'.join(p['guestLines']),p['records'])

    def test_real_guest_contract_and_gpu_regression_have_separate_scopes(self):
        audit=self.check()
        self.assertEqual(audit,self.evidence['runs'][0]['audit'])
        self.assertTrue(audit['brokerPeerBoundaryVerified'])
        self.assertEqual(audit['brokerTestClientUid'],65534)
        self.assertEqual(audit['retiredNativeCommands'],24)
        for key in ('multiClientGpuRoutingVerified','crossProcessGpuResourceImportVerified',
                    'desktopVerified','desktopStabilityVerified','nearNativePerformanceVerified'):
            self.assertIs(audit[key],False)

    def test_missing_duplicate_and_failed_contracts_rejected(self):
        for line in [l for l in self.proof['guestLines'] if l.startswith('BROKER_PEER_')]:
            for duplicate in (False,True):
                self.setUp()
                self.proof['guestLines'] = self.proof['guestLines']+[line] if duplicate else [l for l in self.proof['guestLines'] if l!=line]
                with self.subTest(line=line,duplicate=duplicate), self.assertRaises(ValueError): self.check()
        self.setUp(); self.proof['guestLines'].append('BROKER_PEER_START failed=true')
        with self.assertRaises(ValueError): self.check()

    def test_wrong_identity_lifetime_and_scope_rejected(self):
        for before,after in (('uid=65534','uid=0'),('aliveDuringFinalizer=true','aliveDuringFinalizer=false'),
                             ('exitViaPidfd=true','exitViaPidfd=false'),('liveWorkerRetained=true','liveWorkerRetained=false'),
                             ('fdLeaks=0','fdLeaks=1'),('malformedCases=13','malformedCases=12'),('gpuRouting=false','gpuRouting=true')):
            self.setUp(); self.proof['guestLines']=[l.replace(before,after) for l in self.proof['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()

    def test_contract_order_rejected(self):
        peer=[l for l in self.proof['guestLines'] if l.startswith('BROKER_PEER_')]
        self.proof['guestLines']=[l for l in self.proof['guestLines'] if l not in peer]+peer
        with self.assertRaises(ValueError): self.check()

    def test_gpu_failure_cannot_be_promoted_by_passing_peer_test(self):
        self.proof['guestLines']=[l.replace('BRIDGE_RUNTIME_EXIT=0','BRIDGE_RUNTIME_EXIT=1') for l in self.proof['guestLines']]
        with self.assertRaises(ValueError): self.check()


if __name__=='__main__': unittest.main()
