"""Reject incomplete native reference timing, retirement and audit evidence."""
import copy
import unittest
from verify_native_stream import unique_object, validate_records


class NativeStreamEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.oracle = [dict(round=i,width=3840,height=2160,verifiedPixels=8294400,
                            trianglePixels=2332800,maximumChannelError=1,fnv1a=i,referenceFnv1a=i+10) for i in (1,2)]
        self.host = [dict(nativeStreamStarted=True,guestBridge=False,frames=60,warmup=60,width=3840,height=2160,
                          vendor=0x10de,gpuTimestampFrequency=1000,qpcFrequency=1000)]
        self.host += [dict(nativeBinaryCapture=True,round=i,bytes=33177600,fnv1a=i,file=f'native-backbuffer-{i}.rgba') for i in (1,2)]
        self.host += [dict(nativeStreamCleanup=True,submitted=122,gpuRetired=True)]
        frames = []
        for i in range(122):
            frame = dict(frame=i,audit=i>=120,rotation=i-120 if i>=120 else (i//60)%2,backBuffer=i%2,
                         presentHresult=0,countHresult=0,presentCount=i+1,presentCallQpcBefore=10*i+1,
                         presentCallQpcAfter=10*i+2,statisticsHresult=0,statisticsPresentCount=0,syncQpc=0,
                         visibility=dict(windowVisible=True,windowMinimized=False,centerOwned=False))
            if i < 120: frame.update(gpuStartTicks=10*i+1,gpuEndTicks=10*i+2)
            else: frame['fnv1a'] = i-119
            frames.append(frame)
        self.report = dict(nativeStreamCompleted=True,guestBridge=False,width=3840,height=2160,measuredFrames=60,
                           warmupFrames=60,submitted=122,retired=122,gpuTimestampFrequency=1000,qpcFrequency=1000,
                           wallStartQpc=600,wallEndQpc=1200,pacedWallSeconds=0.6,
                           gpuClearDrawMedianMs=1.0,gpuClearDrawP95Ms=1.0,frames=frames)

    def verify(self, report=None, host=None, oracle=None, exit_code=0):
        return validate_records(self.report if report is None else report, self.host if host is None else host,
                                self.oracle if oracle is None else oracle, exit_code)

    def test_verified_submission_does_not_claim_display_or_performance_ratio(self):
        result = self.verify()
        self.assertTrue(result['nativeReferenceSubmissionVerified'])
        self.assertTrue(result['auditPixelsVerified'])
        for key in ('guestBridgeVerified','allFramesDisplayedVerified','nativePerformanceRatioVerified'):
            self.assertFalse(result[key])

    def test_missing_or_forged_cleanup(self):
        for host in (self.host[:-1],self.host+[self.host[-1]]):
            with self.assertRaises(ValueError): self.verify(host=host)
        for key,value in (('gpuRetired',False),('gpuRetired',1),('submitted',121),('submitted',True)):
            host=copy.deepcopy(self.host);host[-1][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError): self.verify(host=host)
        for value in (False,1,3):
            with self.assertRaises(ValueError): self.verify(exit_code=value)

    def test_frame_sequence_retirement_and_timing_mismatch(self):
        for key,value in (('retired',121),('submitted',True),('gpuClearDrawMedianMs',0.5),
                          ('gpuClearDrawP95Ms',float('nan')),('pacedWallSeconds',False),('wallStartQpc',1000)):
            report=copy.deepcopy(self.report);report[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError): self.verify(report=report)
        report=copy.deepcopy(self.report);report['frames'].pop()
        with self.assertRaises(ValueError): self.verify(report=report)

    def test_bad_gpu_timestamps_present_counts_or_visibility_types(self):
        for key,value in (('gpuStartTicks',True),('gpuEndTicks',10),('gpuStartTicks',1),('frame',0),
                          ('presentHresult',False),('presentCount',0),('presentCallQpcBefore',1),('backBuffer',2),
                          ('rotation',2),('statisticsPresentCount',1000),('audit',1)):
            report=copy.deepcopy(self.report);report['frames'][61][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError): self.verify(report=report)
        report=copy.deepcopy(self.report);report['frames'][61]['visibility']['centerOwned']=1
        with self.assertRaises(ValueError): self.verify(report=report)

    def test_pixel_receipts_and_duplicate_keys(self):
        host=copy.deepcopy(self.host);host[1]['fnv1a']=999
        with self.assertRaises(ValueError): self.verify(host=host)
        oracle=copy.deepcopy(self.oracle);oracle[1]['fnv1a']=1
        with self.assertRaises(ValueError): self.verify(oracle=oracle)
        with self.assertRaises(ValueError): unique_object([('x',1),('x',2)])


if __name__=='__main__': unittest.main()
