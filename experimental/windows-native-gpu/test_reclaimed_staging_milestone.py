"""Replay the two actual 4K NVIDIA runs and reject unsafe reclamation claims."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib
from native68_source_archive import native68_sources

from verify_reclaimed_staging_input_response import (
    delivery_metadata, reclaimed_staging_admission, validate_reclaimed_staging_input_response,
)
from verify_qmp_input_response import validate_qmp_input_response


class ReclaimedStagingMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo / 'docs/evidence/OMARCHY-4K-STAGING-RECLAIM-2026-10-20.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '78811621581df4b3e898af0b01c1e73df736d0cea3949507557bf007264c27cf'
        cls.value = json.loads(raw)
        receipt = cls.value['rawBase']
        raw = (cls.repo / receipt['evidencePath']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == receipt['evidenceSha256']
        base = next(row for row in json.loads(raw)['runs'] if row['run'] == receipt['run'])
        pixels = zlib.decompress(base64.b64decode(base['rawFramesZlibBase64'][receipt['frameFile']], validate=True))
        assert len(pixels) == 33177600 and hashlib.sha256(pixels).hexdigest() == receipt['rawSha256']
        cls.base_int = int.from_bytes(pixels, 'little')
        cls.inputs = [cls.unpack(row) for row in cls.value['runs']]

    @classmethod
    def unpack(cls, row, reference=None):
        decode = lambda key: zlib.decompress(base64.b64decode(row[key], validate=True))
        frames = {}
        for name, receipt in row['rawFrames'].items():
            delta = zlib.decompress(base64.b64decode(receipt['xorZlibBase64'], validate=True))
            assert len(delta) == receipt['bytes'] == 33177600
            pixels = (cls.base_int ^ int.from_bytes(delta, 'little')).to_bytes(len(delta), 'little')
            assert hashlib.sha256(pixels).hexdigest() == receipt['rawSha256']
            frames[name] = pixels
        return dict(report=json.loads(decode('reportZlibBase64')), guest=decode('guestZlibBase64').decode(),
                    records=json.loads(decode('nativeRecordsZlibBase64')), frames=frames,
                    source=json.loads(decode('sourceZlibBase64')), owner_output=decode('ownerOutputZlibBase64').decode(),
                    wallpaper_reference=reference or cls.value['wallpaperReference'])

    def test_both_complete_gpu_input_responses_and_real_idle_trims(self):
        self.assertEqual([row['run'] for row in self.value['runs']], [95, 96])
        self.assertEqual(self.value['acceptedRunCount'], 2)
        self.assertTrue(self.value['boundedRepeatedInputResponseVerified'])
        self.assertFalse(self.value['hardwareReallocationObserved'])
        for row, data in zip(self.value['runs'], self.inputs):
            proof = validate_reclaimed_staging_input_response(**data)
            self.assertEqual(proof, row['audit'])
            self.assertTrue(proof['safeReclamationObserved'])
            self.assertTrue(proof['gpuInputResponseWithReclaimEnabledVerified'])
            self.assertEqual(proof['staging']['trimmedTextures'], 1)
            self.assertEqual(proof['staging']['reallocatedTextures'], 0)
            self.assertEqual(proof['staging']['capacity'], 3)
            self.assertEqual(proof['inputDelivery']['acknowledgedCommands'], 33)
            self.assertEqual(proof['inputResponse']['finalFrameConfirmationPixels'], 9143)
            for key in ('hostWindowIntegrationVerified', 'normalLauncherVerified', 'fullDesktopAccepted',
                        'allFramesDisplayedVerified', 'nearNativePerformanceVerified'):
                self.assertFalse(proof[key])
            pressure = next(row for row in data['records'] if row.get('nativeResidencyStagingPressure'))
            self.assertEqual(pressure['releasedBytes'], 33423360)
            self.assertEqual(pressure['usageBefore'] - pressure['usageAfter'], 33423360)
            self.assertLess(data['report']['disconnectCleanup']['peakReportedGpuUsageBytes'], 1073741824)

    def test_exact_build_auditors_controls_image_and_prior_failure_provenance(self):
        for data in self.inputs:
            source, report = data['source'], data['report']
            self.assertEqual(report['driverBridgeSha256'], '0f98c12a948693ebb7721d64f700fea4dcf0eb53a9fefd7e82f933286df7a52d')
            self.assertEqual(report['initramfsSha256'], '23aadee3e3feb7d05d75910c9ce88e6a001a432d2f73d803a2e0424509868943')
            self.assertEqual(source['sourceImageSha256Before'], source['sourceImageSha256After'])
            receipt = source['nativeBuildReceipt']
            self.assertEqual(receipt['bridgeSha256'], report['driverBridgeSha256'])
            for name, raw in native68_sources(self.repo, receipt).items():
                self.assertEqual(hashlib.sha256(raw).hexdigest(), receipt['sources'][name])
            for name, digest in source['manifest']['sources'].items():
                if name.endswith(('.h', '.cpp')):
                    self.assertEqual(receipt['sources']['experimental/windows-native-gpu/' + name], digest)
            for name, key in (('build-native-v68.log', 'buildLogSha256'),
                              ('build-native-default-v68.log', 'defaultBuildLogSha256')):
                control = self.value['controls'][name]
                log = zlib.decompress(base64.b64decode(control['logZlibBase64'], validate=True))
                self.assertEqual(hashlib.sha256(log).hexdigest(), control['sha256'])
                self.assertEqual(control['sha256'], receipt[key])
                self.assertIn(b'STAGING_RECLAIM_CONTROL', log)
        for name, digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(), digest)
        image = self.value['image']
        png = (self.repo / image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(), image['pngSha256'])
        self.assertEqual(struct.unpack('>II', png[16:24]), (3840, 2160))
        self.assertEqual((image['run'], image['frame'], image['pixelsEdited']), (96, 12, False))
        self.assertEqual(hashlib.sha256(self.inputs[1]['frames']['compositor-copy-2.raw']).hexdigest(), image['sourceRawSha256'])
        prior = self.value['previousEvidence']
        self.assertEqual(prior['retainedFailureRun'], 94)
        self.assertEqual(hashlib.sha256((self.repo / prior['path']).read_bytes()).hexdigest(), prior['sha256'])

    def test_actual_busy_pending_fences_stale_generation_and_release_ledger_rejected(self):
        for label, key, value in (
            ('nativeCompositorStagingTrimmed', 'slotOccupied', True),
            ('nativeCompositorStagingTrimmed', 'inputCopyFenceObserved', 1),
            ('nativeCompositorStagingTrimmed', 'outputCopyFenceObserved', 1),
            ('nativeCompositorStagingTrimmed', 'generation', 2),
            ('nativeCompositorStagingTrimmed', 'commandsReset', False),
            ('nativeCompositorStagingTrimmed', 'serializedResidency', False),
            ('nativeResidencyStagingPressure', 'limitBytes', 2147483648),
            ('nativeResidencyStagingPressure', 'releasedBytes', 0),
            ('nativeCompositorRingStopped', 'liveStagingTextureBytes', 33423360),
        ):
            records = copy.deepcopy(self.inputs[0]['records'])
            next(row for row in records if label in row)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                reclaimed_staging_admission(records, self.inputs[0]['report']['disconnectCleanup']['compositorBackpressure'])

    def test_gpu_audits_pixel_bytes_and_input_response_still_mandatory(self):
        base = self.inputs[0]
        data = dict(base, records=[row for row in base['records'] if not row.get('nativeCompositorRingAudited')])
        with self.assertRaises(ValueError):
            validate_reclaimed_staging_input_response(**data)
        pixels = bytearray(base['frames']['compositor-copy-2.raw'])
        pixels[0] ^= 255
        data = dict(base, frames=dict(base['frames'], **{'compositor-copy-2.raw': bytes(pixels)}))
        with self.assertRaises(ValueError):
            validate_reclaimed_staging_input_response(**data)
        data = dict(base, guest=re.sub(r'^.*OpenGL VENDOR:.*$', '', base['guest'], flags=re.M))
        with self.assertRaises(ValueError):
            validate_reclaimed_staging_input_response(**data)

    def test_input_peer_isolation_ack_and_exact_sources_still_mandatory(self):
        base = self.inputs[0]
        for label, key, value in (
            ('nativeInputChannelOpened', 'socketPeerVerified', False),
            ('nativeSharedTexturePresenterOpened', 'windowInputEnabled', True),
            ('nativeInputChannelStopped', 'acknowledgements', 32),
        ):
            records = copy.deepcopy(base['records'])
            next(row for row in records if label in row)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                delivery_metadata(base['guest'], records, base['source'], base['owner_output'])
        source = copy.deepcopy(base['source'])
        source['manifest']['sources']['staging_reclaim.h'] = '0' * 64
        with self.assertRaises(ValueError):
            delivery_metadata(base['guest'], base['records'], source, base['owner_output'])

    def test_original_usage_guard_failure_remains_rejected(self):
        previous = json.loads((self.repo / self.value['previousEvidence']['path']).read_bytes())
        row = next(row for row in previous['retainedFailures'] if row['run'] == 94)
        data = self.unpack(row, previous['wallpaperReference'])
        self.assertEqual(data['report']['hostBridgeExit'], 1)
        self.assertTrue(data['report']['ownedQemuControl']['qemuForcedStop'])
        self.assertGreater(data['report']['disconnectCleanup']['peakReportedGpuUsageBytes'], 1073741824)
        with self.assertRaises((ValueError, KeyError)):
            validate_qmp_input_response(**data)


if __name__ == '__main__':
    unittest.main()
