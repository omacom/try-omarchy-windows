"""Replay bounded real desktops and reject claims beyond their measured scope."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_real_sdl_desktop import bgra_to_rgba,linear_blit
from verify_deferred_residency import validate_ownership,validate_rejected_attempt
from verify_managed_residency_state import validate_managed_state
from verify_managed_mapping_desktop import validate_managed_mapping_desktop

class ManagedResidencyEvidence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-MANAGED-RESIDENCY-2026-10-22.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='cd0e4d31d46a2df23624acb15f2e5cfe7ab2344da19b7782e74679075d34b0c4'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    @staticmethod
    @lru_cache(maxsize=8)
    def blob(digest):
        cls=ManagedResidencyEvidence;entry=cls.value['blobs'].get(digest)
        if entry is None:entry=next(b['blobs'][digest] for b in cls.bases if digest in b['blobs'])
        raw=zlib.decompress(base64.b64decode(entry['zlibBase64'],validate=True))
        if 'xorBase' in entry:
            other=cls.blob(entry['xorBase']);assert len(raw)==len(other)
            raw=(int.from_bytes(raw,'little')^int.from_bytes(other,'little')).to_bytes(len(raw),'little')
        assert len(raw)==entry['bytes'] and hashlib.sha256(raw).hexdigest()==digest==entry['sha256']
        return raw

    @classmethod
    def row(cls,number):return next(r for r in cls.value['runs'] if r['run']==number)

    @classmethod
    @lru_cache(maxsize=1)
    def inputs(cls,number,with_frames=False):
        row=cls.row(number);frames={}
        def frame(name):
            if name not in frames:
                item=row['frames'][name]
                if 'blob' in item:raw=cls.blob(item['blob'])
                elif item['transform']=='bgra-to-rgba':raw=bgra_to_rgba(frame(item['native']))
                else:
                    assert item['transform']=='xor-linear-reference'
                    reference=linear_blit(frame(item['source']),3840,2160,item['width'],item['height'])
                    delta=cls.blob(item['deltaBlob']);assert len(reference)==len(delta)
                    raw=(int.from_bytes(reference,'little')^int.from_bytes(delta,'little')).to_bytes(len(delta),'little')
                assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==item['sha256'];frames[name]=raw
            return frames[name]
        if with_frames:
            for name in row['frames']:frame(name)
        records=[]
        for line in cls.blob(row['files']['driver.errors.log']).decode('utf-8').splitlines():
            try:records.append(json.loads(line))
            except ValueError:pass
        result=dict(report=json.loads(cls.blob(row['files']['driver.json'])),source=json.loads(cls.blob(row['files']['source.json'])),
            records=records,guest=cls.blob(row['files']['driver.combined-guest.log']).decode('utf-8').replace('\r\n','\n').replace('\r','\n'))
        if with_frames:
            observations=[r for line in cls.blob(row['files']['driver.cadence.jsonl']).decode('utf-8').splitlines()
                if (r:=json.loads(line)).get('event')=='virtioTraceObserved']
            result.update(frames=frames,uart_raw=cls.blob(row['files']['driver.log']),trace_raw=cls.blob(row['files']['graphics-virtio.trace']),
                trace_observations=observations,wallpaper_reference=cls.bases[-1]['wallpaperReference'],resize_reference=cls.bases[-1]['resizeReference'])
        return result

    def test_both_complete_desktops_have_exact_pixels_input_lifecycles_and_cleanup(self):
        self.assertEqual(self.value['renderedRuns'],[131,132])
        for number in self.value['renderedRuns']:
            with self.subTest(run=number):
                proof=validate_managed_mapping_desktop(**self.inputs(number,True),require_virtual_pressure=False)
                self.assertEqual(proof,self.row(number)['proof'])
                self.assertEqual(proof['frameCount'],1800);self.assertEqual(proof['ordinaryGpuOnlyFrames'],1798)
                self.assertEqual([row['maximumChannelError'] for row in proof['frames']],[1,1])
                self.assertTrue(proof['sharedBudget']['exactLedgerReplayed'])
                self.assertFalse(proof['fullDesktopAccepted']);self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_unexercised_extended_address_boundary_cannot_pass_the_strict_gate(self):
        for number in self.value['renderedRuns']:
            data=self.inputs(number)
            with self.assertRaisesRegex(ValueError,'boundary was not exercised'):
                validate_managed_state(data['report'],data['records'],data['source'],data['guest'])
            proof=validate_managed_state(**data,require_virtual_pressure=False)
            self.assertFalse(proof['extendedVirtualMappingBoundaryAccepted'])
        self.assertFalse(self.value['extendedVirtualMappingPressureAccepted'])

    def test_mapped_busy_failed_or_unaccounted_eviction_is_rejected(self):
        data=self.inputs(131)
        for field,value in (('cpuMapped',True),('allGpuWorkRetired',False),('nativeCall',False),('ntstatus',-1),('retainedBefore',999)):
            records=copy.deepcopy(data['records'])
            next(r for r in records if r.get('nativeDeferredResidencyReclaimed'))[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):validate_ownership(data['report'],records)
        records=copy.deepcopy(data['records'])
        next(r for r in records if r.get('nativeGuestEvictionDeferred'))['nativeCall']=True
        with self.assertRaises(ValueError):validate_ownership(data['report'],records)

    def test_reference_leaks_or_fabricated_summary_counts_are_rejected(self):
        data=self.inputs(132)
        for field in ('liveRetainedResidencyReferences','reusedResidencyReferences','reclaimedResidencyReferences','peakRetainedResidencyReferences'):
            report=copy.deepcopy(data['report']);report['disconnectCleanup'][field]+=1
            with self.subTest(field=field),self.assertRaises(ValueError):validate_ownership(report,data['records'])

    def test_weakened_physical_guard_and_wrong_negotiation_are_rejected(self):
        original=self.inputs(132)
        for kind in ('guard','policy','contract','standalone'):
            data=copy.deepcopy(original)
            if kind=='guard':data['report']['disconnectCleanup']['reportedGpuUsageLimitBytes']*=2
            elif kind=='policy':data['source']['manifest']['managedGpuVaPolicy']['residentUsageGuardBytes']*=2
            elif kind=='contract':next(r for r in data['records'] if r.get('nativeCoordinatedObjectCapacity'))['version']=5
            else:next(r for r in data['records'] if r.get('nativeGpuMappingAccounted'))['standalonePages']=262145
            with self.subTest(kind=kind),self.assertRaises(ValueError):validate_managed_state(**data,require_virtual_pressure=False)

    def test_both_earlier_hardware_failures_remain_rejected(self):
        self.assertEqual(self.value['rejectedRuns'],[128,129])
        for number in self.value['rejectedRuns']:
            data=self.inputs(number);row=self.row(number)
            proof=validate_rejected_attempt(**data,uart=self.blob(row['files']['driver.log']),trace=self.blob(row['files']['graphics-virtio.trace']))
            self.assertEqual(proof,row['proof']);self.assertFalse(proof['accepted'])
            self.assertFalse(proof['rootCauseVerified'])
        self.assertTrue(self.row(129)['proof']['mappedAddressQuotaObserved'])

    def test_invalidated_post_run_source_guard_is_preserved_without_certification(self):
        self.assertEqual(self.value['controllerRejectedRuns'],[130]);row=self.row(130)
        self.assertFalse(row['rendered']);self.assertNotIn('proof',row)
        failure=self.blob(row['controllerRejection']['console']).decode('utf-8')
        self.assertIn('guest_original_paths.items()',failure);self.assertIn('AssertionError',failure)
        self.assertFalse(row['controllerRejection']['renderingAcceptanceCertified'])

    def test_exact_source_build_and_controller_archives_exclude_proprietary_binaries(self):
        for archive in self.value['compiledSourceArchives'].values():
            self.assertEqual(archive['files'],archive['receipt']['sources'])
            for digest in (*archive['files'].values(),archive['buildScript']):self.blob(digest)
        for row in self.value['runs']:
            source=json.loads(self.blob(row['files']['source.json']))
            self.assertEqual(source['nativeBuildReceipt'],self.value['compiledSourceArchives'][str(row['nativeVersion'])]['receipt'])
            self.assertEqual(row['runnerSource'],source['sourceHarnessSha256'])
            for digest in (*row['files'].values(),*row['guestOriginalSources'].values(),*row['activeSources'].values(),row['runnerSource'],row['observerSource']):self.blob(digest)
            for name,entry in source['manifest']['files'].items():
                if name.startswith(('wsl-libs/','wsl-drivers/')):
                    self.assertTrue(all(entry['sha256'] not in b['blobs'] for b in (self.value,*self.bases)))
        for group in ('sources','controllers','controlLogs'):
            for digest in self.value[group].values():self.blob(digest)
        self.assertFalse(self.value['defaultResidencyPolicyChanged']);self.assertTrue(self.value['managedBackendOptIn'])
