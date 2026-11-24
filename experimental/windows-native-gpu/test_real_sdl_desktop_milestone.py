"""Replay actual guest-to-SDL pixels and retain the failed stability repeat."""
import base64
import copy
import hashlib
import json
from pathlib import Path
import unittest
import zlib
from verify_real_sdl_desktop import bgra_to_rgba,linear_blit,pixel_audit,validate_real_sdl_desktop


class RealSdlDesktopMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        raw=(cls.repo/'docs/evidence/OMARCHY-QEMU-SDL-DESKTOP-2026-10-21.json').read_bytes()
        assert hashlib.sha256(raw).hexdigest()=='0369b574ce3f1c4c3bfd7f29333d7dcc0cf06735d6cf048f1a625db6a2abce28'
        cls.value=json.loads(raw);cls.blobs={}
        def blob(key):
            if key not in cls.blobs:
                item=cls.value['blobs'][key]
                raw=zlib.decompress(base64.b64decode(item['zlibBase64'],validate=True))
                if 'xorBase' in item:
                    base=blob(item['xorBase'])
                    assert len(raw)==len(base)
                    raw=(int.from_bytes(raw,'little')^int.from_bytes(base,'little')).to_bytes(len(raw),'little')
                assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==key==item['sha256']
                cls.blobs[key]=raw
            return cls.blobs[key]
        cls.blob=staticmethod(blob)
        row=next(item for item in cls.value['runs'] if item['run']==102)
        cls.accepted=row;cls.frames={}
        def frame(name):
            if name not in cls.frames:
                item=row['frames'][name]
                if 'blob' in item: raw=blob(item['blob'])
                elif item['transform']=='bgra-to-rgba': raw=bgra_to_rgba(frame(item['native']))
                else:
                    assert item['transform']=='xor-linear-reference'
                    reference=linear_blit(frame(item['source']),3840,2160,item['width'],item['height'])
                    delta=blob(item['deltaBlob'])
                    assert len(reference)==len(delta)
                    raw=(int.from_bytes(reference,'little')^int.from_bytes(delta,'little')).to_bytes(len(delta),'little')
                assert len(raw)==item['bytes'] and hashlib.sha256(raw).hexdigest()==item['sha256']
                cls.frames[name]=raw
            return cls.frames[name]
        for name in row['frames']: frame(name)
        cls.report=json.loads(blob(row['files']['driver.json']))
        cls.source=json.loads(blob(row['files']['source.json']))
        # Match Path.read_text's universal-newline view used by the hardware
        # auditor; the evidence retains every original CRLF byte separately.
        cls.guest=blob(row['files']['driver.log']).decode().replace('\r\n','\n')
        cls.records=[]
        for line in blob(row['files']['driver.errors.log']).decode().splitlines():
            try:cls.records.append(json.loads(line))
            except ValueError:pass

    def audit(self,**changes):
        args=dict(report=self.report,guest=self.guest,records=self.records,frames=self.frames,source=self.source,
            wallpaper_reference=self.value['wallpaperReference'],resize_reference=self.value['resizeReference'])
        args.update(changes)
        return validate_real_sdl_desktop(**args)

    def test_complete_guest_to_sdl_and_live_retirement(self):
        proof=self.audit()
        self.assertEqual(proof,self.accepted['audit'])
        self.assertEqual([x['maximumChannelError'] for x in proof['frames']],[1,1])
        self.assertEqual(proof['ordinaryGpuOnlyFrames'],298)
        self.assertTrue(proof['liveSdlDetachVerified'])
        self.assertFalse(proof['fullDesktopAccepted'])
        self.assertFalse(proof['nearNativePerformanceVerified'])

    def test_exact_compiled_sources_and_frozen_auditors(self):
        for version,archive in self.value['compiledSourceArchives'].items():
            receipt=archive['receipt']
            self.assertEqual(len(archive['files']),117)
            self.assertEqual(archive['files'],receipt['sources'])
            for name,digest in archive['files'].items():
                self.assertEqual(hashlib.sha256(self.blob(digest)).hexdigest(),digest)
        for name,digest in self.value['auditors'].items():
            self.assertEqual(hashlib.sha256((self.repo/'experimental/windows-native-gpu'/name).read_bytes()).hexdigest(),digest)
        archive=self.value['compiledSourceArchives']['71']
        self.assertEqual(archive['receipt'],self.source['nativeBuildReceipt'])
        for name,digest in self.source['manifest']['sources'].items():
            if name.endswith(('.h','.cpp')):
                self.assertEqual(archive['files']['experimental/windows-native-gpu/'+name],digest)

    def test_both_failed_attempts_remain_rejected(self):
        failed=[row for row in self.value['runs'] if not row['accepted']]
        self.assertEqual([row['run'] for row in failed],[101,103])
        self.assertEqual(self.value['acceptedRunCount'],1)
        for row in failed:
            source=json.loads(self.blob(row['files']['source.json']))
            report=json.loads(self.blob(row['files']['driver.json']))
            self.assertTrue(source['sourceImageUnchanged'])
            self.assertNotEqual(report['hostBridgeExit'],0)
            self.assertNotIn('relay-window-0000000000000300.rgba',row['frames'])
        lost=failed[1]
        text=self.blob(lost['files']['driver.errors.log']).decode()
        self.assertIn('SDL consumer device lost',text)
        rows=[json.loads(line) for line in self.blob(lost['files']['driver.cadence.jsonl']).decode().splitlines()]
        queued=[r for r in rows if r['event']=='nativeSdlCompositorQueued']
        self.assertEqual([r['frame'] for r in queued],list(range(1,208)))
        self.assertEqual(hashlib.sha256(self.blob(lost['profileObserver']['sourceBlob'])).hexdigest(),lost['profileObserver']['sha256'])

    def test_reject_missing_or_reordered_handoffs(self):
        for key in ('nativeCompositorRingInput','nativeSdlCompositorQueued','nativeCompositorRingFrame'):
            records=copy.deepcopy(self.records)
            next(row for row in records if row.get(key))['frame']=2
            with self.subTest(key=key),self.assertRaises(ValueError):self.audit(records=records)

    def test_reject_unretired_fences_leaks_and_product_claims(self):
        cases=(('nativeSdlCompositorDetached','consumerFenceObserved',299),
               ('nativeSdlCompositorDetached','producerFenceObserved',299))
        for key,field,value in cases:
            records=copy.deepcopy(self.records)
            next(row for row in records if row.get(key))[field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):self.audit(records=records)
        for field,value in (('liveImportedObjects',1),('nearNativePerformanceVerified',True),('cpuTextureUpload',True)):
            records=copy.deepcopy(self.records)
            next(row for row in records if row.get('nativeSdlCompositorQueued'))['qemuState'][field]=value
            with self.subTest(field=field),self.assertRaises(ValueError):self.audit(records=records)
        report=copy.deepcopy(self.report);report['ownedQemuControl']['liveAllocationMappings']=1
        with self.assertRaises(ValueError):self.audit(report=report)

    def test_reject_corrupted_complete_source_and_scaled_window(self):
        frames=dict(self.frames);name='relay-source-0000000000000001.rgba'
        bad=bytearray(frames[name]);bad[0]^=255;frames[name]=bytes(bad)
        with self.assertRaises(ValueError):self.audit(frames=frames)
        name='relay-window-0000000000000001.rgba';raw=self.frames[name]
        bad=bytearray(raw);bad[0]=0 if bad[0]>127 else 255
        with self.assertRaises(ValueError):pixel_audit(self.frames['compositor-copy-1.raw'],
            self.frames['relay-source-0000000000000001.rgba'],bytes(bad),2560,1566)

    def test_linear_reference_small_known_colors_and_row_order(self):
        source=bytes((0,0,0,255,100,0,0,255,0,100,0,255,100,100,0,255))
        self.assertEqual(linear_blit(source,2,2,1,1),bytes((50,50,0,255)))
        self.assertEqual(linear_blit(source,2,2,2,2),source[8:]+source[:8])


if __name__=='__main__':unittest.main()
