"""Retain actual v10 admission/cleanup and reject unsafe or incomplete receipts."""
import base64
import copy
import json
from pathlib import Path
import unittest
import zlib
from verify_4k_presentation_budget import validate
from verify_normal_root_graphics import validate as legacy_validate


class PresentationBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.value=json.loads((cls.root/'docs/evidence/NORMAL-OMARCHY-4K-PRESENTATION-BUDGET-2026-10-16.json').read_text())

    @staticmethod
    def inputs(row):
        unpack=lambda key:zlib.decompress(base64.b64decode(row[key],validate=True))
        return dict(report=json.loads(unpack('reportZlibBase64')),guest=unpack('guestReceiptsZlibBase64').decode().replace('\r\n','\n'),
            records=json.loads(unpack('nativeRecordsZlibBase64')),source=row['source'],
            frames={name:zlib.decompress(base64.b64decode(data,validate=True)) for name,data in row['rawFramesZlibBase64'].items()})

    def test_two_fresh_4k_guests_pass_admission_readback_and_explicit_shutdown(self):
        for row in self.value['passing']:
            proof=validate(**self.inputs(row));self.assertEqual(proof,row['audit'])
            self.assertTrue(proof['presentationMemoryAdmissionVerified']);self.assertTrue(proof['clean4kCompositorShutdownVerified'])
            self.assertEqual(proof['readback']['framesSubmitted'],12);self.assertEqual(proof['readback']['ordinaryFramesWithoutCpuReadback'],10)
            self.assertLessEqual(proof['peakReportedGpuUsageBytes'],1073741824)
            for key in ('desktopVerified','quickshellGpuVerified','normalOmarchySessionVerified','allFramesDisplayedVerified','nearNativePerformanceVerified','physicalGpuQuotaVerified'):
                self.assertFalse(proof[key])

    def test_missing_reordered_or_denied_presentation_admission_is_rejected(self):
        original=self.inputs(self.value['passing'][-1])
        for mutation in ('missing','order','denied','usage','overflow','size','unknown'):
            data=copy.deepcopy(original);checks=[r for r in data['records'] if 'nativePresentationMemoryGuard' in r]
            if mutation=='missing':data['records'].remove(checks[2])
            if mutation=='order':checks[0]['stage']='device-after'
            if mutation=='denied':checks[2]['accepted']=False
            if mutation=='usage':checks[2]['usageBytes']=1073741825
            if mutation=='overflow':checks[2]['additionalBytes']=(1<<64)-1
            if mutation=='size':checks[2]['additionalBytes']=0
            if mutation=='unknown':checks[2]['stage']='unaccounted'
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(**data)

    def test_mismatched_published_budget_and_native_build_are_rejected(self):
        original=self.inputs(self.value['passing'][-1])
        for mutation in ('native','profile','guard','cpu','software'):
            data=copy.deepcopy(original)
            if mutation=='native':data['report']['driverBridgeSha256']='0'*64
            if mutation=='profile':data['source']['manifest']['kernelCpuCapacityProfile']=9
            if mutation=='guard':next(r for r in data['records'] if 'openGlProfileNegotiated' in r)['reportedGpuUsageLimitBytes']=536870912
            if mutation=='cpu':data['report']['disconnectCleanup']['vendorCpuMappedByteLimit']+=4096
            if mutation=='software':data['source']['manifest']['qtQuickSoftwareProbe']=True
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(**data)

    def test_forced_shutdown_and_deferred_or_pending_resources_are_rejected(self):
        original=self.inputs(self.value['passing'][-1])
        for mutation in ('stop','signal','vm','deferred','unmap','pending','failed','broker'):
            data=copy.deepcopy(original);clean=data['report']['disconnectCleanup'];vm=data['report']['ownedQemuControl']
            if mutation=='stop':data['guest']+='HYPRLAND_STARTUP_STOP reason=bounded-diagnostic signal=TERM windowsPresentation=false\n'
            if mutation=='signal':data['guest']+='NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL signal=15\n'
            if mutation=='vm':vm['qemuForcedStop']=True
            if mutation=='deferred':vm['allocationMappingsReleasedAfterVmExit']=1
            if mutation=='unmap':vm['fenceUnmapAcknowledgements']-=1
            if mutation=='pending':clean['pendingGpuQueueWaits']=1
            if mutation=='failed':clean['failedVendorAllocations']=1
            if mutation=='broker':data['guest']=data['guest'].replace('retainedViews=0 nativeCalls=','retainedViews=1 nativeCalls=')
            with self.subTest(mutation=mutation),self.assertRaises(ValueError):validate(**data)

    def test_first_seven_frame_timeout_remains_rejected(self):
        row=self.value['failed'][0];data=self.inputs(row)
        self.assertIn('HYPRLAND_HOST_QUEUE_END frames=7 target=12 failed=1\n',data['guest'])
        self.assertIn('NORMAL_ROOT_GRAPHICS_EXIT result=124 normalOmarchySession=false\n',data['guest'])
        self.assertEqual(data['report']['disconnectCleanup']['completedCompositorFrames'],7)
        self.assertGreater(data['report']['ownedQemuControl']['fenceMappingsReleasedAfterVmExit'],0)
        with self.assertRaises(ValueError):validate(**data)

    def test_new_legacy_300_frame_scene_retains_original_budgets(self):
        row=self.value['legacy'];data=self.inputs(row);proof=legacy_validate(**data,expected_frames=300)
        self.assertEqual(proof,row['audit']);self.assertEqual(proof['scene']['queued']['framesSubmitted'],300)
        clean=data['report']['disconnectCleanup']
        for name,value in (('vendorCpuMappedByteLimit',96*1024*1024),('vendorGpuMappedByteLimit',256*1024*1024),('reportedGpuUsageLimitBytes',256*1024*1024)):
            self.assertEqual(clean[name],value)

    def test_measured_ipc_timeout_is_retained_despite_clean_gpu_teardown(self):
        data=self.inputs(self.value['failed'][1]);clean=data['report']['disconnectCleanup'];vm=data['report']['ownedQemuControl']
        self.assertIn('stage=receive errno=11 receivedBytes=0 elapsedMs=5312\n',data['guest'])
        self.assertIn('HYPRLAND_HOST_QUEUE_END frames=7 target=12 failed=1\n',data['guest'])
        self.assertTrue(clean['driverCleanupVerified']);self.assertEqual(clean['completedCompositorFrames'],7)
        self.assertEqual(vm['fenceMappingsReleasedAfterVmExit'],0)
        with self.assertRaises(ValueError):validate(**data)

    def test_native_broker_mutations_and_default_rejections_pass(self):
        decode=lambda name:zlib.decompress(base64.b64decode(self.value['validationLogs'][name],validate=True)).decode()
        controls=decode('profile10-controls-v1/build.log')
        self.assertIn('NATIVE_PRESENTATION_BUDGET fixed48ByteReceipt=true exact1GiBUsage=true overflowRejected=true oldGuardsUnchanged=true gpu=false',controls)
        self.assertIn('BROKER_PRESENTATION_BUDGET fixed48ByteReceipt=true exact1GiBUsage=true malformedRejected=true kernel=mock gpu=false',controls)
        self.assertIn('PASS: 22 explicit OpenGL transport cases',controls);self.assertIn('PASS: 21 explicit OpenGL transport cases',controls)
        self.assertIn('PASS: 105 native capacity/control rejection cases',decode('profile10-default-cli.log'))
        self.assertIn('pidfdExitRequired=true native=mock kernel=mock gpu=false',decode('profile10-root-broker.log'))


if __name__=='__main__':unittest.main()
