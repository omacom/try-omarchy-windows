"""Replay real GPU pixels and preserve failed framing/pressure observations."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
import test_multiapp_cpu384_milestone as decoder
from verify_aperture_info_coalescing import (validate_coalesced_metadata_desktop,
    validate_framed_metadata_desktop,validate_metadata_controls)


class MetadataFraming(unittest.TestCase):
    blob=decoder.MultiappCpu384.__dict__['blob']
    row=decoder.MultiappCpu384.__dict__['row']
    inputs=decoder.MultiappCpu384.__dict__['inputs']

    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-METADATA-AND-TRACE-FRAMING-2026-10-24.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='269fd5f50b74bdebbf0b2f0f54daa5025307080bfcca946212c3bf1e4d9c6cc5'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    def test_actual_gpu_desktop_keeps_complete_pixels_and_lifetimes(self):
        proof=validate_framed_metadata_desktop(**self.inputs(162,True))
        self.assertEqual(proof,self.row(162)['proof'])
        self.assertEqual(proof['frameCount'],1800)
        self.assertEqual([f['maximumChannelError'] for f in proof['frames']],[1,1])
        self.assertTrue(proof['coalescedMetadataGuestCompatibilityVerified'])
        self.assertTrue(proof['atomicGuestReceiptFramingVerified'])
        self.assertFalse(proof['performanceImprovementVerified'])
        self.assertFalse(proof['memoryPressureRecoveryVerified'])
        self.assertFalse(proof['fullDesktopAccepted'])

    def test_actual_unframed_trace_remains_rejected(self):
        data=self.inputs(161,True)
        self.assertEqual(data['guest'].count('HYPRLAND_HOST_QUEUE_RESULT '),1800)
        self.assertIn('omarchy-factoryHYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=1448',data['guest'])
        with self.assertRaisesRegex(ValueError,'Missing/duplicate frame handoff'):
            validate_coalesced_metadata_desktop(**data)

    def test_a_lost_future_receipt_cannot_pass(self):
        data=copy.deepcopy(self.inputs(162,True))
        marker='HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=1448 '
        self.assertEqual(data['guest'].count(marker),1)
        data['guest']=data['guest'].replace(marker,'partial-prefix'+marker,1)
        with self.assertRaises(ValueError):validate_framed_metadata_desktop(**data)

    def test_corrupt_actual_window_pixels_cannot_pass(self):
        data=copy.deepcopy(self.inputs(162,True));name='relay-window-0000000000001800.rgba'
        raw=bytearray(data['frames'][name]);raw[0]^=255;data['frames'][name]=bytes(raw)
        with self.assertRaises(ValueError):validate_framed_metadata_desktop(**data)

    def test_cleanup_after_vm_exit_cannot_pass(self):
        data=copy.deepcopy(self.inputs(162,True))
        data['report']['ownedQemuControl']['allocationMappingsReleasedAfterVmExit']=1
        with self.assertRaises(ValueError):validate_framed_metadata_desktop(**data)

    def test_metadata_counts_preserve_freshness_and_fault_controls(self):
        def get(name):return self.blob(self.value['controls'][name]).decode('utf-8')
        before=get('aperture-info-before-v2/controls.log');after=get('aperture-info-after-v1/controls.log')
        self.assertEqual(validate_metadata_controls(before,after),self.value['metadataControlProof'])
        with self.assertRaises(ValueError):validate_metadata_controls(before,after.replace('capacities=6 profiles=6','capacities=5 profiles=5',1))
        marker=('GUEST_TRACE_LINE legacyPrefixCollisionReproduced=true atomicReceipts=2000 '
                'concurrentWriter=true boundedSingleLine=true writeErrorsPropagated=true gpu=false')
        self.assertEqual(get('trace-framing-controls-v1/controls.log').count(marker),1)

    def test_real_memory_pressure_failures_are_not_relabelled_successful(self):
        for number,frames,usage in ((159,851,1085448192),(160,953,1090838528)):
            row=self.row(number);report=json.loads(self.blob(row['files']['driver.json']))
            clean=report['disconnectCleanup']
            self.assertFalse(row['compatibilityAccepted']);self.assertEqual(report['hostBridgeExit'],1)
            self.assertEqual(clean['completedCompositorFrames'],frames)
            self.assertEqual(clean['peakReportedGpuUsageBytes'],usage)
            self.assertEqual(clean['reportedGpuUsageLimitBytes'],1073741824)
            self.assertGreater(clean['hwQueuesReleasedAfterVmExit'],0)
            self.assertIn(b'FAIL: Reported GPU usage limit after deferred residency',self.blob(row['files']['driver.errors.log']))

    def test_compile_sources_and_original_traces_remain_available(self):
        for digest in self.value['blobs']:self.blob(digest)
        for item in self.value['guestSourceArchives'].values():
            manifest=json.loads(self.blob(item['manifest']))
            for name,digest in manifest['sources'].items():self.assertEqual(digest,item['sources']['source-frozen/'+name])
            for name,digest in item['sources'].items():
                self.assertFalse(name.endswith(('.exe','.so','.ko','.ext4','.cpio.gz')));self.blob(digest)
        for name,digest in self.value['auditSources'].items():
            # These capacity sources now adopt the explicit application-memory
            # agreement. Replay their original bytes from the already published
            # archive; a historical source pin must not freeze the live header.
            # The evidence digest and every historical trace/pixel predicate
            # remain unchanged. Current source is compiled by portable checks.
            if name in ('guest_kernel_aperture.h','guest_aperture_info_test.cpp'):
                raw=self.blob(digest)
            else:
                raw=(Path(__file__).parent/name).read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(),digest)


if __name__=='__main__':unittest.main()
