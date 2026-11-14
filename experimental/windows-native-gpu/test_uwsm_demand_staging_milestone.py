"""Replay guarded staging and terminal-primary cleanup on actual NVIDIA captures."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import struct
import unittest
import zlib

from verify_uwsm_demand_staging import (
    staging_admission, terminal_retirement, validate_demand_staging,
    validate_terminal_demand_staging,
)


class UwsmDemandStaging(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo / 'docs/evidence/OMARCHY-4K-DEMAND-STAGING-2026-10-19.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '1d5b04214e8d9e056cfa6aac90f405387f54f4b54523031a7b8f6367317d67aa'
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

    def test_two_fresh_full_gpu_captures(self):
        self.assertEqual([row['run'] for row in self.value['runs']], [81, 82])
        for row, data in zip(self.value['runs'], self.inputs):
            proof = validate_terminal_demand_staging(**data)
            self.assertEqual(proof, row['audit'])
            self.assertTrue(proof['terminalDemandStagingCaptureVerified'])
            self.assertEqual(proof['terminal']['retainedActors'], 0)
            self.assertEqual(proof['uwsm']['capture']['gpuApplicationCycles'], 5)
            self.assertFalse(proof['fullDesktopAccepted'])
            self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_binary_source_auditor_and_pixel_provenance(self):
        for data in self.inputs:
            source, report = data['source'], data['report']
            self.assertEqual(report['driverBridgeSha256'], 'be6c6a9e672e5074985e4d91c779c73692d04feff5ee882f2002cf93873b343b')
            self.assertEqual(report['initramfsSha256'], 'bac5995069638c151abd0b2fac7331a892ac7ef86cb761b70c05d193dbc44e95')
            self.assertEqual(report['driverBridgeSha256'], source['nativeBuildReceipt']['bridgeSha256'])
            self.assertEqual(source['sourceImageSha256Before'], source['sourceImageSha256After'])
            for name, digest in source['manifest']['sources'].items():
                if name.endswith(('.h', '.cpp')):
                    self.assertEqual(source['nativeBuildReceipt']['sources']['experimental/windows-native-gpu/' + name], digest)
        for name, digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest(), digest)
        image = self.value['image']
        png = (self.repo / image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(png).hexdigest(), image['pngSha256'])
        self.assertEqual(struct.unpack('>II', png[16:24]), (3840, 2160))
        self.assertEqual((image['run'], image['frame'], image['pixelsEdited']), (82, 12, False))
        self.assertEqual(hashlib.sha256(self.inputs[1]['frames']['compositor-copy-2.raw']).hexdigest(), image['sourceRawSha256'])

    def test_staging_requires_own_ordered_guards_and_exact_reclamation(self):
        original = self.inputs[0]
        for mutation in ('guard', 'limit', 'allocation', 'beforeUse', 'initial', 'bytes', 'released', 'retry', 'frameOrder'):
            records = copy.deepcopy(original['records'])
            backpressure = original['report']['disconnectCleanup']['compositorBackpressure']
            allocation = next(row for row in records if row.get('nativeCompositorStagingAllocated') is True)
            opened = next(row for row in records if row.get('nativeCompositorRingOpened') is True)
            stopped = next(row for row in records if row.get('nativeCompositorRingStopped') is True)
            check = next(row for row in records if row.get('stage') == 'staging-before')
            if mutation == 'guard': check['accepted'] = False
            if mutation == 'limit': check['limitBytes'] += 1
            if mutation == 'allocation': records.remove(allocation)
            if mutation == 'beforeUse':
                records.remove(allocation)
                records.append(allocation)
            if mutation == 'initial': opened['initialStagingTextures'] = 3
            if mutation == 'bytes': allocation['preparedBytes'] += 1
            if mutation == 'released': stopped['stagingTexturesReleased'] -= 1
            if mutation == 'retry': backpressure += 1
            if mutation == 'frameOrder':
                row = next(row for row in records if row.get('stage') == 'frame-admitted')
                records.remove(row)
                records.append(row)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                staging_admission(records, backpressure)

    def test_terminal_cleanup_requires_actual_drain_and_sealed_ownership(self):
        original = self.inputs[0]
        pid = self.value['runs'][0]['audit']['uwsm']['session']['compositorPid']
        for mutation in ('pidfd', 'missing', 'early', 'remaining', 'drain', 'sealed', 'batch', 'actor', 'counter'):
            records, guest = copy.deepcopy(original['records']), original['guest']
            step = next(row for row in records if row.get('nativeStoppedGroupRetirement') is True)
            group = next(row for row in records if row.get('nativeCoordinatorSummary') is True)
            ring = next(row for row in records if row.get('nativeCompositorRingStopped') is True)
            if mutation == 'pidfd': guest = guest.replace('primaryPidfdExited=true', 'primaryPidfdExited=false')
            if mutation == 'missing': guest = guest.replace('BROKER_COORDINATOR_GROUP_STOPPED ', 'MISSING_GROUP_STOPPED ')
            if mutation == 'early':
                line = next(line for line in guest.splitlines() if line.startswith('BROKER_COORDINATOR_GROUP_STOPPED '))
                guest = line + '\n' + guest.replace(line + '\n', '')
            if mutation == 'remaining': group['liveObjectsBeforeDisconnect'] = 1
            if mutation == 'drain':
                records.remove(ring)
                records.append(ring)
            if mutation == 'sealed': step['groupSealed'] = False
            if mutation == 'batch': step['attempted'] = 17
            if mutation == 'actor': step['primaryActor'] += 1
            if mutation == 'counter': step['destroyed'] += 1
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                terminal_retirement(guest, records, pid)

    def test_original_gpu_and_live_lifecycle_checks_remain_mandatory(self):
        for mutation in ('witness', 'cpuScan', 'pixels'):
            data = copy.deepcopy(self.inputs[0])
            if mutation == 'witness':
                data['guest'] = data['guest'].replace('compositorStillRunning=true rootOwnedWitness=true', 'compositorStillRunning=false rootOwnedWitness=false')
            if mutation == 'cpuScan':
                data['report']['disconnectCleanup']['cpuAllocationFingerprintBytes'] = 1
            if mutation == 'pixels':
                raw = data['frames']['compositor-copy-2.raw']
                data['frames']['compositor-copy-2.raw'] = bytes([raw[0] ^ 1]) + raw[1:]
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_terminal_demand_staging(**data)

    def test_previous_failure_and_corrected_baseline_keep_their_scope(self):
        self.assertEqual([row['run'] for row in self.value['retainedFailures']], [79])
        for row in self.value['retainedFailures']:
            self.assertEqual(row['currentAuditRejection'], 'Native child resources still depend on VM exit')
            with self.assertRaises(ValueError):
                validate_terminal_demand_staging(**self.unpack(row, True))
        self.assertEqual([row['run'] for row in self.value['preTerminalBaselines']], [80])
        for row in self.value['preTerminalBaselines']:
            data = self.unpack(row, True)
            self.assertEqual(validate_demand_staging(**data), row['audit'])
            self.assertEqual(row['originalAuditRejection']['error'], 'Missing per-frame/import guard')
            with self.assertRaises(ValueError):
                validate_terminal_demand_staging(**data)
        previous = self.value['previousEvidence']
        raw = (self.repo / previous['path']).read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), previous['sha256'])
        self.assertEqual(previous['retainedRejectedRuns'], [78])
        row = next(row for row in json.loads(raw)['retainedFailures'] if row['run'] == 78)
        self.assertFalse(row['uwsmManagedGpuCaptureAccepted'])


if __name__ == '__main__':
    unittest.main()
