"""Corrupt retained actual GPU process-transfer captures; CI performs no GPU work."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import unittest
import zlib
from verify_native_worker_capture import validate_worker_receipts


class NativeWorkerMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.evidence=json.loads((cls.root/'docs/evidence/WINDOWS-GPU-WORKER-2026-10-13.json').read_text())

    def setUp(self): self.run=copy.deepcopy(self.evidence['runs'][0])

    def check(self,run=None,present=False):
        r=self.run if run is None else run; p=r['proof']
        return validate_worker_receipts(p['report'],'\n'.join(p['guestLines']),p['records'],r['audit'],present=present)

    def marker(self,name): return next(r for r in self.run['proof']['records'] if r.get(name) is True)

    def replace(self,before,after):
        lines=self.run['proof']['guestLines']; self.assertTrue(any(before in l for l in lines))
        self.run['proof']['guestLines']=[l.replace(before,after) for l in lines]

    def test_real_copies_and_bounded_presentation_keep_their_actual_scopes(self):
        self.assertEqual(len(self.evidence['runs']),2)
        self.assertEqual(len(self.evidence['presentationRuns']),1)
        for present,runs in ((False,self.evidence['runs']),(True,self.evidence['presentationRuns'])):
            for run in runs:
                audit=self.check(run,present); self.assertEqual(audit,run['audit'])
                self.assertTrue(audit['windowsGpuProcessTransferVerified']); self.assertEqual(audit['guestClientUid'],65534)
                self.assertEqual(audit['kernelCpuViews'],173)
                self.assertEqual(audit['separateWindowsPresentationVerified'],present)
                for key in ('independentGuestGpuClientsVerified','desktopVerified','desktopStabilityVerified',
                            'nearNativePerformanceVerified','gpuSideCrossProcessWaitVerified','gbmVerified',
                            'dmaBufVerified','requiredApiMatrixVerified','allNvidiaFeaturesVerified','openGl4kVerified'):
                    self.assertIs(audit[key],False)

    def test_failed_presentations_and_successful_same_process_control_cannot_be_promoted(self):
        for run in self.evidence['failures']:
            candidate=dict(proof=run['proof'],audit=self.run['audit'])
            with self.subTest(label=run['label']),self.assertRaises(ValueError): self.check(candidate)
        control=self.evidence['sameProcessControl']; self.assertTrue(control['pixelAudit']['full4kDiagnosticVerified'])
        with self.assertRaises(ValueError): self.check(dict(proof=control['proof'],audit=control['pixelAudit']))

    def test_windows_process_identity_private_handles_and_replies_are_required(self):
        for marker,key,value in (
            ('nativeGpuWorkerStarted','pid',self.run['audit']['windowsCoordinatorPid']),
            ('nativeGpuWorkerStarted','privatePipes',False),('nativeGpuWorkerStarted','explicitInheritance',False),
            ('nativeGpuWorkerStarted','guestChoosesHandle',True),('nativeGpuWorkerInitialized','parentPidChosenByGuest',True),
            ('nativeGpuWorkerInitialized','ntHandleClosedAfterOpen',False),('nativeGpuWorkerConsumed','pid',1),
            ('nativeGpuWorkerConsumed','sequence',3),('nativeGpuWorkerConsumed','hresult',1),
            ('nativeGpuWorkerConsumed','fnv1a',0),('nativeGpuWorkerShutdown','resourcesReleased',False),
            ('nativeGpuWorkerExited','exit',1),('nativeGpuWorkerExited','forced',True),('nativeGpuWorkerExited','failed',True)):
            self.setUp(); self.marker(marker)[key]=value
            with self.subTest(marker=marker,key=key),self.assertRaises(ValueError): self.check()

    def test_lifecycle_cannot_be_missing_repeated_or_reordered(self):
        for name in ('nativeGpuWorkerStarted','nativeGpuWorkerInitialized','nativeGpuWorkerConsumed',
                     'nativeGpuWorkerShutdown','nativeGpuWorkerExited'):
            for duplicate in (False,True):
                self.setUp(); records=self.run['proof']['records']; selected=self.marker(name)
                records.append(copy.deepcopy(selected)) if duplicate else records.remove(selected)
                with self.subTest(name=name,duplicate=duplicate),self.assertRaises(ValueError): self.check()
        self.setUp(); records=self.run['proof']['records']; closed=self.marker('nativeGpuWorkerExited')
        records.remove(closed); records.insert(records.index(self.marker('nativeGpuWorkerConsumed')),closed)
        with self.assertRaises(ValueError): self.check()

    def test_native_counters_and_kernel_mapping_lifetime_are_not_optional(self):
        for key,value in (('startedNativeGpuWorkers',2),('liveNativeGpuWorkers',1),('forcedNativeGpuWorkers',1),
                          ('failedNativeGpuWorkers',1),('transferredNativeTextureHandles',0),('vendorCpuMappedByteLimit',192*1024*1024)):
            self.setUp(); p=self.run['proof']; p['report']['disconnectCleanup'][key]=value
            self.marker('driverCleanupVerified')[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check()
        for before,after in (('viewsCreated=173','viewsCreated=172'),('viewsReleased=173','viewsReleased=172'),
                             ('retainedViews=0','retainedViews=1'),('rejected=0','rejected=1'),
                             ('transport=authenticated-broker nativeWorkerClients=1','transport=virtio-port')):
            self.setUp(); self.replace(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()

    def test_ordinary_client_shader_and_display_workload_selection_remain_strict(self):
        for before,after in (('uid=65534 gid=65534','uid=0 gid=0'),('effectiveCapabilities=0','effectiveCapabilities=1'),
                             ('noNewPrivs=true','noNewPrivs=false'),('controllerOpen=denied','controllerOpen=allowed'),
                             ('nativePortOpen=denied','nativePortOpen=allowed'),('protocol=3','protocol=2'),
                             ('observedHash=b64b8f45bcb78879','observedHash=0000000000000000')):
            self.setUp(); self.replace(before,after)
            with self.subTest(after=after),self.assertRaises(ValueError): self.check()
        with self.assertRaises(ValueError): self.check(present=True)
        self.run=copy.deepcopy(self.evidence['presentationRuns'][0])
        with self.assertRaises(ValueError): self.check()
        self.marker('nativeSharedTexturePresented')['presentProcessId']=self.run['audit']['windowsCoordinatorPid']
        with self.assertRaises(ValueError): self.check(present=True)

    def test_presentation_pass_is_not_a_claim_of_stability_or_a_desktop(self):
        self.run=copy.deepcopy(self.evidence['presentationRuns'][0])
        for key in ('lab4kRenderingVerified','sharedHandoffVerified','full4kDiagnosticVerified','dxgiPresentationVerified'):
            old=self.run['audit'][key]; self.run['audit'][key]=False
            with self.subTest(key=key),self.assertRaises(ValueError): self.check(present=True)
            self.run['audit'][key]=old
        for key in ('guestDesktopAcceleratedByThisBackend','nativePerformanceMeasured','graphicsBackendProductReady'):
            self.run['audit'][key]=True
            with self.subTest(key=key),self.assertRaises(ValueError): self.check(present=True)
            self.run['audit'][key]=False

    def test_preview_contains_the_exact_native_4k_readback_without_resampling(self):
        meta=self.evidence['shaderImage']; data=(self.root/meta['file']).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),meta['pngSha256']); self.assertEqual(data[:8],b'\x89PNG\r\n\x1a\n')
        pos=8; compressed=b''
        while pos<len(data):
            size=struct.unpack('>I',data[pos:pos+4])[0]; kind=data[pos+4:pos+8]; payload=data[pos+8:pos+8+size]
            self.assertEqual(struct.unpack('>I',data[pos+8+size:pos+12+size])[0],zlib.crc32(kind+payload)&0xffffffff)
            if kind==b'IHDR': self.assertEqual(struct.unpack('>IIBBBBB',payload),(3840,2160,8,6,0,0,0))
            if kind==b'IDAT': compressed+=payload
            pos+=12+size
        raw=zlib.decompress(compressed); self.assertEqual(len(raw),2160*15361)
        self.assertTrue(all(raw[y*15361]==0 for y in range(2160)))
        pixels=b''.join(raw[y*15361+1:(y+1)*15361] for y in range(2160))
        self.assertEqual(hashlib.sha256(pixels).hexdigest(),meta['denseRgbaSha256'])
        self.assertEqual(meta['denseRgbaSha256'],self.evidence['runs'][0]['audit']['frames'][1]['rgbaSha256'])
        self.assertFalse(meta['windowScreenshot']); self.assertFalse(meta['hyprland'])


if __name__=='__main__': unittest.main()
