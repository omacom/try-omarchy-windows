"""Reject false OpenGL milestones using retained real hardware receipts."""
import copy
import json
from pathlib import Path
import unittest
from test_owned_runtime_qemu import async_submissions_complete
from verify_opengl_capture import validate_opengl


class OpenGlMilestone(unittest.TestCase):
    def setUp(self):
        evidence = json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/OPENGL-2026-10-11.json').read_text())
        proof = evidence['proof']
        self.report, self.guest, self.records = proof['report'], '\n'.join(proof['guestLines']), proof['records']

    def check(self):
        return validate_opengl(self.report, self.guest, self.records)

    def test_actual_shader_receipts_pass_with_limited_claim(self):
        result = self.check()
        self.assertTrue(result['boundedOpenGlShaderVerified'])
        self.assertEqual([f['maximumChannelError'] for f in result['frames']], [1,1])
        for key in ('desktopVerified','openGlConformanceVerified','openGl4kVerified','nearNativePerformanceVerified'):
            self.assertIs(result[key], False)

    def test_old_submission_lifetime_limit_stays_unchanged(self):
        d = self.report
        self.assertFalse(async_submissions_complete(d['guestCommandEnqueues'],d['nativeAsyncSubmissionAcceptances'],
                         d['nativeAsyncSubmissionRetirements'],d['disconnectCleanup']))

    def test_wrong_renderer_or_profile_rejected(self):
        for old,new in (('D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)','llvmpipe'),
                        ('4.6 (Core Profile) Mesa 26.0.8','OpenGL ES 3.2 Mesa 26.0.8'),
                        ('frames=2 display=false','frames=2 display=true'),
                        ('clientHint=16','clientHint=12')):
            with self.subTest(new=new):
                saved = self.guest; self.guest = saved.replace(old,new)
                with self.assertRaises(ValueError): self.check()
                self.guest = saved

    def test_pixel_corruption_or_missing_row_rejected(self):
        lines = self.guest.splitlines()
        index = next(i for i,line in enumerate(lines) if line.startswith('OPENGL_PIXEL_ROW frame=1 y=0 '))
        for action in ('corrupted','missing','duplicate','reordered'):
            with self.subTest(action=action):
                changed = lines.copy()
                if action == 'corrupted': changed[index] = changed[index].replace('rgba=0000ffff','rgba=ff00ffff',1)
                if action == 'missing': changed.pop(index)
                if action == 'duplicate': changed.insert(index,changed[index])
                if action == 'reordered': changed[index],changed[index+1] = changed[index+1],changed[index]
                saved = self.guest; self.guest = '\n'.join(changed)
                with self.assertRaises(ValueError): self.check()
                self.guest = saved

    def test_failure_or_incomplete_teardown_rejected(self):
        for old,new in (('BRIDGE_RUNTIME_EXIT=0','BRIDGE_RUNTIME_EXIT=1'),
                        ('frame=2 vertices=3 error=0000','frame=2 vertices=3 error=0505'),
                        ('OPENGL_TEST_COMPLETE','OPENGL_FAILURE')):
            with self.subTest(new=new):
                saved = self.guest; self.guest = saved.replace(old,new)
                with self.assertRaises(ValueError): self.check()
                self.guest = saved

    def test_unretired_native_command_rejected(self):
        self.report['nativeAsyncSubmissionRetirements'][-1]['observed'] = 0
        with self.assertRaises(ValueError): self.check()

    def test_unaccounted_vm_mappings_rejected(self):
        for key in ('fenceMappingsReleasedAfterVmExit','allocationMappingsReleasedAfterVmExit','allocationSlotMappingsReleasedAfterVmExit'):
            with self.subTest(key=key):
                saved = copy.deepcopy(self.report['ownedQemuControl'])
                self.report['ownedQemuControl'][key] += 1
                with self.assertRaises(ValueError): self.check()
                self.report['ownedQemuControl'] = saved

    def test_unsafe_or_malformed_profile_rejected(self):
        profile = next(r for r in self.records if 'openGlProfileNegotiated' in r)
        for key,value in (('pendingCommands',17),('reservedByteLimit',1<<40),('commandLimit',True)):
            with self.subTest(key=key):
                old = profile[key]; profile[key] = value
                with self.assertRaises(ValueError): self.check()
                profile[key] = old

    def test_global_shared_handle_rejected(self):
        for record in self.records:
            if record.get('flags') in (131,1155): record['globalSharedHandle'] = True
        with self.assertRaises(ValueError): self.check()


if __name__ == '__main__':
    unittest.main()
