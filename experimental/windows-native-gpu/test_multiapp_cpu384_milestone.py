"""Replay CPU384 failures and complete real two-application GPU captures."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_multiapp_desktop import (validate_multiapp_desktop, validate_cpu384_legacy_desktop,
                                    validate_multiapp_rejection, validate_windows)
from verify_managed_mapping_desktop import validate_managed_mapping_desktop
from verify_real_sdl_desktop import bgra_to_rgba, linear_blit


class MultiappCpu384(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = Path(__file__).resolve().parents[2]
        raw = (cls.repo/'docs/evidence/OMARCHY-MULTIAPP-CPU384-2026-10-23.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest() == '868420fc86e8ce03f9bec276e7f7b3f4d818cc9e054fc6b9af79d2283e471680'
        cls.value = json.loads(raw); cls.bases = []; item = cls.value
        while 'baseEvidence' in item:
            link = item['baseEvidence']; raw = (cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest() == link['sha256']
            item = json.loads(raw); cls.bases.append(item)

    @classmethod
    @lru_cache(maxsize=8)
    def blob(cls, digest):
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
            uart_raw=cls.blob(row['files']['driver.log']),trace_raw=cls.blob(row['files']['graphics-virtio.trace']))
        if with_frames:
            for name in row['frames']: frame(name)
            observations = [r for line in cls.blob(row['files']['driver.cadence.jsonl']).decode('utf-8').splitlines()
                if (r := json.loads(line)).get('event') == 'virtioTraceObserved']
            result.update(frames=frames,trace_observations=observations,
                wallpaper_reference=cls.bases[-1]['wallpaperReference'],resize_reference=cls.bases[-1]['resizeReference'],
                require_virtual_pressure=False)
        return result

    def test_both_complete_two_application_repeats(self):
        for number in (145,146):
            proof = validate_multiapp_desktop(**self.inputs(number,True))
            self.assertEqual(proof,self.row(number)['proof'])
            self.assertTrue(proof['boundedTwoApplicationExperimentVerified'])
            self.assertEqual(proof['lifecycles']['peakConcurrentActors'],4)
            self.assertEqual(proof['lifecycles']['liveRetiredActors'],7)
            self.assertEqual([r['maximumChannelError'] for r in proof['frames']],[1,1])
            self.assertFalse(proof['intermittentAliasLimitResolved'])
            self.assertFalse(proof['fullDesktopAccepted'])

    def test_legacy_v6_keeps_original_192_mib_and_object_limits(self):
        proof = validate_cpu384_legacy_desktop(**self.inputs(147,True))
        self.assertEqual(proof,self.row(147)['proof'])
        self.assertTrue(proof['legacyV6CompatibilityVerified'])
        self.assertFalse(proof['residencyReferenceReuseExercised'])
        self.assertFalse(proof['sharedBudget']['physicalMemoryGuardRaised'])

    def test_failures_remain_rejected_with_exact_trace(self):
        for number in (143,144):
            proof = validate_multiapp_rejection(number,**self.inputs(number))
            self.assertEqual(proof,self.row(number)['proof'])
            self.assertFalse(proof['completeRunAccepted'])
            self.assertEqual(proof['completedHandoffs'],0 if number==143 else 1113)

    def test_fatal_reaping_cannot_be_relabelled_clean_retirement(self):
        data = copy.deepcopy(self.inputs(144)); data['report']['disconnectCleanup']['driverCleanupVerified'] = True
        with self.assertRaises(ValueError): validate_multiapp_rejection(144,**data)

    def test_alias_failure_cannot_be_relabelled_an_object_or_gpu_budget_failure(self):
        data = copy.deepcopy(self.inputs(144))
        next(r for r in data['records'] if r.get('nativeSharedAliasAdmissionRejected'))['referenceLimit'] = 32
        with self.assertRaises(ValueError): validate_multiapp_rejection(144,**data)

    def test_mixed_cpu_or_gpu_budget_receipts_are_rejected(self):
        for key,value in (('cpuMappingBytes',201326592),('residentUsageGuardBytes',2147483648)):
            data = copy.deepcopy(self.inputs(145)); data['source']['manifest']['managedGpuVaPolicy'][key] = value
            with self.assertRaises(ValueError): validate_multiapp_desktop(**data)

    def test_new_limits_cannot_silently_apply_to_legacy_guest(self):
        data = copy.deepcopy(self.inputs(147))
        next(r for r in data['records'] if r.get('nativeCoordinatedObjectCapacity'))['cpuBytes'] = 402653184
        with self.assertRaises(ValueError): validate_cpu384_legacy_desktop(**data)

    def test_stalled_counter_or_missing_focus_checkpoint_rejected(self):
        data = self.inputs(145)
        for old,new in (('OMARCHY_MULTIWINDOW_CHECKPOINT phase=4','OMARCHY_MULTIWINDOW_CHECKPOINT phase=3'),
                        ('OMARCHY_MULTIWINDOW_4_COMPANION_PULSE_HEX','OMARCHY_MULTIWINDOW_3_COMPANION_PULSE_HEX')):
            with self.assertRaises(ValueError): validate_windows(data['guest'].replace(old,new,1),data['records'],{})

    def test_historical_auditor_still_rejects_new_profile(self):
        with self.assertRaises(ValueError): validate_managed_mapping_desktop(**self.inputs(145,True))

    def test_exact_sources_are_retained_without_private_binaries(self):
        for digest in self.value['blobs']: self.blob(digest)
        for item in self.value['nativeSourceArchives'].values():
            self.assertEqual(item['sources'],item['receipt']['sources'])
            for name,digest in item['sources'].items():
                self.assertFalse(name.endswith(('.exe','.ko','.so','.ext4','.cpio.gz'))); self.blob(digest)
        for item in self.value['guestSourceArchives'].values():
            for name,digest in item['sources'].items():
                self.assertFalse(name.endswith(('.exe','.ko','.so','.ext4','.cpio.gz'))); self.blob(digest)
        for name,digest in self.value['auditSources'].items():
            self.assertEqual(hashlib.sha256((Path(__file__).parent/name).read_bytes()).hexdigest(),digest)


if __name__ == '__main__': unittest.main()
