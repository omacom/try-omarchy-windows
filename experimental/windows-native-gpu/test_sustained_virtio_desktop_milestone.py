"""Replay long GPU streams, driver compatibility and rejected residency controls."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import unittest
import zlib
from verify_real_sdl_desktop import bgra_to_rgba,linear_blit
from verify_sustained_virtio_desktop import (active_timing,validate_sustained_trace_desktop,
    validate_sustained_memory_failure,validate_rejected_sustained_candidate)


class SustainedNvidiaDesktop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gpu=Path(__file__).resolve().parent;cls.repo=cls.gpu.parents[1]
        raw=(cls.repo/'docs/evidence/OMARCHY-SUSTAINED-NVIDIA-2026-10-22.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='9d50ef255fb6bdadf53211bb0411b1ee570555e59822bb2b53e33f1422b0f9ea'
        cls.value=json.loads(raw);cls.bases=[];item=cls.value
        while 'baseEvidence' in item:
            link=item['baseEvidence'];raw=(cls.repo/link['path']).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==link['sha256']
            item=json.loads(raw);cls.bases.append(item)

    @staticmethod
    @lru_cache(maxsize=8)
    def blob(digest):
        cls=SustainedNvidiaDesktop;entry=cls.value['blobs'].get(digest)
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
    def inputs(cls,number):
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
                assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==item['sha256']
                frames[name]=raw
            return frames[name]
        for name in row['frames']:frame(name)
        records=[]
        for line in cls.blob(row['files']['driver.errors.log']).decode('utf-8').splitlines():
            try:records.append(json.loads(line))
            except ValueError:pass
        uart=cls.blob(row['files']['driver.log'])
        guest=cls.blob(row['files']['driver.combined-guest.log']).decode('utf-8') if 'driver.combined-guest.log' in row['files'] else uart.decode('utf-8',errors='replace')
        return dict(report=json.loads(cls.blob(row['files']['driver.json'])),source=json.loads(cls.blob(row['files']['source.json'])),
            records=records,frames=frames,guest=guest.replace('\r\n','\n').replace('\r','\n'),uart_raw=uart,
            trace_raw=cls.blob(row['files']['graphics-virtio.trace']),trace_observations=row['liveTraceObservations'],
            wallpaper_reference=cls.bases[-1]['wallpaperReference'],resize_reference=cls.bases[-1]['resizeReference'])

    def test_two_1800_frame_runs_have_complete_gpu_pixels_live_fences_input_and_cleanup(self):
        self.assertEqual(self.value['acceptedRuns'],[125,127])
        for number in self.value['acceptedRuns']:
            with self.subTest(run=number):
                proof=validate_sustained_trace_desktop(**self.inputs(number))
                self.assertEqual(proof,self.row(number)['proof'])
                self.assertEqual(proof['frameCount'],1800);self.assertEqual(proof['ordinaryGpuOnlyFrames'],1798)
                self.assertGreaterEqual(proof['activeTiming']['streamDurationSeconds'],25)
                self.assertEqual([f['maximumChannelError'] for f in proof['frames']],[1,1])
                self.assertFalse(proof['fullDesktopAccepted']);self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_all_three_failed_hardware_attempts_and_unstarted_preflight_stay_rejected(self):
        self.assertEqual(self.value['rejectedRuns'],[122,123,126])
        for number in self.value['rejectedRuns']:
            data=self.inputs(number)
            if number==122:proof=validate_sustained_memory_failure(data['report'],data['records'],data['source'],data['trace_raw'])
            else:proof=validate_rejected_sustained_candidate(data['report'],data['guest'],data['records'],data['source'],data['uart_raw'],data['trace_raw'],
                'old-vendor-runtime' if number==123 else 'mapped-eviction')
            self.assertEqual(proof,self.row(number)['proof']);self.assertFalse(proof['accepted'])
            with self.assertRaises(ValueError):validate_sustained_trace_desktop(**data)
        preflight=self.value['preflightFailures'][0];record=json.loads(self.blob(preflight['record']))
        self.assertEqual(record['run'],124);self.assertFalse(record['vmStarted']);self.assertFalse(record['gpuWorkStarted'])

    def test_exact_source_archives_and_rejected_candidate_are_retained_without_vendor_binaries(self):
        for archive in self.value['compiledSourceArchives'].values():
            self.assertEqual(archive['files'],archive['receipt']['sources'])
            for digest in (*archive['files'].values(),archive['buildScript']):self.blob(digest)
        for row in self.value['runs']:
            source=json.loads(self.blob(row['files']['source.json']))
            self.assertEqual(source['nativeBuildReceipt'],self.value['compiledSourceArchives'][str(row['nativeVersion'])]['receipt'])
            self.assertEqual(row['runnerSource'],source['sourceHarnessSha256']);self.assertEqual(row['observerSource'],source['traceObserverSha256'])
            for digest in (*row['files'].values(),*row['guestOriginalSources'].values(),*row['activeSources'].values(),row['runnerSource'],row['observerSource']):self.blob(digest)
        for digest in (*self.value['sources'].values(),*self.value['controllers'].values(),*self.value['controlLogs'].values()):self.blob(digest)
        self.assertFalse(self.value['budgetCandidateActiveInWorkingHost'])
        self.assertEqual(self.value['workingDriverSource'],self.value['compiledSourceArchives']['77']['files']['experimental/windows-native-gpu/driver_bridge.cpp'])
        self.assertNotEqual(self.value['workingDriverSource'],self.value['compiledSourceArchives']['78']['files']['experimental/windows-native-gpu/driver_bridge.cpp'])
        vendor=json.loads(self.blob(self.value['privateDriverSnapshotReceipt']))
        for file in vendor['files'].values():
            self.assertTrue(all(file['sha256'] not in b['blobs'] for b in (self.value,*self.bases)))
        self.assertEqual(vendor['collectorSha256'],self.value['sources']['snapshot-nvidia-wsl-runtime.ps1'])
        self.assertEqual(hashlib.sha256((self.gpu/'verify_sustained_virtio_desktop.py').read_bytes()).hexdigest(),self.value['sources']['verify_sustained_virtio_desktop.py'])

    def test_driver_changes_unsigned_snapshots_and_incomplete_trace_cannot_pass(self):
        original=self.inputs(125)
        for field,value in (('driverVersion','32.0.16.1714'),('signedInstalledDriver',False),('redistributionApproved',True)):
            data=dict(original,source=copy.deepcopy(original['source']));data['source']['manifest']['nativeDriverRuntimeSnapshot'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):validate_sustained_trace_desktop(**data)
        data=dict(original,source=copy.deepcopy(original['source']));data['source']['installedNvidiaRuntimeAfter']['driverVersion']='changed'
        with self.assertRaises(ValueError):validate_sustained_trace_desktop(**data)
        with self.assertRaises(ValueError):validate_sustained_trace_desktop(**dict(original,trace_raw=original['trace_raw'][:-1]))

    def test_burst_timings_and_invented_display_performance_do_not_certify_sustained_work(self):
        data=self.inputs(125);guest=data['guest']
        first=int(re.search(r'HYPRLAND_ACTIVE_QUEUE_TIMING .*?monotonicNs=(\d+)',guest)[1])
        burst=re.sub(r'(HYPRLAND_ACTIVE_QUEUE_TIMING [^\n]*?monotonicNs=)(\d+)',
            lambda m:m[1]+str(first+(int(m[2])-first)//4),guest)
        with self.assertRaisesRegex(ValueError,'25-second minimum'):active_timing(burst,data['source'])
        for field in ('physicalScanoutCadenceVerified','physicalInputLatencyVerified','nearNativePerformanceVerified','fullDesktopAccepted'):
            self.assertFalse(self.value[field])
        self.assertFalse(self.value['genericRuntimeVerifierIsDesktopAcceptanceGate'])

    def test_failed_gpu_work_leaked_mappings_and_software_uploads_cannot_pass(self):
        original=self.inputs(125)
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['disconnectCleanup']['failedCompositorFrames']=1
        with self.assertRaises(ValueError):validate_sustained_trace_desktop(**data)
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['ownedQemuControl']['liveAllocationMappings']=1
        with self.assertRaises(ValueError):validate_sustained_trace_desktop(**data)
        data=dict(original,records=copy.deepcopy(original['records']))
        next(r for r in data['records'] if r.get('nativeSdlCompositorQueued'))['cpuUpload']=True
        with self.assertRaises(ValueError):validate_sustained_trace_desktop(**data)


if __name__=='__main__':unittest.main()
