"""Replay real failures and require complete pixels for legacy compatibility."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from aperture_admission_audit import admission_snapshots
from verify_four_actor_legacy_desktop import validate_legacy_desktop
from verify_managed_mapping_desktop import validate_managed_mapping_desktop
from verify_multiwindow_attempt import validate_rejected_multiwindow
from verify_real_sdl_desktop import bgra_to_rgba, linear_blit


class MultiwindowAttempts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo/'docs/evidence/OMARCHY-MULTIWINDOW-ATTEMPTS-2026-10-23.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '9e642fdaaf7aea75a5a8c6ed0c0b1a51c63d35022ce959d246b79e0589f2e0b6'
        cls.value = json.loads(raw); cls.bases = []; item = cls.value
        while 'baseEvidence' in item:
            link = item['baseEvidence']; raw = (cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest() == link['sha256']
            item = json.loads(raw); cls.bases.append(item)

    @staticmethod
    @lru_cache(maxsize=10)
    def blob(digest):
        cls = MultiwindowAttempts
        entry = next(b['blobs'][digest] for b in [cls.value,*cls.bases] if digest in b['blobs'])
        raw = zlib.decompress(base64.b64decode(entry['zlibBase64'],validate=True))
        if 'xorBase' in entry:
            other = cls.blob(entry['xorBase']); assert len(raw) == len(other)
            raw = (int.from_bytes(raw,'little') ^ int.from_bytes(other,'little')).to_bytes(len(raw),'little')
        assert len(raw) == entry['bytes'] and hashlib.sha256(raw).hexdigest() == digest == entry['sha256']
        return raw

    @classmethod
    def row(cls, number): return next(r for r in cls.value['runs'] if r['run'] == number)

    @classmethod
    @lru_cache(maxsize=1)
    def inputs(cls, number, with_frames=False):
        row = cls.row(number); frames = {}; records = []
        def frame(name):
            if name not in frames:
                item = row['frames'][name]
                if 'blob' in item: raw = cls.blob(item['blob'])
                elif item['transform'] == 'bgra-to-rgba': raw = bgra_to_rgba(frame(item['native']))
                else:
                    assert item['transform'] == 'xor-linear-reference'
                    reference = linear_blit(frame(item['source']),3840,2160,item['width'],item['height'])
                    delta = cls.blob(item['deltaBlob']); assert len(delta) == len(reference)
                    raw = (int.from_bytes(reference,'little') ^ int.from_bytes(delta,'little')).to_bytes(len(delta),'little')
                assert len(raw) == item['bytes'] and hashlib.sha256(raw).hexdigest() == item['sha256']
                frames[name] = raw
            return frames[name]
        for line in cls.blob(row['files']['driver.errors.log']).decode('utf-8').splitlines():
            try: records.append(json.loads(line))
            except ValueError: pass
        result = dict(report=json.loads(cls.blob(row['files']['driver.json'])),
            source=json.loads(cls.blob(row['files']['source.json'])),records=records,
            guest=cls.blob(row['files']['driver.combined-guest.log']).decode('utf-8').replace('\r\n','\n').replace('\r','\n'),
            uart=cls.blob(row['files']['driver.log']),trace=cls.blob(row['files']['graphics-virtio.trace']))
        if with_frames:
            for name in row['frames']: frame(name)
            observations = [r for line in cls.blob(row['files']['driver.cadence.jsonl']).decode('utf-8').splitlines()
                if (r := json.loads(line)).get('event') == 'virtioTraceObserved']
            result.update(frames=frames,uart_raw=result.pop('uart'),trace_raw=result.pop('trace'),
                trace_observations=observations,wallpaper_reference=cls.bases[-1]['wallpaperReference'],
                resize_reference=cls.bases[-1]['resizeReference'],require_virtual_pressure=False)
        return result

    def test_rejected_runs_replay_without_desktop_acceptance(self):
        self.assertEqual(self.value['newMultiwindowRenderedRuns'],[])
        for number in self.value['rejectedMultiwindowRuns']:
            with self.subTest(run=number):
                proof = validate_rejected_multiwindow(number,**self.inputs(number))
                self.assertEqual(proof,self.row(number)['proof'])
                self.assertFalse(proof['multiwindowDesktopAccepted'])
                self.assertEqual(proof['completedFrames'],0)
                self.assertEqual(proof['nativeCleanupVerified'],number != 141)

    def test_native84_legacy_scene_keeps_full_pixel_input_and_cleanup_checks(self):
        proof = validate_legacy_desktop(**self.inputs(142,True))
        self.assertEqual(proof,self.row(142)['proof'])
        self.assertTrue(proof['legacyV6CompatibilityVerified'])
        self.assertFalse(proof['newFourActorDesktopAccepted'])
        self.assertFalse(proof['residencyReferenceReuseExercised'])
        self.assertFalse(proof['sharedBudget']['extendedVirtualMappingBoundaryAccepted'])
        self.assertEqual(proof['frameCount'],1800)
        self.assertEqual([r['maximumChannelError'] for r in proof['frames']],[1,1])

    def test_historical_verifier_keeps_rejecting_the_new_host(self):
        with self.assertRaises((ValueError,RuntimeError,AssertionError)):
            validate_managed_mapping_desktop(**self.inputs(142,True))

    def test_old_fixture_cannot_receive_new_object_limits(self):
        data = copy.deepcopy(self.inputs(142,True))
        next(r for r in data['records'] if r.get('nativeCoordinatedObjectCapacity'))['kernelLeaseLimit'] = 1536
        with self.assertRaises((ValueError,RuntimeError,AssertionError)): validate_legacy_desktop(**data)

    def test_failed_native_cleanup_cannot_be_relabelled_clean(self):
        data = copy.deepcopy(self.inputs(141)); data['report']['disconnectCleanup']['driverCleanupVerified'] = True
        with self.assertRaises((ValueError,RuntimeError,AssertionError)): validate_rejected_multiwindow(141,**data)

    def test_atomic_runtime_evidence_is_distinct_from_boot_faults(self):
        guest = self.inputs(139)['guest']; rows = admission_snapshots(guest)
        self.assertEqual(len(rows),3)
        self.assertEqual(sum(r['kind']==1 and r['activeCpuBytes']>0 for r in rows),1)
        for old,new in (('leaseExhausted=1','leaseExhausted=0'),('atomicSnapshot=true','atomicSnapshot=false')):
            with self.assertRaises((ValueError,RuntimeError,AssertionError)):
                admission_snapshots(guest.replace(old,new,1))

    def test_retained_sources_match_exact_compiled_inputs(self):
        for digest in self.value['blobs']: self.blob(digest)
        for item in self.value['nativeSourceArchives'].values():
            self.assertEqual(item['sources'],item['receipt']['sources'])
            for name,digest in item['sources'].items():
                self.assertFalse(name.endswith(('.exe','.ko','.so','.ext4','.cpio.gz')))
                self.blob(digest)
        for item in self.value['guestSourceArchives'].values():
            for digest in item['sources'].values(): self.blob(digest)
        for name,digest in self.value['auditSources'].items():
            self.assertEqual(hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest(),digest)


if __name__ == '__main__': unittest.main()
