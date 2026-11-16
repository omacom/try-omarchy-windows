"""Reject false GPU capability milestones using retained real hardware captures."""
import copy
import json
from pathlib import Path
import unittest
from verify_broker_objects_capture import validate_broker_objects,validate_broker_fences
from verify_broker_routing_capture import validate_broker_routing
from verify_resource_fd_capture import validate_resource_fds
from verify_mesa_fence_fd_capture import validate_mesa_fence_fds


class BrokerObjectsMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/BROKER-OBJECTS-2026-10-13.json').read_text())

    def setUp(self): self.run=copy.deepcopy(next(r for r in self.evidence['runs'] if r['scope']=='recipient'))

    def check(self,run=None):
        run=self.run if run is None else run
        p=run['proof']; values=(p['report'],'\n'.join(p['guestLines']),p['records'])
        return validate_broker_fences(*values) if run['scope']=='fence' else validate_broker_objects(*values,recipient=run['scope']=='recipient')

    def replace(self,before,after):
        lines=self.run['proof']['guestLines']; self.assertTrue(any(before in l for l in lines))
        self.run['proof']['guestLines']=[l.replace(before,after) for l in lines]

    def test_real_resources_recipient_retention_and_fences_keep_their_scopes(self):
        self.assertEqual([r['scope'] for r in self.evidence['runs']],['resource','recipient','recipient','fence','fence'])
        for run in self.evidence['runs']:
            audit=self.check(run); self.assertEqual(audit,run['audit'])
            self.assertTrue(audit['unprivilegedGraphicsClientVerified']); self.assertEqual(audit['brokerProtocol'],3)
            for key in ('crossProcessGpuResourceImportVerified','multiClientGpuRoutingVerified','gbmVerified','dmaBufVerified',
                        'desktopVerified','desktopStabilityVerified','nearNativePerformanceVerified','openGl4kVerified'):
                self.assertIs(audit[key],False)
            self.assertEqual(audit['retiredNativeCommands'],19 if run['scope']=='fence' else 24)
            if run['scope']=='fence': self.assertTrue(audit['mesaFutureGpuQueueOrderingVerified'])
            else:
                self.assertTrue(audit['fullBufferReadbackVerified'])
                self.assertEqual(audit['recipientFdBeyondProducerExitVerified'],run['scope']=='recipient')

    def test_retained_hardware_failures_are_not_promoted(self):
        self.assertEqual(len(self.evidence['failures']),2)
        for run in self.evidence['failures']:
            with self.subTest(label=run['label']),self.assertRaises(ValueError): self.check(run)

    def test_transports_protocol_and_workload_cannot_be_substituted(self):
        p=self.run['proof']; values=(p['report'],'\n'.join(p['guestLines']),p['records'])
        for call in (lambda:validate_broker_routing(*values),lambda:validate_resource_fds(*values,full_readback=True),
                     lambda:validate_broker_objects(*values),lambda:validate_broker_fences(*values),
                     lambda:validate_mesa_fence_fds(*values)):
            with self.assertRaises(ValueError): call()
        self.replace('protocol=3','protocol=2')
        with self.assertRaises(ValueError): self.check()

    def test_recipient_lifetime_receipts_are_unique_and_required(self):
        for prefix in ('MESA_RESOURCE_FD_RECIPIENT_ARMED ','MESA_RESOURCE_FD_RECIPIENT producerExited=',
                       'MESA_RESOURCE_FD_RECIPIENT closingCapability=','BROKER_OBJECT_DRAIN ',
                       'BROKER_OBJECT_CLOSE ','BROKER_OBJECT_SUMMARY '):
            for duplicate in (False,True):
                self.setUp(); lines=self.run['proof']['guestLines']; selected=next(l for l in lines if l.startswith(prefix))
                self.run['proof']['guestLines']=lines+[selected] if duplicate else [l for l in lines if l!=selected]
                with self.subTest(prefix=prefix,duplicate=duplicate),self.assertRaises(ValueError): self.check()

    def test_early_native_close_wrong_identity_and_gpu_scope_are_rejected(self):
        for before,after in (('retained=1','retained=0'),('resolved=6','resolved=5'),('reaped=2 closed=2','reaped=1 closed=1'),
                             ('capabilityOpen=true','capabilityOpen=false'),('gpuWork=false','gpuWork=true'),
                             ('producerFdClosed=true','producerFdClosed=false'),
                             ('kind=2 ticket=263 cookie=264','kind=2 ticket=263 cookie=265')):
            self.setUp(); self.replace(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
        self.setUp(); lines=self.run['proof']['guestLines']; close=next(l for l in lines if l.startswith('BROKER_OBJECT_CLOSE '))
        lines.remove(close); lines.insert(next(i for i,l in enumerate(lines) if l.startswith('MESA_RESOURCE_FD_RECIPIENT producerExited=')),close)
        with self.assertRaises(ValueError): self.check()

    def test_pixels_storage_and_native_reference_counts_still_required(self):
        for before,after in (('fnv1a=95c9d10519374325','fnv1a=0000000000000000'),
                             ('sourceReleased=true fdClosed=true','sourceReleased=false fdClosed=true'),
                             ('readbackBytes=33177600','readbackBytes=7864320')):
            self.setUp(); self.replace(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
        for key,value in (('closedResourceHandles',1),('liveSharedResourceHandles',1),('exportedSyncHandles',1)):
            self.setUp(); p=self.run['proof']; p['report']['disconnectCleanup'][key]=value
            next(r for r in p['records'] if 'driverCleanupVerified' in r)[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check()

    def test_fence_gpu_ordering_and_recoverable_errors_still_required(self):
        for before,after in (('recoverableErrors=4','recoverableErrors=3'),('resolved=8','resolved=6'),
                             ('dependentObserved=3','dependentObserved=1'),('futureWaits=1','futureWaits=0'),
                             ('fdRetained=true oldFenceRetained=true','fdRetained=false oldFenceRetained=false')):
            self.run=copy.deepcopy(next(r for r in self.evidence['runs'] if r['scope']=='fence')); self.replace(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()


if __name__=='__main__': unittest.main()
