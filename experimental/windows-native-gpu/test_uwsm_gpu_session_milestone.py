"""Replay physical UWSM/NVIDIA captures and reject incomplete session evidence."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import unittest
import zlib

from verify_uwsm_gpu_session import CGROUP, session_metadata, validate_uwsm_gpu_capture


class UwsmGpuSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo / 'docs/evidence/OMARCHY-4K-UWSM-GPU-SESSION-2026-10-19.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '2766fb81bf0b1b9a3c3c6aeed49e377d0de6003e8d3287b532837ade36a32e78'
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

    def test_active_uwsm_gpu_session(self):
        self.assertEqual([row['run'] for row in self.value['runs']], [77])
        for row, data in zip(self.value['runs'], self.inputs):
            proof = validate_uwsm_gpu_capture(**data)
            self.assertEqual(proof, row['audit'])
            self.assertTrue(proof['uwsmManagedGpuCaptureVerified'])
            self.assertTrue(proof['session']['runningGraphicalTargetVerified'])
            self.assertFalse(proof['session']['gpuProof'])
            self.assertEqual((proof['capture']['gpuApplicationCycles'], proof['capture']['liveRetiredActors']), (5, 6))
            self.assertEqual(proof['capture']['deferredApertureMappings'], 0)
            for key in ('normalLauncherVerified', 'normalOmarchySessionVerified', 'fullDesktopAccepted', 'nearNativePerformanceVerified'):
                self.assertFalse(proof[key])
            self.assertFalse(proof['capture']['allFramesDisplayedVerified'])

    def test_actual_binary_image_auditor_and_pixel_provenance(self):
        for data in self.inputs:
            self.assertEqual(data['report']['driverBridgeSha256'], 'e6d3820c4ed8d29d96c9bbb36533c001f8f3290658d8eba133031463c00c9c2a')
            self.assertEqual(data['report']['initramfsSha256'], '249401129839ed0e3e9b192fefc3ab0f0687275567e1b1cf0be4cc4cdf7ebd99')
            self.assertEqual(data['report']['qemuSha256'], '4691955c77cb33ba656a7ad2b3cc97c7c514f8705dc46c5d328d641f38c8ba46')
            self.assertEqual(data['source']['sourceImageSha256Before'], data['source']['sourceImageSha256After'])
        for name, digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(), digest)
        image = self.value['image']
        png = (self.repo / image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(), image['pngSha256'])
        self.assertEqual(struct.unpack('>II', png[16:24]), (3840, 2160))
        self.assertEqual((image['run'], image['frame'], image['pixelsEdited']), (77, 12, False))
        self.assertEqual(hashlib.sha256(self.inputs[0]['frames']['compositor-copy-2.raw']).hexdigest(), image['sourceRawSha256'])

    def test_missing_or_forged_session_state_is_not_gpu_evidence(self):
        original = self.inputs[0]
        pid = self.value['runs'][0]['audit']['capture']['compositorPid']
        for mutation in ('root', 'pid', 'uid', 'pam', 'state', 'cgroup', 'missing', 'scope', 'fixture'):
            guest = original['guest']
            source = copy.deepcopy(original['source'])
            if mutation == 'root': guest = guest.replace('rootChannel=true pinnedPidfd=true', 'rootChannel=false pinnedPidfd=true')
            if mutation == 'pid': guest = guest.replace(f'MainPID={pid}\n', f'MainPID={pid + 1}\n')
            if mutation == 'uid': guest = re.sub(r'(UWSM_COMPOSITOR_LOGIN pid=\d+) uid=1000', r'\1 uid=0', guest)
            if mutation == 'pam': guest = re.sub(r'(UWSM_COMPOSITOR_LOGIN .+ sessionId=)[A-Za-z0-9_-]+', r'\1forged', guest)
            if mutation == 'state':
                begin = guest.index('NORMAL_ROOT_UWSM_STATE_BEGIN')
                guest = guest[:begin] + guest[begin:].replace('ActiveState=active', 'ActiveState=inactive', 1)
            if mutation == 'cgroup': guest = guest.replace(CGROUP, CGROUP + '-foreign')
            if mutation == 'missing': guest = re.sub(r'^NORMAL_ROOT_UWSM_STATE_BEGIN.*?^NORMAL_ROOT_UWSM_STATE_END[^\n]*\n', '', guest, flags=re.M | re.S)
            if mutation == 'scope': source['manifest']['normalOmarchySessionVerified'] = True
            if mutation == 'fixture': source['manifest']['uwsmManagedProbe'] = False
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                session_metadata(guest, source, pid)

    def test_gpu_retirement_and_pixels_remain_mandatory(self):
        for mutation in ('witness', 'cpuScan', 'pixels'):
            data = copy.deepcopy(self.inputs[0])
            if mutation == 'witness': data['guest'] = data['guest'].replace('compositorStillRunning=true rootOwnedWitness=true', 'compositorStillRunning=false rootOwnedWitness=false')
            if mutation == 'cpuScan': data['report']['disconnectCleanup']['cpuAllocationFingerprintBytes'] = 1
            if mutation == 'pixels':
                raw = data['frames']['compositor-copy-2.raw']
                data['frames']['compositor-copy-2.raw'] = bytes([raw[0] ^ 1]) + raw[1:]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): validate_uwsm_gpu_capture(**data)

    def test_startup_receipt_order_and_early_state_failures_stay_rejected(self):
        self.assertEqual([row['run'] for row in self.value['retainedFailures']], [69, 70, 71, 72, 73, 75, 76, 78])
        for row in self.value['retainedFailures']:
            # Decode failed buffers only while needed, keeping CI memory bounded.
            data = self.unpack(row, row['run'] == 76)
            self.assertFalse(row['uwsmManagedGpuCaptureAccepted'])
            self.assertTrue(data['source']['sourceImageUnchanged'])
            with self.subTest(run=row['run']), self.assertRaises((ValueError, KeyError)):
                validate_uwsm_gpu_capture(**data)
