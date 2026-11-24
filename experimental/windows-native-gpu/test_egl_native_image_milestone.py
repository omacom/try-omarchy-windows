"""Reject corrupted real EGL buffer captures without loading a GPU driver in CI."""
import copy
import hashlib
import json
from pathlib import Path
import unittest

from verify_egl_native_image_capture import validate_egl_native_images


class EglNativeImageCheckpoint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parents[2]
        cls.evidence = json.loads((cls.root/'docs/evidence/EGL-NATIVE-IMAGE-DEVELOPMENT-2026-10-14.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None, enabled=True):
        p = self.proof if proof is None else proof
        return validate_egl_native_images(p['report'], '\n'.join(p['guestLines']), p['records'], enabled=enabled)

    def change(self, before, after):
        self.assertTrue(any(before in l for l in self.proof['guestLines']))
        self.proof['guestLines'] = [l.replace(before, after) for l in self.proof['guestLines']]

    def test_live_format_matrix_and_scope(self):
        self.assertEqual(len(self.evidence['runs']), 4)
        formats = set()
        for run in self.evidence['runs']:
            audit = self.check(run['proof'])
            self.assertEqual(audit, run['audit'])
            formats.add((audit['dxgiFormat'], audit['drmFourcc']))
            self.assertTrue(audit['eglNativeImageVerified'])
            self.assertEqual(audit['shader']['verifiedPixels'], 9490)
            for key in ('gbmVerified', 'drmDeviceVerified', 'desktopVerified', 'openGl4kVerified', 'nearNativePerformanceVerified'):
                self.assertIs(audit[key], False)
            for name, hashes in self.evidence['mesaPatch']['files'].items():
                self.assertEqual(run['source']['mesaSource'][name], hashes['patchedSha256'])
        self.assertEqual(formats, {(28,'34324241'), (28,'34324258'), (87,'34325241'), (87,'34325258')})
        picture = self.root/self.evidence['preview']['path']
        self.assertEqual(hashlib.sha256(picture.read_bytes()).hexdigest(), self.evidence['preview']['sha256'])
        self.assertIs(self.evidence['preview']['desktopScreenshot'], False)

    def test_disabled_and_retained_actual_failures(self):
        disabled = self.evidence['disabled']
        self.assertEqual(self.check(disabled['proof'], enabled=False), disabled['audit'])
        self.assertEqual(len(self.evidence['failures']), 3)
        for failed in self.evidence['failures']:
            with self.subTest(case=failed['case']), self.assertRaises(ValueError):
                self.check(failed['proof'])

    def test_export_metadata_cannot_be_forged(self):
        for before, after in (('stride=5124 offset=0', 'stride=5376 offset=0'),
                              ('fourcc=34325258 planes=1', 'fourcc=34325241 planes=1'),
                              ('planes=1 modifier=00ffffffffffffff', 'planes=1 modifier=0000000000000000'),
                              ('export=1 validFd=true', 'export=1 validFd=false')):
            with self.subTest(field=before):
                self.setUp(); self.change(before, after)
                with self.assertRaises(ValueError): self.check()

    def test_independent_shader_pixels(self):
        for n, line in enumerate(self.proof['guestLines']):
            if line.startswith('EGL_NATIVE_IMAGE_PIXEL_ROW y=0 '):
                self.proof['guestLines'][n] = line[:-2]+'00' # Alpha must be exactly 255.
                break
        else: self.fail('Missing retained pixels')
        with self.assertRaisesRegex(ValueError, 'pixel mismatch'): self.check()

    def test_import_rejections_and_lifetime_order(self):
        for before, after in (('kind=foreignFd error=3003', 'kind=foreignFd error=3000'),
                              ('sourceReleased=true firstImageDestroyed=true', 'sourceReleased=false firstImageDestroyed=true'),
                              ('inputFdRetained=true', 'inputFdRetained=false')):
            with self.subTest(field=before):
                self.setUp(); self.change(before, after)
                with self.assertRaises(ValueError): self.check()
        self.setUp()
        lines = self.proof['guestLines']
        a = next(i for i,l in enumerate(lines) if l.startswith('EGL_NATIVE_IMAGE_LIFETIME '))
        b = next(i for i,l in enumerate(lines) if l.startswith('EGL_NATIVE_IMAGE_IMPORTED '))
        lines[a], lines[b] = lines[b], lines[a]
        with self.assertRaisesRegex(ValueError, 'ordering'): self.check()

    def test_native_description_and_retirement(self):
        for field, value in (('format', 88), ('width', 1280), ('verified', False)):
            with self.subTest(field=field):
                self.setUp()
                record = next(r for r in self.proof['records'] if 'nativeSharedResourceImportProbe' in r)
                record[field] = value
                with self.assertRaises(ValueError): self.check()
        self.setUp(); self.proof['report']['nativeAsyncSubmissionRetirements'].pop()
        with self.assertRaises(ValueError): self.check()

    def test_cleanup_requires_release_before_exit(self):
        self.change('created=73 released=73 held=0 bytes=0 contextsDestroyed=true',
                    'created=73 released=72 held=1 bytes=3684156 contextsDestroyed=true')
        with self.assertRaises(ValueError): self.check()
        self.setUp(); self.proof['report']['ownedQemuControl']['qemuForcedStop'] = True
        with self.assertRaises(ValueError): self.check()

    def test_caps_and_provenance(self):
        self.change('formats=7 maxWidth=4096', 'formats=7 maxWidth=8192')
        with self.assertRaises(ValueError): self.check()
        self.setUp(); self.proof['report']['realWslDxgForwarding'] = True
        with self.assertRaises(ValueError): self.check()


if __name__ == '__main__':
    unittest.main()
