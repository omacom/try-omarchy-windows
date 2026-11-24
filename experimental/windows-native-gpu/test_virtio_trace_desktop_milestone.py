"""Replay the complete live carrier cohort and retain the failed input attempt."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_real_sdl_desktop import bgra_to_rgba,linear_blit
from verify_virtio_trace_desktop import validate_trace_desktop,OFFICIAL_INPUT


class VirtioTraceDesktop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gpu=Path(__file__).resolve().parent;cls.repo=cls.gpu.parents[1]
        raw=(cls.repo/'docs/evidence/OMARCHY-VIRTIO-TRACE-DESKTOP-2026-10-21.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='34db6019cab6c704f374f4b6726611838dc2f3f4790a7176f51632fffeef6dbf'
        cls.value=json.loads(raw)
        def referenced(row):
            raw=(cls.repo/row['path']).read_bytes();assert hashlib.sha256(raw).hexdigest()==row['sha256']
            return json.loads(raw)
        cls.base=referenced(cls.value['baseEvidence']);cls.older=referenced(cls.base['baseEvidence'])

    @staticmethod
    @lru_cache(maxsize=10)
    def blob(digest):
        cls=VirtioTraceDesktop
        item=cls.value['blobs'].get(digest) or cls.base['blobs'].get(digest) or cls.older['blobs'][digest]
        raw=zlib.decompress(base64.b64decode(item['zlibBase64'],validate=True))
        if 'xorBase' in item:
            other=cls.blob(item['xorBase']);assert len(raw)==len(other)
            raw=(int.from_bytes(raw,'little')^int.from_bytes(other,'little')).to_bytes(len(raw),'little')
        assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==digest==item['sha256']
        return raw

    @classmethod
    def row(cls,number):return next(row for row in cls.value['runs'] if row['run']==number)

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
        uart=cls.blob(row['files']['driver.log']);virtio=row['carrier']=='virtio'
        return dict(report=json.loads(cls.blob(row['files']['driver.json'])),records=records,frames=frames,
            source=json.loads(cls.blob(row['files']['source.json'])),uart_raw=uart,
            trace_raw=cls.blob(row['files']['graphics-virtio.trace']) if virtio else None,
            guest=cls.blob(row['files']['driver.combined-guest.log']).decode('utf-8') if virtio else uart.decode('utf-8').replace('\r\n','\n').replace('\r','\n'),
            trace_observations=row['liveTraceObservations'],wallpaper_reference=cls.older['wallpaperReference'],
            resize_reference=cls.older['resizeReference'])

    def test_four_accepted_runs_have_exact_sources_full_pixels_live_fences_and_cleanup(self):
        self.assertEqual(self.value['acceptedRuns'],[116,118,120,121])
        for number in self.value['acceptedRuns']:
            with self.subTest(run=number):
                proof=validate_trace_desktop(**self.inputs(number))
                self.assertEqual(proof,self.row(number)['proof'])
                self.assertEqual([f['maximumChannelError'] for f in proof['frames']],[1,1])
                self.assertEqual(proof['frameCount'],300)
                self.assertFalse(proof['nearNativePerformanceVerified'])
                self.assertFalse(proof['allFramesDisplayedVerified'])
                if number in (118,120):self.assertTrue(proof['traceIntegrity']['complete'])

    def test_failed_input_run_and_preflight_rejection_remain_distinct_from_render_pass(self):
        self.assertEqual(self.value['rejectedRuns'],[119]);data=self.inputs(119)
        self.assertEqual(data['report']['hostBridgeExit'],0)
        self.assertNotIn('OMARCHY_NATIVE_INPUT_PTY_HEX',data['guest'])
        self.assertIn('HYPRLAND_HOST_QUEUE_END frames=2 target=300 failed=1',data['guest'])
        self.assertFalse(self.row(119)['timingComparisonEligible'])
        with self.assertRaisesRegex(ValueError,'Missing/invalid active timing'):validate_trace_desktop(**data)
        preflight=self.value['preflightFailures'][0];record=json.loads(self.blob(preflight['record']))
        self.assertFalse(record['vmStarted']);self.assertFalse(record['gpuWorkStarted'])
        self.assertEqual(preflight['run'],117)

    def test_compiled_sources_private_controllers_and_payload_differences_are_reconstructable(self):
        archive=self.value['compiledSourceArchives']['77']
        self.assertEqual(archive['files'],archive['receipt']['sources'])
        self.assertEqual(archive['buildScript'],archive['receipt']['buildScriptSha256'])
        for digest in (*archive['files'].values(),archive['buildScript'],*self.value['controllerSources'].values(),*self.value['traceSources'].values()):self.blob(digest)
        for row in self.value['runs']:
            source=json.loads(self.blob(row['files']['source.json']))
            self.assertEqual(source['nativeBuildReceipt'],archive['receipt'])
            self.assertEqual(row['guestOriginalSources'],source['manifest']['sources'])
            self.assertEqual(row['runnerSource'],source['sourceHarnessSha256'])
            self.assertEqual(row['observerSource'],source['traceObserverSha256'])
            for digest in (*row['files'].values(),*row['guestOriginalSources'].values(),*row['activeSources'].values(),row['runnerSource'],row['observerSource']):self.blob(digest)
        self.assertEqual(self.value['payloadDifferences'],['bin/normal-root-graphics-service','data/virtio-trace-source-receipt.json','virtio-graphics-trace-probe'])
        for name,key in (('verify_virtio_trace_desktop.py','auditorSha256'),('virtio_trace.py','traceDecoder'),('virtio-trace-probe/generate.py','traceGenerator'),('test_owned_runtime_qemu.py','originalHarnessSource')):
            self.assertEqual(hashlib.sha256((self.gpu/name).read_bytes()).hexdigest(),self.value[key])

    def test_truncated_mixed_or_not_live_trace_cannot_pass(self):
        original=self.inputs(118)
        for bad in (original['trace_raw'][:-1],original['trace_raw'].replace(b'complete=true',b'complete=false')):
            with self.assertRaises(ValueError):validate_trace_desktop(**dict(original,trace_raw=bad))
        with self.assertRaisesRegex(ValueError,'Guest replay differs'):
            validate_trace_desktop(**dict(original,guest=original['guest']+'fabricated record\n'))
        for observations in ([],[dict(row,bytes=1) for row in original['trace_observations']]):
            with self.assertRaisesRegex(ValueError,'observed growing'):
                validate_trace_desktop(**dict(original,trace_observations=observations))

    def test_mixed_binary_input_and_leaked_mapping_records_are_rejected(self):
        original=self.inputs(118)
        data=dict(original,source=copy.deepcopy(original['source']));data['source']['manifest']['virtioTraceProbe']['inputImplementationChanged']=True
        with self.assertRaises(ValueError):validate_trace_desktop(**data)
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['driverBridgeSha256']='0'*64
        with self.assertRaises(ValueError):validate_trace_desktop(**data)
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['ownedQemuControl']['liveAllocationMappings']=1
        with self.assertRaises(ValueError):validate_trace_desktop(**data)

    def test_official_input_source_is_retained_and_no_product_performance_claim_is_inferred(self):
        for name,digest in OFFICIAL_INPUT.items():
            self.assertEqual(self.value['qemuCompiledSources'][name],digest);self.blob(digest)
        for key in ('physicalScanoutCadenceVerified','physicalInputLatencyVerified','gpuDeviceLossRecoveryVerified','nearNativePerformanceVerified','fullDesktopAccepted'):
            self.assertFalse(self.value[key])
        self.assertIn('not GPU timestamps',self.value['timingScope'])


if __name__=='__main__':unittest.main()
