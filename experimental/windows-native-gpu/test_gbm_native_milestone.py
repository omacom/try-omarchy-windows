"""Audit retained real GBM captures and reject corrupt pixels, ownership and scope."""
import copy
import hashlib
import json
from pathlib import Path
import unittest
from verify_gbm_native_capture import validate_gbm_native
from verify_opengl_capture import validate_opengl


class GbmNativeCheckpoint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root=Path(__file__).resolve().parents[2]
        cls.evidence=json.loads((cls.root/'docs/evidence/GBM-NATIVE-RENDER-DEVELOPMENT-2026-10-14.json').read_text())

    def setUp(self):
        self.proof=copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self,proof=None):
        p=self.proof if proof is None else proof
        return validate_gbm_native(p['report'],'\n'.join(p['guestLines']),p['records'])

    def change(self,before,after):
        self.assertTrue(any(before in line for line in self.proof['guestLines']))
        self.proof['guestLines']=[line.replace(before,after) for line in self.proof['guestLines']]

    def test_fresh_runs_and_pinned_mesa_source(self):
        self.assertEqual(len(self.evidence['runs']),2)
        patch=self.evidence['mesaPatch']
        local=json.loads((self.root/'experimental/windows-native-gpu/mesa/native-render-patch.json').read_text())
        self.assertEqual(patch,local)
        self.assertEqual(hashlib.sha256((self.root/'experimental/windows-native-gpu/mesa/0004-linux-d3d12-native-render-node.patch').read_bytes()).hexdigest(),patch['patchSha256'])
        for run in self.evidence['runs']:
            audit=self.check(run['proof'])
            self.assertEqual(audit,run['audit'])
            self.assertEqual(audit['verifiedStoragePixels'],954706)
            self.assertEqual(audit['sampledFrame']['verifiedPixels'],9490)
            for key in ('nativeRenderNodeVerified','gbmAllocationVerified','ownedImportedTextureSamplingVerified'):
                self.assertIs(audit[key],True)
            for key in ('desktopVerified','fullDrmDeviceVerified','kernelPrimeVerified','kmsVerified','nearNativePerformanceVerified','openGl4kVerified'):
                self.assertIs(audit[key],False)
            for name,value in patch['files'].items():
                self.assertEqual(run['source']['mesaSource'][name],value['patchedSha256'])

    def test_actual_failure_captures_stay_failures(self):
        self.assertEqual(len(self.evidence['failures']),2)
        for failed in self.evidence['failures']:
            with self.subTest(case=failed['case']),self.assertRaises(ValueError): self.check(failed['proof'])
        legacy=self.evidence['legacyRegression']; proof=legacy['proof']
        self.assertEqual(validate_opengl(proof['report'],'\n'.join(proof['guestLines']),proof['records'],
            transport='authenticated-broker'),legacy['audit'])
        self.assertIs(legacy['imageManifest']['guestRenderNodeIncluded'],False)
        self.assertIs(legacy['imageManifest']['nativeBuffersEnabled'],False)
        self.assertIs(legacy['explicitReleaseBeforeVmExitVerified'],False)

    def test_kernel_contract_and_no_forged_prime(self):
        for before,after in (('kernelPrime=false','kernelPrime=true'),('malformedInfo=7','malformedInfo=6'),
                             ('signature=003152444d444457','signature=003152444d444456'),('defaultDisabled=true','defaultDisabled=false')):
            with self.subTest(field=before):
                self.setUp(); self.change(before,after)
                with self.assertRaises(ValueError): self.check()

    def test_buffer_metadata_and_lifetime(self):
        for before,after in (('stride=5124','stride=5376'),('modifier=00ffffffffffffff','modifier=0000000000000000'),
                             ('sourceBoDestroyed=true','sourceBoDestroyed=false'),('textureAlive=true','textureAlive=false'),
                             ('created=73 released=73 held=0','created=73 released=72 held=1')):
            with self.subTest(field=before):
                self.setUp(); self.change(before,after)
                with self.assertRaises(ValueError): self.check()

    def test_sampled_pixels_and_gpu_origin(self):
        rows=self.proof['guestLines']
        index=next(i for i,line in enumerate(rows) if line.startswith('GBM_NATIVE_SAMPLED_PIXEL_ROW y=0 '))
        rows[index]=rows[index][:-2]+'00'
        with self.assertRaisesRegex(ValueError,'pixel mismatch'): self.check()
        self.setUp(); self.change('softwarePixels=false','softwarePixels=true')
        with self.assertRaises(ValueError): self.check()

    def test_native_import_description(self):
        for key,value in (('format',88),('width',1280),('sharedNtHandleClosed',False)):
            with self.subTest(field=key):
                self.setUp()
                next(r for r in self.proof['records'] if r.get('nativeSharedResourceImportProbe') is True and r.get('width')==1281)[key]=value
                with self.assertRaises(ValueError): self.check()

    def test_retirement_and_cleanup_before_vm_exit(self):
        self.proof['report']['nativeAsyncSubmissionRetirements'].pop()
        with self.assertRaises(ValueError): self.check()
        self.setUp(); self.proof['report']['ownedQemuControl']['allocationMappingsReleasedAfterVmExit']=1
        with self.assertRaises(ValueError): self.check()
        self.setUp(); self.proof['report']['ownedQemuControl']['qemuForcedStop']=True
        with self.assertRaises(ValueError): self.check()

    def test_sampling_and_teardown_order(self):
        lines=self.proof['guestLines']
        a=next(i for i,l in enumerate(lines) if l.startswith('GBM_NATIVE_SAMPLED source='))
        b=next(i for i,l in enumerate(lines) if l.startswith('OPENGL_FRAME frame=2 '))
        lines[a],lines[b]=lines[b],lines[a]
        with self.assertRaises(ValueError): self.check()
        self.setUp(); self.change('KERNEL_RENDER_EXIT unloaded=true nodeRemoved=true','KERNEL_RENDER_EXIT unloaded=true nodeRemoved=false')
        with self.assertRaises(ValueError): self.check()


if __name__=='__main__': unittest.main()
