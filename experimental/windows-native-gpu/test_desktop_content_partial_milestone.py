"""Retain the real wallpaper/window image while rejecting full-desktop claims."""
import base64
import hashlib
import json
from pathlib import Path
import struct
import unittest
import zlib
from verify_coordinated_gpu_clients import validate


class PartialDesktop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo=Path(__file__).resolve().parents[2]
        cls.value=json.loads((cls.repo/'docs/evidence/OMARCHY-4K-DESKTOP-CONTENT-PARTIAL-2026-10-19.json').read_text())

    def unpack(self,row,key):
        return zlib.decompress(base64.b64decode(row[key],validate=True))

    def test_exact_partial_image_and_native_readback_binding(self):
        image=self.value['image'];data=(self.repo/image['path']).read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),'b83166bd7292d42262e84cfc17543605c387152099d734d500be0e799ca64fd3')
        self.assertEqual(data[:8],b'\x89PNG\r\n\x1a\n');self.assertEqual(struct.unpack('>II',data[16:24]),(3840,2160))
        self.assertEqual((image['run'],image['frame']),(49,12))
        row=self.value['runs'][3];records=json.loads(self.unpack(row,'nativeRecordsZlibBase64'))
        capture=[r for r in records if r.get('nativeCompositorBinaryCapture') is True][-1]
        audit=[r for r in records if r.get('nativeCompositorRingAudited') is True][-1]
        self.assertEqual(audit['frame'],12);self.assertEqual(capture['fnv1a'],audit['fnv1a'])
        self.assertEqual(f"{capture['fnv1a']:016x}",row['rawFrame12Fnv1a'])
        self.assertEqual(image['sourceRawSha256'],row['rawFrame12Sha256'])

    def test_actual_nvidia_application_and_wallpaper_are_still_partial(self):
        row=self.value['runs'][3];guest=self.unpack(row,'guestZlibBase64').decode()
        self.assertIn('omarchy-gpu-app[2718]',guest)
        self.assertIn('Running on D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)',guest)
        self.assertIn('GL_OUT_OF_MEMORY',guest);self.assertIn('GPU reset (unknown)',guest)
        reference=self.value['wallpaperPixelCheck']
        self.assertEqual(reference['sourceJpegSha256'],'37746404ae9a4357442e520c34ea105780232ac3d08a9f289f00156e49f37c4e')
        self.assertEqual(len(reference['probes']),689)
        errors=[sum(abs(r[i]-r[i+3]) for i in (2,3,4))/3 for r in reference['probes']]
        self.assertAlmostEqual(sum(errors)/len(errors),reference['meanAbsoluteRgbError'])
        self.assertLess(reference['meanAbsoluteRgbError'],1)
        with self.assertRaises(ValueError):
            validate(json.loads(self.unpack(row,'reportZlibBase64')),guest,json.loads(self.unpack(row,'nativeRecordsZlibBase64')),
                     json.loads(self.unpack(row,'sourceZlibBase64')),profile=11)

    def test_all_failures_source_disk_and_scope_are_preserved(self):
        self.assertEqual([r['run'] for r in self.value['runs']],list(range(46,52)))
        self.assertFalse(self.value['fullDesktopAccepted']);self.assertEqual((self.value['verificationStages'],self.value['desktopGates']),(4,0))
        for row in self.value['runs']:
            self.assertFalse(row['fullDesktopAccepted'])
            source=json.loads(self.unpack(row,'sourceZlibBase64'));self.assertTrue(source['sourceImageUnchanged'])
            self.assertEqual(source['sourceImageSha256Before'],source['sourceImageSha256After'])
        final=json.loads(self.unpack(self.value['runs'][-1],'reportZlibBase64'))
        self.assertEqual(final['disconnectCleanup']['vendorCpuSlotQuotaRejections'],2)
        self.assertLess(final['disconnectCleanup']['peakVendorCpuBytes'],final['disconnectCleanup']['vendorCpuMappedByteLimit'])
        self.assertFalse(final['ownedQemuControl']['qemuForcedStop'])


if __name__=='__main__':
    unittest.main()
