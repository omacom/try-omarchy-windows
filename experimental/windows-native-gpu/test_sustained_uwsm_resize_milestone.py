"""Replay real 300-frame application resizing without inferring desktop acceptance."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib

from verify_sustained_uwsm_gpu import native_frame_sequence, validate_sustained_uwsm_gpu_capture
from verify_sustained_uwsm_resize import (
    RESIZE_SOURCE, resize_metadata, resize_pixels, validate_resize_uwsm_gpu_capture,
)


class SustainedUwsmResize(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo / 'docs/evidence/OMARCHY-4K-SUSTAINED-RESIZE-2026-10-20.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == 'bc8420c380b943d94bd3ed78869f0d822d5ebaaac4a23ac6e2a027580f3de48a'
        cls.value = json.loads(raw)
        receipt = cls.value['rawBase']
        raw = (cls.repo / receipt['evidencePath']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == receipt['evidenceSha256']
        row = next(row for row in json.loads(raw)['runs'] if row['run'] == receipt['run'])
        pixels = zlib.decompress(base64.b64decode(row['rawFramesZlibBase64'][receipt['frameFile']], validate=True))
        assert len(pixels) == 33177600 and hashlib.sha256(pixels).hexdigest() == receipt['rawSha256']
        cls.base_int = int.from_bytes(pixels, 'little')
        cls.inputs = [cls.unpack(row) for row in cls.value['runs']]
        cls.baselines = [cls.unpack(row) for row in cls.value['baselines']]

    @classmethod
    def unpack(cls, row):
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
                    wallpaper_reference=cls.value['wallpaperReference'])

    def test_two_actual_gpu_resize_runs_with_all_300_handoffs(self):
        self.assertEqual([row['run'] for row in self.value['runs']], [99, 100])
        self.assertTrue(self.value['bounded300FrameApplicationResizeRepeated'])
        self.assertFalse(self.value['hardwareStagingReallocationObserved'])
        for row, data in zip(self.value['runs'], self.inputs):
            proof = validate_resize_uwsm_gpu_capture(**data, resize_reference=self.value['resizeReference'])
            self.assertEqual(proof, row['audit'])
            self.assertEqual(proof['frameSequence']['actualGpuHandoffs'], 300)
            self.assertEqual(proof['frameSequence']['ordinaryGpuOnlyHandoffs'], 298)
            self.assertEqual(proof['resizePixels']['firstPanelBackgroundMatches'], 328)
            self.assertTrue(proof['resize']['ordinaryHandoffsContinued'])
            self.assertEqual([phase['size'] for phase in proof['resize']['phases']], [[1440, 960], [1600, 1000], [1200, 800]])
            self.assertEqual(proof['staging']['reallocatedTextures'], 0)
            for key in ('hostWindowIntegrationVerified', 'normalLauncherVerified', 'fullDesktopAccepted',
                        'allFramesDisplayedVerified', 'nearNativePerformanceVerified'):
                self.assertFalse(proof[key])
            self.assertFalse(proof['resizePixels']['middleResizePixelsVerified'])

    def test_baselines_remain_300_frame_input_response_only(self):
        self.assertEqual([row['run'] for row in self.value['baselines']], [97, 98])
        for row, data in zip(self.value['baselines'], self.baselines):
            self.assertEqual(validate_sustained_uwsm_gpu_capture(**data), row['audit'])
            with self.assertRaises(ValueError):
                resize_metadata(data['guest'], data['source'])
        self.assertTrue(self.value['baselines'][0]['audit']['staging']['safeReclamationObserved'])
        self.assertFalse(self.value['baselines'][1]['audit']['staging']['safeReclamationObserved'])
        prior = self.value['previousEvidence']
        self.assertEqual(prior['retainedEarlierFailureRun'], 94)
        self.assertEqual(hashlib.sha256((self.repo / prior['path']).read_bytes()).hexdigest(), prior['sha256'])

    def test_build_source_image_auditor_and_default_mode_provenance(self):
        for data in self.inputs:
            source, report = data['source'], data['report']
            self.assertEqual(report['driverBridgeSha256'], '0f98c12a948693ebb7721d64f700fea4dcf0eb53a9fefd7e82f933286df7a52d')
            self.assertEqual(report['initramfsSha256'], '1d6d2f661faa19913faad315fc2fb9ec0cd4c5e8286b2e1766e61f966c1557cc')
            self.assertEqual(source['sourceImageSha256Before'], source['sourceImageSha256After'])
            self.assertEqual(source['manifest']['sources']['hyprland_startup_probe.c'], RESIZE_SOURCE)
            for name, digest in source['manifest']['sources'].items():
                if name.endswith(('.h', '.cpp')):
                    self.assertEqual(source['nativeBuildReceipt']['sources']['experimental/windows-native-gpu/' + name], digest)
        self.assertEqual(hashlib.sha256(Path(__file__).with_name('hyprland_startup_probe.c').read_bytes()).hexdigest(), RESIZE_SOURCE)
        for name, digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(), digest)
        controls = self.value['controls']['defaultGuestBuilds']
        self.assertEqual(controls['sourceSha256'], RESIZE_SOURCE)
        self.assertEqual([row['name'] for row in controls['builds']], ['startup', 'scene12'])
        self.assertTrue(all(row['exit'] == 0 and '-Werror' in row['command'] for row in controls['builds']))
        for image, frame in zip(self.value['images'], (1, 300)):
            png = (self.repo / image['path']).read_bytes()
            self.assertEqual(hashlib.sha256(png).hexdigest(), image['pngSha256'])
            self.assertEqual(struct.unpack('>II', png[16:24]), (3840, 2160))
            self.assertEqual((image['run'], image['frame'], image['pixelsEdited']), (100, frame, False))
            suffix = 1 if frame == 1 else 2
            self.assertEqual(hashlib.sha256(self.inputs[1]['frames'][f'compositor-copy-{suffix}.raw']).hexdigest(), image['sourceRawSha256'])

    def test_missing_duplicate_mixed_and_cpu_frame_receipts_rejected(self):
        base = self.inputs[0]
        for label, frame, key, value in (
            ('nativeCompositorRingFrame', 150, 'cpuReadback', True),
            ('nativeCompositorRingInput', 150, 'sourceCopyRetired', False),
            ('nativeCompositorWindowSubmitted', 150, 'lastPresentCount', 149),
            ('nativeCompositorWindowSubmitted', 150, 'presentSwapChain', 1),
            ('nativeCompositorFrameSlotReady', 150, 'maximumFrameLatency', 2),
        ):
            records = copy.deepcopy(base['records'])
            next(row for row in records if row.get(label) and row.get('frame') == frame)[key] = value
            with self.subTest(label=label, key=key), self.assertRaises(ValueError):
                native_frame_sequence(base['guest'], records)
        records = [row for row in base['records'] if not (row.get('nativeCompositorRingFrame') and row['frame'] == 150)]
        with self.assertRaises(ValueError):
            native_frame_sequence(base['guest'], records)
        records = base['records'] + [next(row for row in base['records'] if row.get('nativeCompositorRingInput'))]
        with self.assertRaises(ValueError):
            native_frame_sequence(base['guest'], records)

    def test_twelve_frame_manifest_and_truncated_stream_cannot_pass(self):
        base = self.inputs[0]
        data = dict(base, source=copy.deepcopy(base['source']))
        data['source']['manifest']['expectedFrames'] = 12
        with self.assertRaises(ValueError):
            validate_resize_uwsm_gpu_capture(**data, resize_reference=self.value['resizeReference'])
        for guest in (base['guest'].replace('frames=300 target=300 failed=0', 'frames=12 target=12 failed=0'),
                      re.sub(r'^HYPRLAND_HOST_QUEUE_RESULT .*frame=150 .*\n', '', base['guest'], flags=re.M)):
            with self.assertRaises(ValueError):
                native_frame_sequence(guest, base['records'])

    def test_resize_identity_geometry_order_and_opt_in_are_required(self):
        base = self.inputs[0]
        for old, new in (('phase=2 pid=', 'phase=3 pid='), ('width=1600 height=1000', 'width=1440 height=960'),
                         ('beforeQueued=50', 'beforeQueued=0'), ('afterQueued=55', 'afterQueued=50')):
            guest = base['guest'].replace(old, new)
            self.assertNotEqual(guest, base['guest'])
            with self.assertRaises(ValueError):
                resize_metadata(guest, base['source'])
        label = 'OMARCHY_GPU_RESIZE_2_AFTER_ACTIVE_HEX'
        match = re.search(r'^' + label + r' ([0-9a-f]+)$', base['guest'], re.M)
        value = json.loads(bytes.fromhex(match[1]))
        value['pid'] = 1
        guest = base['guest'][:match.start(1)] + json.dumps(value).encode().hex() + base['guest'][match.end(1):]
        with self.assertRaises(ValueError):
            resize_metadata(guest, base['source'])
        source = copy.deepcopy(base['source'])
        del source['manifest']['applicationResizeProbe']
        with self.assertRaises(ValueError):
            resize_metadata(base['guest'], source)

    def test_actual_panel_and_restored_wallpaper_pixels_are_required(self):
        data = self.inputs[0]
        reference = self.value['resizeReference']
        for frames in (
            dict(data['frames'], **{'compositor-copy-1.raw': data['frames']['compositor-copy-2.raw']}),
            dict(data['frames'], **{'compositor-copy-2.raw': data['frames']['compositor-copy-1.raw']}),
        ):
            with self.assertRaises(ValueError):
                resize_pixels(frames, reference)
        altered = copy.deepcopy(reference)
        altered['probes'][0] = altered['probes'][1]
        with self.assertRaises(ValueError):
            resize_pixels(data['frames'], altered)
        pixels = bytearray(data['frames']['compositor-copy-1.raw'])
        for x, y, *_ in reference['probes'][:40]:
            pixels[(y * 3840 + x) * 4:(y * 3840 + x) * 4 + 3] = b'\x00\x00\x00'
        with self.assertRaises(ValueError):
            resize_pixels(dict(data['frames'], **{'compositor-copy-1.raw': bytes(pixels)}), reference)

    def test_complete_gpu_witnesses_and_unchanged_byte_guards_remain_required(self):
        base = self.inputs[0]
        data = dict(base, records=[row for row in base['records'] if not row.get('nativeCompositorRingAudited')])
        with self.assertRaises(ValueError):
            validate_resize_uwsm_gpu_capture(**data, resize_reference=self.value['resizeReference'])
        data = dict(base, report=copy.deepcopy(base['report']))
        data['report']['disconnectCleanup']['reportedGpuUsageLimitBytes'] *= 2
        with self.assertRaises(ValueError):
            validate_resize_uwsm_gpu_capture(**data, resize_reference=self.value['resizeReference'])
        data = dict(base, frames=dict(base['frames']))
        pixels = bytearray(data['frames']['compositor-copy-2.raw'])
        pixels[0] ^= 255
        data['frames']['compositor-copy-2.raw'] = bytes(pixels)
        with self.assertRaises(ValueError):
            validate_resize_uwsm_gpu_capture(**data, resize_reference=self.value['resizeReference'])


if __name__ == '__main__':
    unittest.main()
