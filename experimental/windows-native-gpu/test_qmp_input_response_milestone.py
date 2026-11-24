"""Replay real GPU input responses and retain the distinction from Windows input."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib

from verify_qmp_input_delivery import delivery_metadata, validate_qmp_input_delivery
from verify_qmp_input_response import GUEST_SOURCES, confirmation_pixels, response_metadata, validate_qmp_input_response


class QmpInputResponse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        data = (cls.repo / 'docs/evidence/OMARCHY-4K-QMP-INPUT-RESPONSE-2026-10-20.json').read_bytes()
        assert hashlib.sha256(data).hexdigest() == 'd040db75cd4b5db5421aa95f1ac4a00e6a3f6615f2a5cd15b3cdfea3909c87b6'
        cls.value = json.loads(data)
        receipt = cls.value['rawBase']
        raw = (cls.repo / receipt['evidencePath']).read_bytes()
        assert hashlib.sha256(raw).hexdigest() == receipt['evidenceSha256']
        old = json.loads(raw)
        row = next(row for row in old['runs'] if row['run'] == receipt['run'])
        base = zlib.decompress(base64.b64decode(row['rawFramesZlibBase64'][receipt['frameFile']], validate=True))
        assert len(base) == 33177600 and hashlib.sha256(base).hexdigest() == receipt['rawSha256']
        cls.base_int = int.from_bytes(base, 'little')
        cls.inputs = [cls.unpack(row) for row in cls.value['runs']]

    @classmethod
    def unpack(cls, row):
        decode = lambda key: zlib.decompress(base64.b64decode(row[key], validate=True))
        frames = {}
        for name, receipt in row['rawFrames'].items():
            delta = zlib.decompress(base64.b64decode(receipt['xorZlibBase64'], validate=True))
            assert len(delta) == receipt['bytes'] == 33177600
            raw = (cls.base_int ^ int.from_bytes(delta, 'little')).to_bytes(len(delta), 'little')
            assert hashlib.sha256(raw).hexdigest() == receipt['rawSha256']
            frames[name] = raw
        return dict(report=json.loads(decode('reportZlibBase64')), guest=decode('guestZlibBase64').decode(),
                    owner_output=decode('ownerOutputZlibBase64').decode(), records=json.loads(decode('nativeRecordsZlibBase64')),
                    source=json.loads(decode('sourceZlibBase64')), frames=frames, wallpaper_reference=cls.value['wallpaperReference'])

    def test_one_actual_gpu_response_with_repeat_still_rejected(self):
        self.assertEqual([row['run'] for row in self.value['runs']], [93])
        self.assertFalse(self.value['repeatedInputResponseAccepted'])
        self.assertEqual(self.value['repeatFailureRun'], 94)
        for row, data in zip(self.value['runs'], self.inputs):
            proof = validate_qmp_input_response(**data)
            self.assertEqual(proof, row['audit'])
            self.assertTrue(proof['gpuCaptureWithQmpInputResponseVerified'])
            self.assertEqual(proof['inputDelivery']['acknowledgedCommands'], 33)
            self.assertTrue(proof['inputResponse']['diagnosticResponseRenderedVerified'])
            self.assertGreaterEqual(proof['inputResponse']['finalFrameConfirmationPixels'], 3000)
            for key in ('hostWindowIntegrationVerified', 'normalLauncherVerified', 'fullDesktopAccepted',
                        'allFramesDisplayedVerified', 'nearNativePerformanceVerified'):
                self.assertFalse(proof[key])
            self.assertTrue(proof['inputDelivery']['syntheticAtChannel'])
            self.assertFalse(proof['inputDelivery']['windowsMessagesVerified'])
            self.assertFalse(proof['inputDelivery']['pointerButtonDeliveryVerified'])
            self.assertFalse(proof['inputResponse']['exactGlyphOcrVerified'])

    def test_exact_build_image_and_auditor_provenance(self):
        for data in self.inputs:
            source, report = data['source'], data['report']
            self.assertEqual(report['driverBridgeSha256'], 'bb727730dc44326298e5b6ce4461c1510b008434da96b3a0b8b49988a646b73c')
            self.assertEqual(report['initramfsSha256'], '23aadee3e3feb7d05d75910c9ce88e6a001a432d2f73d803a2e0424509868943')
            self.assertEqual(source['sourceImageSha256Before'], source['sourceImageSha256After'])
            self.assertEqual(source['nativeBuildReceipt']['bridgeSha256'], report['driverBridgeSha256'])
            for name, digest in source['manifest']['sources'].items():
                if name.endswith(('.h', '.cpp')):
                    self.assertEqual(source['nativeBuildReceipt']['sources']['experimental/windows-native-gpu/' + name], digest)
        for name, digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(), digest)
        image = self.value['image']
        png = (self.repo / image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(), image['pngSha256'])
        self.assertEqual(struct.unpack('>II', png[16:24]), (3840, 2160))
        self.assertEqual((image['run'], image['frame'], image['pixelsEdited']), (93, 12, False))
        self.assertEqual(hashlib.sha256(self.inputs[0]['frames']['compositor-copy-2.raw']).hexdigest(), image['sourceRawSha256'])

    @staticmethod
    def replace_snapshot(guest, label, mutate):
        match = re.search(r'^' + re.escape(label) + r' ([0-9a-f]+)$', guest, re.M)
        value = json.loads(bytes.fromhex(match[1]))
        mutate(value)
        return guest[:match.start(1)] + json.dumps(value).encode().hex() + guest[match.end(1):]

    def metadata(self, data):
        return delivery_metadata(data['guest'], data['records'], data['source'], data['owner_output'], guest_sources=GUEST_SOURCES)

    def test_missing_foreign_pty_focus_and_cursor_rejected(self):
        base = self.inputs[0]
        edits = [
            ('OMARCHY_NATIVE_INPUT_PTY_HEX', lambda row: row.update(token='wrong')),
            ('OMARCHY_NATIVE_INPUT_PTY_HEX', lambda row: row.update(uid=0)),
            ('OMARCHY_NATIVE_INPUT_PTY_HEX', lambda row: row.update(terminalPid=1)),
            ('OMARCHY_NATIVE_INPUT_PTY_HEX', lambda row: row.update(pty='/tmp/fake-pty')),
            ('OMARCHY_NATIVE_INPUT_AFTER_ACTIVE_HEX', lambda row: row.update(pid=1)),
            ('OMARCHY_NATIVE_INPUT_AFTER_ACTIVE_HEX', lambda row: row.update(address='0x1')),
            ('OMARCHY_NATIVE_INPUT_AFTER_CURSOR_HEX', lambda row: row.update(x=1920, y=1080)),
            ('OMARCHY_NATIVE_INPUT_AFTER_CURSOR_HEX', lambda row: row.update(x=0, y=0)),
        ]
        for label, edit in edits:
            data = dict(base)
            data['guest'] = self.replace_snapshot(base['guest'], label, edit)
            with self.subTest(label=label), self.assertRaises(ValueError):
                self.metadata(data)
        for guest in (re.sub(r'^OMARCHY_NATIVE_INPUT_PTY_HEX .*\n', '', base['guest'], flags=re.M),
                      base['guest'] + '\n' + re.search(r'^OMARCHY_NATIVE_INPUT_PTY_HEX .*$', base['guest'], re.M)[0]):
            with self.assertRaises(ValueError):
                self.metadata(dict(base, guest=guest))

    def test_native_ownership_isolation_ack_and_sources_required(self):
        base = self.inputs[0]
        for label, key, value in (
            ('nativeInputChannelOpened', 'socketPeerVerified', False),
            ('nativeInputChannelOpened', 'dedicatedQmp', False),
            ('nativeInputChannelOpened', 'ownedQemuPid', 1),
            ('nativeSharedTexturePresenterOpened', 'inputSelfTestIsolated', False),
            ('nativeSharedTexturePresenterOpened', 'windowInputEnabled', True),
            ('nativeInputChannelStopped', 'acknowledgements', 32),
            ('nativeInputChannelStopped', 'workerJoined', False),
            ('nativeInputChannelStopped', 'beforeOwnedVmStop', False),
            ('nativeInputChannelStopped', 'resetCount', 0),
        ):
            data = dict(base, records=copy.deepcopy(base['records']))
            next(row for row in data['records'] if label in row)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.metadata(data)
        for name in ('bounded_input_channel.h', 'hyprland_startup_probe.c', 'gpu-terminal-scene.sh'):
            data = dict(base, source=copy.deepcopy(base['source']))
            data['source']['manifest']['sources'][name] = '0' * 64
            with self.subTest(source=name), self.assertRaises(ValueError):
                self.metadata(data)
        data = dict(base, source=copy.deepcopy(base['source']))
        data['source']['command'].remove('--driver-input-self-test')
        with self.assertRaises(ValueError):
            self.metadata(data)

    def test_complete_gpu_witnesses_and_buffer_bytes_still_required(self):
        base = self.inputs[0]
        data = dict(base, records=[row for row in base['records'] if not row.get('nativeCompositorRingAudited')])
        with self.assertRaises(ValueError):
            validate_qmp_input_response(**data)
        data = dict(base, frames=dict(base['frames']))
        pixels = bytearray(data['frames']['compositor-copy-2.raw'])
        pixels[0] ^= 255
        data['frames']['compositor-copy-2.raw'] = bytes(pixels)
        with self.assertRaises(ValueError):
            validate_qmp_input_response(**data)
        self.assertIn('OpenGL VENDOR:', base['guest'])
        data = dict(base, guest=re.sub(r'^.*OpenGL VENDOR:.*$', '', base['guest'], flags=re.M))
        with self.assertRaises(ValueError):
            validate_qmp_input_response(**data)

    def test_response_requires_pty_barrier_and_changed_gpu_pixels(self):
        base = self.inputs[0]
        line = re.search(r'^OMARCHY_NATIVE_INPUT_RESPONSE_BARRIER .*$', base['guest'], re.M)[0]
        for guest in (base['guest'].replace(line, ''), base['guest'] + '\n' + line,
                      base['guest'].replace('queuedFrames=2 gpuProof=false', 'queuedFrames=11 gpuProof=false'),
                      base['guest'].replace(line, re.sub(r'observedCommit=\d+', 'observedCommit=0', line))):
            with self.assertRaises(ValueError):
                response_metadata(guest, base['frames'])
        stale = dict(base['frames'], **{'compositor-copy-2.raw': base['frames']['compositor-copy-1.raw']})
        with self.assertRaises(ValueError):
            response_metadata(base['guest'], stale)
        swapped = {'compositor-copy-1.raw': base['frames']['compositor-copy-2.raw'],
                   'compositor-copy-2.raw': base['frames']['compositor-copy-1.raw']}
        with self.assertRaises(ValueError):
            response_metadata(base['guest'], swapped)

    def test_retained_failures_and_delivery_only_baseline_keep_scope(self):
        self.assertEqual([row['run'] for row in self.value['retainedFailures']], [90, 91, 94])
        for row in self.value['retainedFailures']:
            with self.assertRaises((ValueError, KeyError)):
                validate_qmp_input_response(**self.unpack(row))
            self.assertFalse(row['qmpInputDeliveryAccepted'])
        repeat = self.unpack(self.value['retainedFailures'][-1])
        self.assertEqual(repeat['report']['hostBridgeExit'], 1)
        self.assertTrue(repeat['report']['ownedQemuControl']['qemuForcedStop'])
        self.assertGreater(repeat['report']['disconnectCleanup']['peakReportedGpuUsageBytes'], 1073741824)
        self.assertEqual([row['run'] for row in self.value['baselines']], [92])
        baseline = self.unpack(self.value['baselines'][0])
        self.assertEqual(validate_qmp_input_delivery(**baseline), self.value['baselines'][0]['originalAudit'])
        clients = json.loads(bytes.fromhex(re.search(r'^OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX ([0-9a-f]+)$', baseline['guest'], re.M)[1]))
        self.assertEqual(confirmation_pixels(baseline['frames']['compositor-copy-2.raw'], clients[0])['pixels'], 0)
        with self.assertRaises(ValueError):
            validate_qmp_input_response(**baseline)

    def test_color_oracle_bounds_controls_are_not_gpu_proof(self):
        client = {'at': [1320, 693], 'size': [1200, 800]}
        raw = bytearray(3840 * 2160 * 4)
        color = bytes((96, 160, 32, 255))
        for y in range(1050, 1074):
            raw[(y * 3840 + 1340) * 4:(y * 3840 + 1740) * 4] = color * 400
        self.assertEqual(confirmation_pixels(raw, client), dict(pixels=9600, bounds=[1340, 1050, 1740, 1074], denseRows=24))
        self.assertEqual(confirmation_pixels(raw, {'at': [0, 0], 'size': [1200, 800]})['pixels'], 0)
        for client in ({'at': [-1, 0], 'size': [1200, 800]}, {'at': [1320, 693], 'size': [4000, 800]},
                       {'at': [0, 0], 'size': [True, 800]}):
            with self.assertRaises(ValueError):
                confirmation_pixels(raw, client)


if __name__ == '__main__':
    unittest.main()
