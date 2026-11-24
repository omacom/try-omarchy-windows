"""Corrupt actual broker-rendered shader and native-finalizer captures."""
import copy
import json
import hashlib
from pathlib import Path
import unittest
import struct
import re
import zlib
from verify_broker_routing_capture import validate_broker_routing
from verify_opengl_capture import validate_opengl


class BrokerRoutingMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence=json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/BROKER-ROUTING-2026-10-13.json').read_text())

    def setUp(self): self.run=copy.deepcopy(self.evidence['runs'][0])

    def check(self,run=None):
        run=self.run if run is None else run; p=run['proof']
        return validate_broker_routing(p['report'],'\n'.join(p['guestLines']),p['records'],finalizer=run['finalizer'])

    def test_all_real_guests_pass_only_their_actual_scopes(self):
        for run in self.evidence['runs']:
            result=self.check(run); self.assertEqual(result,run['audit'])
            self.assertTrue(result['brokerRoutingVerified']); self.assertEqual(result['clientUid'],65534)
            self.assertEqual(result['retiredNativeCommands'],18)
            self.assertEqual(result['brokerNativeFinalizerVerified'],run['finalizer'])
            for flag in ('multiClientGpuRoutingVerified','crossProcessGpuResourceImportVerified','gbmVerified',
                         'desktopVerified','desktopStabilityVerified','nearNativePerformanceVerified','openGl4kVerified'):
                self.assertIs(result[flag],False)

    def test_retained_hardware_failures_stay_failures(self):
        for failure in self.evidence['failures']:
            with self.subTest(label=failure['label']),self.assertRaises(ValueError):
                self.check(dict(proof=failure['proof'],finalizer=False))

    def test_corrected_preview_preserves_exact_logged_top_origin_pixels(self):
        root=Path(__file__).resolve().parents[2]
        meta=json.loads((root/'docs/evidence/BROKER-ROUTING-IMAGE-2026-10-13.json').read_text())
        data=(root/meta['pngFile']).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),meta['pngSha256'])
        self.assertEqual(data[:8],b'\x89PNG\r\n\x1a\n')
        offset=8; compressed=b''
        while offset<len(data):
            size=struct.unpack('>I',data[offset:offset+4])[0]; kind=data[offset+4:offset+8]
            payload=data[offset+8:offset+8+size]
            self.assertEqual(struct.unpack('>I',data[offset+8+size:offset+12+size])[0],zlib.crc32(kind+payload)&0xffffffff)
            if kind==b'IHDR': self.assertEqual(struct.unpack('>IIBBBBB',payload),(130,73,8,6,0,0,0))
            if kind==b'IDAT': compressed+=payload
            offset+=12+size
        raw=zlib.decompress(compressed); self.assertEqual(len(raw),73*521)
        rows={int(y):bytes.fromhex(p) for y,p in re.findall(r'^OPENGL_PIXEL_ROW frame=2 y=(\d+) rgba=([0-9a-f]+)$',
              '\n'.join(self.evidence['runs'][0]['proof']['guestLines']),re.M)}
        for y in range(73):
            self.assertEqual(raw[y*521],0); self.assertEqual(raw[y*521+1:(y+1)*521],rows[y])
        self.assertEqual(hashlib.sha256(b''.join(rows[y] for y in range(73))).hexdigest(),meta['denseRgbaSha256'])

    def test_wrong_client_worker_privileges_and_lifetime_rejected(self):
        changes=(('uid=65534 gid=65534','uid=0 gid=0'),('effectiveCapabilities=0','effectiveCapabilities=1'),
                 ('noNewPrivs=true','noNewPrivs=false'),('controllerOpen=denied','controllerOpen=allowed'),
                 ('nativePortOpen=denied','nativePortOpen=allowed'),('clientUid=65534','clientUid=0'),
                 ('nativeWorkerClients=1','nativeWorkerClients=2'),('kernelControllersClient=false','kernelControllersClient=true'),
                 ('processExited=true','processExited=false'),('rejected=0','rejected=1'),('multiClient=false','multiClient=true'))
        for before,after in changes:
            self.setUp(); self.run['proof']['guestLines']=[l.replace(before,after) for l in self.run['proof']['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()

    def test_mapping_and_cleanup_corruptions_rejected(self):
        for before,after in (('retainedViews=96','retainedViews=95'),('viewsCreated=162','viewsCreated=161'),
                             ('viewsReleased=66','viewsReleased=65'),('nativeCalls=1014','nativeCalls=10001'),
                             ('BROKER_ROUTE_REAP exit=0','BROKER_ROUTE_REAP exit=1'),('BRIDGE_RUNTIME_EXIT=0','BRIDGE_RUNTIME_EXIT=1')):
            self.setUp(); self.run['proof']['guestLines']=[l.replace(before,after) for l in self.run['proof']['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
        for prefix in ('BROKER_RENDER_CLIENT ','BROKER_ROUTE_START ','BROKER_ROUTE_EXIT ','BROKER_ROUTE_REAP '):
            for duplicate in (False,True):
                self.setUp(); lines=self.run['proof']['guestLines']; line=next(l for l in lines if l.startswith(prefix))
                self.run['proof']['guestLines']=lines+[line] if duplicate else [l for l in lines if l!=line]
                with self.subTest(prefix=prefix,duplicate=duplicate),self.assertRaises(ValueError): self.check()

    def test_direct_and_broker_transports_cannot_be_substituted_or_mixed(self):
        p=self.run['proof']
        with self.assertRaises(ValueError): validate_opengl(p['report'],'\n'.join(p['guestLines']),p['records'],allocation_slots=256)
        for mixed in (False,True):
            self.setUp(); lines=self.run['proof']['guestLines']; direct='LINUX_BRIDGE transport=virtio-port'
            self.run['proof']['guestLines']=lines+[direct] if mixed else [direct if l.startswith('LINUX_BRIDGE transport=') else l for l in lines]
            with self.subTest(mixed=mixed),self.assertRaises(ValueError): self.check()

    def test_real_finalizer_must_complete_before_broker_retirement(self):
        for corrupt in ('missing','late','wrong-value'):
            self.run=copy.deepcopy(self.evidence['runs'][-1]); self.assertTrue(self.run['finalizer'])
            lines=self.run['proof']['guestLines']; complete='EXIT_LIFETIME_FINALIZER_COMPLETE observed=57 mappingLive=true'
            if corrupt=='missing': lines.remove(complete)
            elif corrupt=='wrong-value': lines[lines.index(complete)]=complete.replace('57','56')
            else:
                end=next(l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')); lines.remove(end); lines.insert(lines.index(complete),end)
            with self.subTest(corrupt=corrupt),self.assertRaises(ValueError): self.check()


if __name__=='__main__': unittest.main()
