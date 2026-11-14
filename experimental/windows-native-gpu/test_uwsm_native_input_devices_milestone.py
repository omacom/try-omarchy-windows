"""Replay GPU captures with real libinput discovery; never infer typed input."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib

from verify_uwsm_native_input_devices import input_discovery, validate_gpu_input_discovery


class NativeInputDiscovery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo / 'docs/evidence/OMARCHY-4K-NATIVE-INPUT-DISCOVERY-2026-10-20.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '95977f4d8d670c9c90c5d87108964337af22ac9e0ef07e780fb73873a6f91d4b'
        cls.value = json.loads(raw)
        receipt = cls.value['rawBase']
        old_raw = (cls.repo / receipt['evidencePath']).read_bytes()
        assert hashlib.sha256(old_raw).hexdigest() == receipt['evidenceSha256']
        old = json.loads(old_raw)
        row = next(row for row in old['runs'] if row['run'] == receipt['run'])
        base = zlib.decompress(base64.b64decode(row['rawFramesZlibBase64'][receipt['frameFile']], validate=True))
        assert len(base) == 33177600 and hashlib.sha256(base).hexdigest() == receipt['rawSha256']
        cls.base_int = int.from_bytes(base, 'little')
        cls.inputs = [cls.unpack(row, True) for row in cls.value['runs']]

    @classmethod
    def unpack(cls, row, pixels=False):
        decode = lambda key: zlib.decompress(base64.b64decode(row[key], validate=True))
        frames = {}
        if pixels:
            for name, receipt in row['rawFrames'].items():
                delta = zlib.decompress(base64.b64decode(receipt['xorZlibBase64'], validate=True))
                assert len(delta) == receipt['bytes'] == 33177600
                raw = (cls.base_int ^ int.from_bytes(delta, 'little')).to_bytes(len(delta), 'little')
                assert hashlib.sha256(raw).hexdigest() == receipt['rawSha256']
                frames[name] = raw
        return dict(report=json.loads(decode('reportZlibBase64')), guest=decode('guestZlibBase64').decode(),
                    records=json.loads(decode('nativeRecordsZlibBase64')), source=json.loads(decode('sourceZlibBase64')),
                    frames=frames, wallpaper_reference=cls.value['wallpaperReference'])

    def test_two_complete_nvidia_captures_with_input_discovery(self):
        self.assertEqual([row['run'] for row in self.value['runs']], [88, 89])
        for row, data in zip(self.value['runs'], self.inputs):
            proof = validate_gpu_input_discovery(**data)
            self.assertEqual(proof, row['audit'])
            self.assertTrue(proof['gpuCaptureWithInputEnumerationVerified'])
            self.assertTrue(proof['inputDiscovery']['guestInputDevicesEnumerated'])
            self.assertFalse(proof['inputDiscovery']['gpuProof'])
            for key in ('typedInputVerified', 'pointerDeliveryVerified', 'hostWindowIntegrationVerified',
                        'normalLauncherVerified', 'fullDesktopAccepted', 'nearNativePerformanceVerified'):
                self.assertFalse(proof[key])

    def test_actual_host_guest_library_auditor_and_frame_provenance(self):
        for data in self.inputs:
            source, report = data['source'], data['report']
            self.assertEqual(report['driverBridgeSha256'], 'c6c5769ca7c0022b56bdb81f92fd1f20aa329222cb17fa046acb9321eb03ff7a')
            self.assertEqual(report['initramfsSha256'], 'e6b6a8db589ccd8244752ca9523fb1ec13420b8bb840c67cdac0e1ddd989295a')
            self.assertEqual(report['driverBridgeSha256'], source['nativeBuildReceipt']['bridgeSha256'])
            self.assertEqual(source['sourceImageSha256Before'], source['sourceImageSha256After'])
            self.assertEqual(source['manifest']['aquamarineInputBuild']['sourceHashes']['src/backend/Backend.cpp'],
                             '74d4cba8989336a259026d8c96694bf406d7d56aec820e35b9c95ce291ff9e80')
            for name, digest in source['manifest']['sources'].items():
                if name.endswith(('.h', '.cpp')):
                    self.assertEqual(source['nativeBuildReceipt']['sources']['experimental/windows-native-gpu/' + name], digest)
        for name, digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(), digest)
        image = self.value['image']
        png = (self.repo / image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(), image['pngSha256'])
        self.assertEqual(struct.unpack('>II', png[16:24]), (3840, 2160))
        self.assertEqual((image['run'], image['frame'], image['pixelsEdited']), (89, 12, False))
        self.assertEqual(hashlib.sha256(self.inputs[1]['frames']['compositor-copy-2.raw']).hexdigest(), image['sourceRawSha256'])

    def test_missing_foreign_replayed_devices_and_sources_are_rejected(self):
        original = self.inputs[0]
        label = 'OMARCHY_NATIVE_INPUT_DEVICES_HEX'
        encoded = re.search(r'^' + label + r' ([0-9a-f]+)$', original['guest'], re.M).group(1)
        for mutation in ('keyboard', 'pointer', 'layout', 'identity', 'fixture', 'source', 'alias', 'library', 'missing', 'duplicate'):
            guest, source = original['guest'], copy.deepcopy(original['source'])
            devices = json.loads(bytes.fromhex(encoded))
            if mutation == 'keyboard': devices['keyboards'].pop(0)
            if mutation == 'pointer': devices['mice'][0]['name'] = 'foreign-pointer'
            if mutation == 'layout': devices['keyboards'][0]['layout'] = 'de'
            if mutation == 'identity': devices['keyboards'][0]['address'] = devices['mice'][0]['address']
            if mutation in ('keyboard', 'pointer', 'layout', 'identity'):
                guest = guest.replace(encoded, json.dumps(devices).encode().hex(), 1)
            if mutation == 'fixture': source['manifest']['nativeInputProbe'] = False
            if mutation == 'source': source['manifest']['aquamarineInputBuild']['sourceHashes']['src/backend/Backend.cpp'] = '0' * 64
            if mutation == 'alias': source['manifest']['files']['lib/libaquamarine.so.14']['sha256'] = '0' * 64
            if mutation == 'library': guest = guest.replace('/usr/lib/x86_64-linux-gnu/libaquamarine.so.14'.encode().hex(), '/foreign/libaquamarine.so.14'.encode().hex())
            if mutation == 'missing': guest = guest.replace(label + ' ', 'MISSING_INPUT_SNAPSHOT ')
            if mutation == 'duplicate':
                line = next(line for line in guest.splitlines() if line.startswith('OMARCHY_NATIVE_INPUT_LIBRARY_HEX '))
                guest += '\n' + line + '\n'
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                input_discovery(guest, source)

    def test_duplicate_seat_missing_libinput_or_kernel_witness_is_rejected(self):
        original = self.inputs[0]
        for mutation in ('reuse', 'duplicate', 'conflict', 'count', 'keyboard', 'vendor', 'kernel'):
            guest = original['guest']
            if mutation == 'reuse':
                marker = 'Private native-render existing seat input reused; libseat=true libinput=true KMS=false'.encode().hex()
                guest = guest.replace(marker, b'missing seat reuse'.hex())
            if mutation == 'duplicate':
                guest += '\nOMARCHY_NATIVE_INPUT_BACKEND_LOG_HEX ' + "Seat opened with backend 'logind'".encode().hex() + '\n'
            if mutation == 'conflict':
                guest += '\nOMARCHY_NATIVE_INPUT_BACKEND_LOG_HEX ' + b'Device already taken'.hex() + '\n'
            if mutation == 'count':
                guest = guest.replace(b'Private native-render input device count 5'.hex(), b'Private native-render input device count 4'.hex())
            if mutation == 'keyboard':
                guest = guest.replace(b'libinput: New device QEMU Virtio Keyboard: 1575-1'.hex(), b'missing keyboard'.hex())
            if mutation == 'vendor':
                guest = guest.replace(b'libinput: New device QEMU Virtio Tablet: 1575-3'.hex(), b'libinput: New device QEMU Virtio Tablet: 1575-9'.hex())
            if mutation == 'kernel':
                guest = guest.replace('Watching system buttons on /dev/input/', 'MISSING_KERNEL_OBSERVATION /dev/input/')
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                input_discovery(guest, original['source'])

    def test_device_metadata_cannot_replace_actual_gpu_pixels_and_lifetimes(self):
        for mutation in ('witness', 'cpuScan', 'pixels'):
            data = copy.deepcopy(self.inputs[0])
            if mutation == 'witness':
                data['guest'] = data['guest'].replace('compositorStillRunning=true rootOwnedWitness=true', 'compositorStillRunning=false rootOwnedWitness=false')
            if mutation == 'cpuScan': data['report']['disconnectCleanup']['cpuAllocationFingerprintBytes'] = 1
            if mutation == 'pixels':
                raw = data['frames']['compositor-copy-2.raw']
                data['frames']['compositor-copy-2.raw'] = bytes([raw[0] ^ 1]) + raw[1:]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_gpu_input_discovery(**data)

    def test_failed_input_attempts_and_early_dispatch_crash_remain_rejected(self):
        self.assertEqual([row['run'] for row in self.value['retainedFailures']], [83, 84, 85, 86, 87])
        for row in self.value['retainedFailures']:
            self.assertFalse(row['gpuInputDiscoveryAccepted'])
            with self.subTest(run=row['run']), self.assertRaises(ValueError):
                validate_gpu_input_discovery(**self.unpack(row, True))
        for directory, files in self.value['failedInputSourceLayers'].items():
            for name, receipt in files.items():
                raw = zlib.decompress(base64.b64decode(receipt['sourceZlibBase64'], validate=True))
                self.assertEqual(hashlib.sha256(raw).hexdigest(), receipt['sha256'], (directory, name))
        early = next(row for row in self.value['retainedFailures'] if row['run'] == 87)
        self.assertEqual(early['rawFrames'], {})
        self.assertEqual(early['originalAudits']['terminal-staging-audit-v1.json']['frameCount'], 0)

    def test_notification_overlap_keeps_the_original_wallpaper_failure(self):
        row = next(row for row in self.value['retainedFailures'] if row['run'] == 85)
        self.assertEqual(row['currentAuditRejection'], 'Captured wallpaper does not match independent shipped-image samples')
        data = self.unpack(row, True)
        raw = data['frames']['compositor-copy-2.raw']
        for x, y in ((3648, 96), (3744, 96)):
            self.assertEqual(raw[(y * 3840 + x) * 4:(y * 3840 + x) * 4 + 3], bytes(3))
        with self.assertRaisesRegex(ValueError, 'Captured wallpaper'):
            validate_gpu_input_discovery(**data)


if __name__ == '__main__':
    unittest.main()
