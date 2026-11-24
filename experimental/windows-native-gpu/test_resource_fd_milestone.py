"""Corrupt real resource-FD captures to reject false storage/lifetime milestones."""
import copy
import json
from pathlib import Path
import unittest
from verify_resource_fd_capture import validate_resource_fds, validate_resource_fd_control


class ResourceFdMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/RESOURCE-FD-2026-10-12.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None, validator=validate_resource_fds):
        p = self.proof if proof is None else proof
        return validator(p['report'], '\n'.join(p['guestLines']), p['records'])

    def test_actual_guests_prove_only_native_resource_fd_storage(self):
        for run in self.evidence['runs']:
            result = self.check(run['proof'])
            self.assertEqual(result, run['audit'])
            self.assertTrue(result['sharedStorageVerified'])
            self.assertEqual([r['verifiedPixels'] for r in result['rounds']], [8294400,8294400])
            self.assertEqual(result['diagnosticReadbackBytes'], 7864320)
            for key in ('desktopVerified','drmDeviceVerified','dmaBufVerified','openGl4kVerified',
                        'openGlConformanceVerified','nearNativePerformanceVerified'):
                self.assertIs(result[key], False)

    def test_actual_wrong_color_control_rejected(self):
        control = self.evidence['wrongColorControl']
        self.assertEqual(self.check(control['proof'], validate_resource_fd_control), control['audit'])
        with self.assertRaises(ValueError): self.check(control['proof'])
        self.assertEqual(self.evidence['knownFullReadbackFailure']['proof']['report']['disconnectCleanup']['vendorCpuSlotQuotaRejections'], 1)
        with self.assertRaises(ValueError): self.check(self.evidence['knownFullReadbackFailure']['proof'])

    def test_banded_evidence_cannot_be_promoted_to_full_readback(self):
        for run in self.evidence['runs']:
            p = run['proof']
            with self.assertRaises(ValueError):
                validate_resource_fds(p['report'], '\n'.join(p['guestLines']), p['records'], full_readback=True)
        with self.assertRaises(ValueError):
            p = self.evidence['knownFullReadbackFailure']['proof']
            validate_resource_fds(p['report'], '\n'.join(p['guestLines']), p['records'], full_readback=True)

    def test_allocation_layout_must_match_the_selected_audit(self):
        from verify_opengl_capture import validate_opengl_cleanup
        for slots in (256, 512, True, '128'):
            with self.assertRaises(ValueError):
                validate_opengl_cleanup(self.proof['report'], self.proof['records'], allocation_slots=slots)
        with self.assertRaises(ValueError):
            validate_resource_fds(self.proof['report'], '\n'.join(self.proof['guestLines']), self.proof['records'], full_readback='yes')

    def test_mixed_or_changed_kernel_layout_rejected(self):
        for line in ('LINUX_BRIDGE allocationCpuLayout=true slots=256 stride=1048576 kernel=true',
                     'LINUX_BRIDGE allocationCpuLayout=true slots=128 stride=65536 kernel=true',
                     'LINUX_BRIDGE fenceHubLayout=true slots=256 stride=4096 readonly=true kernel=true'):
            p = copy.deepcopy(self.proof)
            p['guestLines'].append(line)
            with self.assertRaises(ValueError): self.check(p)

    def test_missing_and_duplicate_receipts_rejected(self):
        for prefix in ('MESA_RESOURCE_FD_BEGIN','MESA_RESOURCE_FD_TEXTURE_CREATED','MESA_RESOURCE_FD_EXPORTED',
                       'MESA_RESOURCE_FD_IMPORTED','MESA_RESOURCE_FD_SHARED_STORAGE round=1',
                       'MESA_RESOURCE_FD_SHARED_STORAGE round=2','MESA_RESOURCE_FD_COMPLETE','RESOURCE_FD_CONTRACT_PASS'):
            for duplicate in (False,True):
                p = copy.deepcopy(self.proof)
                receipt = next(line for line in p['guestLines'] if line.startswith(prefix))
                p['guestLines'] = p['guestLines']+[receipt] if duplicate else [line for line in p['guestLines'] if line != receipt]
                with self.subTest(prefix=prefix, duplicate=duplicate), self.assertRaises(ValueError): self.check(p)

    def test_wrong_pixels_coverage_and_ownership_rejected(self):
        for before, after in (('fnv1a=95c9d10519374325','fnv1a=0000000000000000'),
                              ('rgba=00ff00ff','rgba=ff0000ff'),('pixels=8294400','pixels=8294399'),
                              ('width=3840 height=2160 pixels=','width=1920 height=1080 pixels='),
                              ('writer=source importedRead=true','writer=imported importedRead=true'),
                              ('sourceReleased=true fdClosed=true','sourceReleased=false fdClosed=true'),
                              ('bands=5 readbackBytes=7864320','bands=1 readbackBytes=33177600')):
            p = copy.deepcopy(self.proof); p['guestLines'] = [line.replace(before,after) for line in p['guestLines']]
            with self.subTest(after=after), self.assertRaises(ValueError): self.check(p)

    def test_missing_and_reordered_retirement_rejected(self):
        for mode in ('missing','reordered'):
            p=copy.deepcopy(self.proof)
            complete=next(i for i,line in enumerate(p['guestLines']) if line.startswith('MESA_RESOURCE_FD_COMPLETE '))
            indices=[i for i,line in enumerate(p['guestLines']) if line.startswith('LINUX_BRIDGE nativeHwQueueSignalQueued=') and
                     p['guestLines'].index('MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true') < i < complete]
            if mode=='missing': p['guestLines'].pop(indices[-1])
            else: p['guestLines'][indices[-1]],p['guestLines'][indices[-2]]=p['guestLines'][indices[-2]],p['guestLines'][indices[-1]]
            with self.subTest(mode=mode),self.assertRaises(ValueError): self.check(p)

    def test_native_metadata_type_and_handle_exposure_rejected(self):
        for marker,key,value in (('nativeResourceHandleExported','rawHandleExposed',True),
                                 ('nativeResourceFdOpenReturned','ntstatus',1),
                                 ('nativeResourceFdOpenReturned','hasKeyedMutex',True),
                                 ('nativeResourceFdOpenReturned','hasSync',True),
                                 ('nativeResourceFdOpenReturned','runtimeBytes',264),
                                 ('nativeResourceFdImported','allocations',2),
                                 ('nativeResourceFdImported','rawHandleExposed',True)):
            p=copy.deepcopy(self.proof); next(r for r in p['records'] if r.get(marker) is True)[key]=value
            with self.subTest(marker=marker,key=key),self.assertRaises(ValueError): self.check(p)
        p=copy.deepcopy(self.proof); closed=next(r for r in p['records'] if r.get('nativeResourceHandleClosed') is True)
        p['records'].remove(closed); p['records'].insert(0,closed)
        with self.assertRaises(ValueError): self.check(p)

    def test_abnormal_exit_failure_and_live_objects_rejected(self):
        for before,after in (('BRIDGE_RUNTIME_EXIT=0','BRIDGE_RUNTIME_EXIT=1'),
                             ('MESA_RESOURCE_FD_COMPLETE','MESA_RESOURCE_FD_FAILURE'),
                             ('BRIDGE_RUNTIME_EXIT=0','BRIDGE_RUNTIME_SIGNAL number=11 coreDumped=true\nBRIDGE_RUNTIME_EXIT=0')):
            p=copy.deepcopy(self.proof); p['guestLines']=[line.replace(before,after) for line in p['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check(p)
        for key,value in (('liveSharedResourceHandles',1),('closedResourceHandles',0),('vendorCpuSlotQuotaRejections',1)):
            p=copy.deepcopy(self.proof); p['report']['disconnectCleanup'][key]=value
            next(r for r in p['records'] if 'driverCleanupVerified' in r)[key]=value
            with self.subTest(key=key),self.assertRaises(ValueError): self.check(p)

    def test_wrong_color_control_cannot_hide_unrelated_failures(self):
        for before,after in (('stage=SharedPixels','stage=ReadbackMap'),('BRIDGE_RUNTIME_EXIT=1','BRIDGE_RUNTIME_EXIT=0'),
                             ('MESA_RESOURCE_FD_NEGATIVE_CONTROL','MESA_RESOURCE_FD_COMPLETE')):
            p=copy.deepcopy(self.evidence['wrongColorControl']['proof']); p['guestLines']=[line.replace(before,after) for line in p['guestLines']]
            with self.subTest(after=after),self.assertRaises(ValueError): self.check(p,validate_resource_fd_control)


if __name__ == '__main__': unittest.main()
