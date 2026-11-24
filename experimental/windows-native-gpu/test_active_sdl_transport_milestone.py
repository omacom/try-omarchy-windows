"""Replay active desktop evidence and reject pixel, timing and provenance failures."""
import base64
import copy
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import unittest
import zlib

from verify_real_sdl_desktop import bgra_to_rgba,linear_blit
from verify_active_sdl_transport import active_timing,validate_active_sdl_desktop


class ActiveSdlTransport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gpu=Path(__file__).resolve().parent;cls.repo=cls.gpu.parents[1]
        raw=(cls.repo/'docs/evidence/OMARCHY-ACTIVE-SDL-TRANSPORT-2026-10-21.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='33773dba4ee8d31b6fe391049e68d1290605660e08e958a14c7d35c6dfd77075'
        cls.evidence=json.loads(raw)
        base=cls.evidence['baseEvidence'];raw=(cls.repo/base['path']).read_bytes()
        assert hashlib.sha256(raw).hexdigest()==base['sha256']
        cls.base=json.loads(raw)

    @staticmethod
    @lru_cache(maxsize=10)
    def blob(digest):
        cls=ActiveSdlTransport
        item=cls.evidence['blobs'].get(digest) or cls.base['blobs'][digest]
        raw=zlib.decompress(base64.b64decode(item['zlibBase64'],validate=True))
        if 'xorBase' in item:
            other=cls.blob(item['xorBase']);assert len(other)==len(raw)
            raw=(int.from_bytes(raw,'little')^int.from_bytes(other,'little')).to_bytes(len(raw),'little')
        assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==digest==item['sha256']
        return raw

    @classmethod
    def row(cls,number):return next(row for row in cls.evidence['runs'] if row['run']==number)

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
        return dict(report=json.loads(cls.blob(row['files']['driver.json'])),
                    source=json.loads(cls.blob(row['files']['source.json'])),records=records,frames=frames,
                    guest=cls.blob(row['files']['driver.log']).decode('utf-8').replace('\r\n','\n'),
                    wallpaper_reference=cls.base['wallpaperReference'],resize_reference=cls.base['resizeReference'])

    def test_four_accepted_runs_have_complete_pixels_fences_and_live_cleanup(self):
        self.assertEqual(self.evidence['acceptedRuns'],[106,109,110,111])
        for number in self.evidence['acceptedRuns']:
            with self.subTest(run=number):
                proof=validate_active_sdl_desktop(**self.inputs(number))
                self.assertEqual(proof,self.row(number)['audit'])
                self.assertEqual([r['maximumChannelError'] for r in proof['frames']],[1,1])
                self.assertEqual(proof['activeTiming']['rows'],300)
                self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_both_rejected_attempts_remain_rejected(self):
        self.assertEqual(self.evidence['rejectedRuns'],[107,108])
        for number in self.evidence['rejectedRuns']:
            data=self.inputs(number)
            with self.subTest(run=number),self.assertRaises(ValueError):validate_active_sdl_desktop(**data)
        data=self.inputs(108)
        self.assertEqual(data['report']['hostBridgeExit'],0)
        for frame in (1,300):
            self.assertFalse(any(data['frames'][f'relay-window-{frame:016d}.rgba']))
        self.assertEqual(active_timing(data['guest'],data['source']),self.row(108)['timing'])
        self.assertFalse(self.evidence['performanceGainEstablished'])

    def test_exact_compiled_guest_and_runner_sources_are_reconstructable(self):
        for archive in self.evidence['compiledSourceArchives'].values():
            self.assertEqual(archive['files'],archive['receipt']['sources'])
            self.assertEqual(archive['buildScript'],archive['receipt']['buildScriptSha256'])
            for digest in (*archive['files'].values(),archive['buildScript']):self.blob(digest)
        for row in self.evidence['runs']:
            source=json.loads(self.blob(row['files']['source.json']))
            archive=self.evidence['compiledSourceArchives'][str(row['nativeVersion'])]
            self.assertEqual(source['nativeBuildReceipt'],archive['receipt'])
            self.assertEqual(row['guestOriginalSources'],source['manifest']['sources'])
            self.assertEqual(row['activeSources'],source['manifest']['activeFrameProbe']['generatedSources'])
            self.assertEqual(row['runnerSource'],source['sourceHarnessSha256'])
            for digest in (*row['guestOriginalSources'].values(),*row['activeSources'].values(),row['runnerSource'],row['observerSource']):self.blob(digest)
        self.assertEqual(hashlib.sha256((self.gpu/'verify_active_sdl_transport.py').read_bytes()).hexdigest(),self.evidence['auditorSha256'])
        self.assertEqual(hashlib.sha256((self.gpu/'active-frame-probe/generate.py').read_bytes()).hexdigest(),self.evidence['activeGenerator'])

    def test_timing_rejects_missing_reordered_and_overclaimed_records(self):
        data=self.inputs(109);guest=data['guest'];source=data['source']
        first=next(line for line in guest.splitlines(True) if line.startswith('HYPRLAND_ACTIVE_QUEUE_TIMING '))
        for changed in (guest.replace(first,'',1),guest.replace(first,first.replace('frame=1 ','frame=2 '),1),
                        guest.replace('cpuWallClock=true gpuTimestamp=false','cpuWallClock=true gpuTimestamp=true',1)):
            with self.assertRaises(ValueError):active_timing(changed,source)
        changed=copy.deepcopy(source);changed['manifest']['activeFrameProbe']['benchmarkAccepted']=True
        with self.assertRaises(ValueError):active_timing(guest,changed)

    def test_mixed_binaries_payloads_and_transport_configuration_are_rejected(self):
        original=self.inputs(109)
        for field in ('driverBridgeSha256','qemuSha256','initramfsSha256'):
            data=dict(original,report=copy.deepcopy(original['report']));data['report'][field]='0'*64
            with self.subTest(field=field),self.assertRaises(ValueError):validate_active_sdl_desktop(**data)
        data=dict(original,records=copy.deepcopy(original['records']))
        next(row for row in data['records'] if row.get('nativeGpuTransportConfigured'))['inputPathChanged']=True
        with self.assertRaises(ValueError):validate_active_sdl_desktop(**data)

    def test_unreleased_mappings_and_failed_gpu_work_cannot_pass(self):
        original=self.inputs(109)
        for field in ('liveAllocationMappings','fenceMappingsReleasedAfterVmExit'):
            data=dict(original,report=copy.deepcopy(original['report']));data['report']['ownedQemuControl'][field]=1
            with self.subTest(field=field),self.assertRaises(ValueError):validate_active_sdl_desktop(**data)
        data=dict(original,report=copy.deepcopy(original['report']));data['report']['disconnectCleanup']['failedCompositorFrames']=1
        with self.assertRaises(ValueError):validate_active_sdl_desktop(**data)

    def test_window_diagnostics_are_original_and_do_not_replace_pixel_proof(self):
        data=self.inputs(111);row=self.row(111)
        for frame in (1,300):
            state=next(record['qemuState'] for record in data['records'] if record.get('nativeSdlCompositorQueued') and record['frame']==frame)
            original=json.loads(self.blob(row['files'][f'relay-window-state-{frame:016d}.json']))
            self.assertEqual(original,state['windowAudit'])
            self.assertFalse(original['visibilityProvesPixelOwnership'])
            self.assertFalse(original['physicalScanoutVerified'])
        for name,digest in row['qemuCompiledSources'].items():self.blob(digest)
        self.assertEqual({name:row['qemuCompiledSources'][name] for name in data['source']['qemuWindowAuditProvenance']['inputFiles']},
                         data['source']['qemuWindowAuditProvenance']['inputFiles'])
        # Shown/focused flags do not rescue an all-zero capture.
        changed=dict(data,frames=dict(data['frames']))
        name='relay-window-0000000000000001.rgba';changed['frames'][name]=bytes(len(data['frames'][name]))
        with self.assertRaisesRegex(ValueError,'independent full GL_LINEAR'):validate_active_sdl_desktop(**changed)


if __name__=='__main__':unittest.main()
