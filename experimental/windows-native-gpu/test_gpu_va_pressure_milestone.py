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
from verify_gpu_va_pressure import validate_pressure_transfers,validate_rejected_pressure
from verify_managed_residency_state import validate_managed_state
from verify_managed_mapping_desktop import validate_managed_mapping_desktop
from verify_pressure_mapping_desktop import validate_pressure_mapping_desktop
from verify_pressure_frame_timing import active_timing

class GpuVaPressureEvidence(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-GPU-VA-PRESSURE-2026-10-22.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='553cbccc5a095026ff02e13cb7a837bacea2018cfac204c2126d409b6139cf18'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    @staticmethod
    @lru_cache(maxsize=8)
    def blob(digest):
        cls=GpuVaPressureEvidence;entry=cls.value['blobs'].get(digest)
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

    def test_both_real_pressure_desktops_have_exact_pixels_and_native_cleanup(self):
        self.assertEqual(self.value['renderedRuns'],[135,136])
        for number in self.value['renderedRuns']:
            with self.subTest(run=number):
                proof=validate_pressure_mapping_desktop(**self.inputs(number,True))
                self.assertEqual(proof,self.row(number)['proof'])
                self.assertEqual(proof['frameCount'],1800)
                self.assertEqual(proof['ordinaryGpuOnlyFrames'],1798)
                self.assertTrue(proof['sharedBudget']['extendedVirtualMappingBoundaryAccepted'])
                self.assertGreater(proof['pressureDiagnostic']['peakVirtualBytesBeforeFrameOne'],2*1024**3)
                self.assertEqual([r['maximumChannelError'] for r in proof['frames']],[1,1])
                self.assertFalse(proof['fullDesktopAccepted']);self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_corrupted_new_and_evicted_contents_cannot_pass(self):
        data=self.inputs(135)
        for field in ('fnv1a','priorFnv1a'):
            guest=data['guest'];line=next(x for x in guest.splitlines() if x.startswith('D3D12_VA_PRESSURE_CYCLE pass=1 index=27 '))
            token=next(x for x in line.split() if x.startswith(field+'='))
            corrupted=line.replace(token,field+'=0000000000000000')
            with self.subTest(field=field),self.assertRaises(ValueError):
                validate_pressure_transfers(guest.replace(line,corrupted),data['records'],data['source'])

    def test_missing_cycle_cpu_completion_or_address_change_is_rejected(self):
        data=self.inputs(136);guest=data['guest']
        line=next(x for x in guest.splitlines() if x.startswith('D3D12_VA_PRESSURE_CYCLE pass=0 index=0 '))
        address=next(x for x in line.split() if x.startswith('gpuVa='))
        for changed in ('',line.replace('cpuFenceWrite=false','cpuFenceWrite=true'),
                line.replace(address,'gpuVa=65536'),line.replace('target=1 ','target=2 ')):
            with self.subTest(change=changed[-80:]),self.assertRaises(ValueError):
                validate_pressure_transfers(guest.replace(line,changed),data['records'],data['source'])

    def test_unexercised_boundary_or_wrong_virtual_adapter_is_rejected(self):
        data=self.inputs(135);records=copy.deepcopy(data['records'])
        first=next(i for i,r in enumerate(records) if r.get('nativeCompositorRingInput'))
        for row in records[:first]:
            if row.get('nativeGpuMappingAccounted'):row['totalPages']=min(row['totalPages'],262144)
        with self.assertRaises(ValueError):validate_pressure_transfers(data['guest'],records,data['source'])
        with self.assertRaises(ValueError):validate_pressure_transfers(
            data['guest'].replace('adapterLuid=0000000057475055','adapterLuid=0000000000000001'),data['records'],data['source'])

    def test_native_retirement_and_pre_stream_release_are_required(self):
        data=self.inputs(135)
        for old,new in (('gpuRetired=true','gpuRetired=false'),('nativeObjectsReleased=true','nativeObjectsReleased=false'),
                ('beforeFirstQueue=true','beforeFirstQueue=false')):
            with self.subTest(field=old),self.assertRaises(ValueError):validate_pressure_transfers(
                data['guest'].replace(old,new),data['records'],data['source'])
        records=copy.deepcopy(data['records'])
        next(r for r in records if 'cpuValueWrittenByBridge' in r)['cpuValueWrittenByBridge']=True
        with self.assertRaises(ValueError):validate_pressure_transfers(data['guest'],records,data['source'])

    def test_changed_workload_provenance_or_memory_guard_is_rejected(self):
        data=self.inputs(136)
        for kind in ('generator','pressure_source','memory_guard','source_guard'):
            source=copy.deepcopy(data['source'])
            if kind=='generator':source['manifest']['activeFrameProbe']['generatorSha256']='0'*64
            elif kind=='pressure_source':source['manifest']['pressureSources']['d3d12_va_pressure_probe.cpp']='0'*64
            elif kind=='memory_guard':source['manifest']['preStreamD3D12Pressure']['residentMemoryGuardRaised']=True
            else:source['preStreamPressureSourcesUnchanged']=False
            with self.subTest(kind=kind),self.assertRaises(ValueError):
                if kind in ('generator','pressure_source'):active_timing(data['guest'],source)
                else:validate_pressure_transfers(data['guest'],data['records'],source)

    def test_failed_initial_diagnostic_and_preflight_stay_rejected(self):
        self.assertEqual(self.value['rejectedRuns'],[134]);data=self.inputs(134);row=self.row(134)
        proof=validate_rejected_pressure(**data,uart_raw=self.blob(row['files']['driver.log']),trace_raw=self.blob(row['files']['graphics-virtio.trace']))
        self.assertEqual(proof,row['proof']);self.assertFalse(proof['accepted'])
        preflight=self.value['preflightRejection'];self.assertEqual(preflight['run'],133)
        self.assertFalse(preflight['vmStarted']);self.assertFalse(preflight['sourceRecordWritten'])
        self.assertIn('AuthorizationManager check failed',self.blob(preflight['stderr']).decode('utf-8'))

    def test_sources_and_original_artifacts_exclude_private_payloads(self):
        for archive in self.value['compiledSourceArchives'].values():
            self.assertEqual(archive['files'],archive['receipt']['sources'])
            for digest in (*archive['files'].values(),archive['buildScript']):self.blob(digest)
        for row in self.value['runs']:
            source=json.loads(self.blob(row['files']['source.json']))
            self.assertEqual(row['runnerSource'],source['sourceHarnessSha256'])
            self.assertEqual(row['pressureSources'],self.value['pressureSourceArchives'][str(row['guestVersion'])])
            for digest in (*row['files'].values(),*row['activeSources'].values(),*row['guestOriginalSources'].values(),
                    *row['pressureSources'].values(),row['runnerSource'],row['observerSource']):self.blob(digest)
            for name,entry in source['manifest']['files'].items():
                if name.startswith(('wsl-libs/','wsl-drivers/')):
                    self.assertTrue(all(entry['sha256'] not in b['blobs'] for b in (self.value,*self.bases)))
            self.assertTrue(all(source['manifest']['initramfsSha256'] not in b['blobs'] for b in (self.value,*self.bases)))
        for group in ('sources','controllers','controlLogs'):
            for digest in self.value[group].values():self.blob(digest)
        self.assertFalse(self.value['defaultResidencyPolicyChanged']);self.assertFalse(self.value['shaderCoverageExtended'])
        self.assertFalse(self.value['fullDesktopAccepted']);self.assertFalse(self.value['nearNativePerformanceVerified'])

    def test_historical_workload_pins_do_not_accept_the_new_diagnostic(self):
        with self.assertRaises(ValueError):validate_managed_mapping_desktop(**self.inputs(135,True))
