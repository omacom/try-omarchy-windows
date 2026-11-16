"""Malformed evidence must not turn a 4K diagnostic into a successful run."""
import copy
import unittest
from verify_4k_capture import LIVE_COUNTERS, validate_cleanup, validate_oracle_frames


class CaptureEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.frames=[dict(round=i,width=3840,height=2160,verifiedPixels=8294400,
                          trianglePixels=2332800,maximumChannelError=1,fnv1a=i,referenceFnv1a=i+100) for i in (1,2)]
        cleanup=dict(driverCleanupVerified=True,cleanupFailures=0,pendingNativeSubmissions=0,pendingHwQueueSignals=0,
                     **{'live'+k:0 for k in LIVE_COUNTERS})
        vm=dict(ownedQemuExited=True,qemuExit=0,qemuForcedStop=False,fenceControlFailed=False,
                liveFenceMappings=0,liveAllocationMappings=0,liveAllocationMappedBytes=0,liveAllocationSlotMappings=0,
                fenceApertureSlots=128,allocationApertureSlots=128,fenceMappingsCreated=80,fenceUnmapAcknowledgements=80,
                allocationMappingsCreated=93,allocationUnmapAcknowledgements=93,
                allocationSlotMappingsCreated=140,allocationSlotUnmapAcknowledgements=140)
        self.report=dict(hostBridgeExit=0,disconnectCleanup=cleanup,ownedQemuControl=vm)
        self.records=copy.deepcopy([cleanup,vm])
    def test_valid_typed_records(self):
        validate_oracle_frames(self.frames)
        validate_cleanup(self.report,self.records)
    def test_wrong_oracle_profile_counts_types_and_tolerance(self):
        for key,value in [('round',2),('round',True),('width',130),('height',73),('verifiedPixels',0),
                          ('trianglePixels',0),('maximumChannelError',3),('maximumChannelError',True),
                          ('maximumChannelError',-1),('fnv1a',-1),('fnv1a',1<<64),('referenceFnv1a',False)]:
            frames=copy.deepcopy(self.frames); frames[0][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError): validate_oracle_frames(frames)
    def test_missing_duplicate_and_repeated_frames(self):
        for frames in (self.frames[:1],self.frames+self.frames[:1],[self.frames[0],self.frames[0]]):
            with self.assertRaises(ValueError): validate_oracle_frames(frames)
        frames=copy.deepcopy(self.frames);frames[1]['fnv1a']=frames[0]['fnv1a']
        with self.assertRaises(ValueError): validate_oracle_frames(frames)
    def test_missing_live_counter_and_pending_work(self):
        for key in ('liveAdapters','liveDevices','liveVendorCpuBytes','pendingNativeSubmissions'):
            report=copy.deepcopy(self.report);del report['disconnectCleanup'][key]
            with self.subTest(key=key),self.assertRaises(ValueError):validate_cleanup(report,self.records)
        for key in ('liveDevices','pendingNativeSubmissions','cleanupFailures'):
            report=copy.deepcopy(self.report);report['disconnectCleanup'][key]=1
            with self.assertRaises(ValueError):validate_cleanup(report,self.records)
    def test_boolean_exit_and_incomplete_mapping_retirement(self):
        report=copy.deepcopy(self.report);report['hostBridgeExit']=False
        with self.assertRaises(ValueError):validate_cleanup(report,self.records)
        for key,value in [('qemuForcedStop',True),('qemuExit',False),('fenceUnmapAcknowledgements',79),
                          ('allocationMappingsCreated',True),('allocationSlotUnmapAcknowledgements',139)]:
            report=copy.deepcopy(self.report);report['ownedQemuControl'][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):validate_cleanup(report,self.records)
    def test_missing_duplicate_or_modified_raw_receipt(self):
        for records in (self.records[:1],self.records+[self.records[0]]):
            with self.assertRaises(ValueError):validate_cleanup(self.report,records)
        records=copy.deepcopy(self.records);records[0]['driverCleanupVerified']=1
        with self.assertRaises(ValueError):validate_cleanup(self.report,records)


if __name__=='__main__':unittest.main()
