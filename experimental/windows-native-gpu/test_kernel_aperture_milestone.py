"""Corrupt real kernel/graphics receipts rather than accepting summary claims."""
import copy
import json
from pathlib import Path
import re
import unittest
from verify_kernel_aperture_capture import validate_kernel_aperture


class KernelApertureMilestone(unittest.TestCase):
    def setUp(self):
        self.evidence = json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/KERNEL-APERTURE-2026-10-12.json').read_text())
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None):
        p = proof or self.proof
        return validate_kernel_aperture(p['report'], '\n'.join(p['guestLines']), p['records'])

    def test_both_real_runs_pass_without_broader_claims(self):
        for run in self.evidence['runs']:
            with self.subTest(run=run['label']):
                result = self.check(run['proof'])
                self.assertTrue(result['kernelApertureVerified'])
                self.assertEqual((result['kernelFenceLeases'], result['kernelAllocationLeases']), (71,91))
                for key in ('drmDeviceVerified','dmaBufVerified','guestGraphicsIoctlKernelImplemented',
                            'desktopVerified','nearNativePerformanceVerified','openGlConformanceVerified','openGl4kVerified'):
                    self.assertIs(result[key], False)

    def test_missing_or_duplicate_contracts_rejected(self):
        lines = self.proof['guestLines']
        index = next(i for i,line in enumerate(lines) if line.startswith('APERTURE_CONTRACT_PASS'))
        for action in ('missing','duplicate'):
            with self.subTest(action=action):
                changed=copy.deepcopy(self.proof)
                if action=='missing': changed['guestLines'].pop(index)
                else: changed['guestLines'].insert(index,lines[index])
                with self.assertRaises(ValueError): self.check(changed)

    def test_revocation_or_module_cleanup_failure_rejected(self):
        for before,after in (('revokeSigbus=true','revokeSigbus=false'),('unloaded=true','unloaded=false'),
                             ('KERNEL_APERTURE_EXIT activeLeases=0','KERNEL_APERTURE_EXIT activeLeases=1'),
                             ('mmioReads=0 mmioWrites=0 unloaded=true','mmioReads=1 mmioWrites=0 unloaded=true')):
            with self.subTest(after=after):
                changed=copy.deepcopy(self.proof)
                changed['guestLines']=[line.replace(before,after) for line in changed['guestLines']]
                with self.assertRaises(ValueError): self.check(changed)

    def test_raw_sysfs_layout_rejected(self):
        self.proof['guestLines']=[line.replace(' kernel=true','') for line in self.proof['guestLines']]
        with self.assertRaises(ValueError): self.check()

    def test_missing_kernel_lease_rejected(self):
        index=next(i for i,line in enumerate(self.proof['guestLines']) if 'kernelApertureLease=true kind=2' in line)
        self.proof['guestLines'].pop(index)
        with self.assertRaises(ValueError): self.check()

    def test_duplicate_or_malformed_cookie_rejected(self):
        for bad in ('0','invalid','duplicate'):
            with self.subTest(cookie=bad):
                changed=copy.deepcopy(self.proof)
                indices=[i for i,line in enumerate(changed['guestLines']) if 'kernelApertureLease=true' in line]
                index=indices[1] if bad=='duplicate' else indices[0]
                value=re.search(r'cookie=(\d+)',changed['guestLines'][indices[0]])[1] if bad=='duplicate' else bad
                changed['guestLines'][index]=re.sub(r'cookie=\d+', 'cookie='+value,changed['guestLines'][index])
                with self.assertRaises(ValueError): self.check(changed)

    def test_wrong_kernel_lease_size_rejected(self):
        index=next(i for i,line in enumerate(self.proof['guestLines']) if 'kernelApertureLease=true kind=1' in line)
        self.proof['guestLines'][index]=self.proof['guestLines'][index].replace('bytes=4096','bytes=8192')
        with self.assertRaises(ValueError): self.check()

    def test_cleanup_before_render_completion_rejected(self):
        lines=self.proof['guestLines']
        index=next(i for i,line in enumerate(lines) if line.startswith('KERNEL_APERTURE_EXIT'))
        lines.insert(0,lines.pop(index))
        with self.assertRaises(ValueError): self.check()

    def test_real_pixels_and_native_retirement_still_required(self):
        self.proof['report']['nativeAsyncSubmissionRetirements'][-1]['observed']=0
        with self.assertRaises(ValueError): self.check()


if __name__ == '__main__':
    unittest.main()
