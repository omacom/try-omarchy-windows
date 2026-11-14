"""Reject false capacity milestones by corrupting retained real-GPU captures."""
import copy
import json
from pathlib import Path
import unittest
from verify_cpu_capacity_capture import validate_cpu_capacity, unpack
from verify_resource_fd_capture import validate_resource_fds


class CpuCapacityMilestone(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((Path(__file__).resolve().parents[2]/'docs/evidence/CPU-CAPACITY-2026-10-12.json').read_text())

    def setUp(self):
        self.proof = copy.deepcopy(self.evidence['runs'][0]['proof'])

    def check(self, proof=None):
        return validate_resource_fds(*unpack(self.proof if proof is None else proof), full_readback=True)

    def replace(self, before, after):
        self.proof['guestLines'] = [line.replace(before, after) for line in self.proof['guestLines']]

    def cleanup(self, key, value):
        self.proof['report']['disconnectCleanup'][key] = value
        next(r for r in self.proof['records'] if 'driverCleanupVerified' in r)[key] = value

    def test_two_full_buffer_guests_and_failed_control(self):
        self.assertEqual(validate_cpu_capacity(self.evidence), self.evidence['audit'])
        for run in self.evidence['runs']:
            audit = self.check(run['proof'])
            self.assertEqual(audit, run['audit'])
            self.assertEqual(audit['nativeFullBufferMappingBytes'], 33226752)
            self.assertEqual(audit['retiredNativeCommands'], 24)
            self.assertTrue(audit['fullBufferReadbackVerified'])
            for key in ('desktopVerified', 'desktopStabilityVerified', 'nearNativePerformanceVerified',
                        'generalApertureFragmentationSolved', 'openGl4kVerified', 'dmaBufVerified'):
                self.assertIs(audit[key], False)
        with self.assertRaises(ValueError): self.check(self.evidence['oldLayoutControl']['proof'])

    def test_full_buffer_selection_and_marker_required(self):
        with self.assertRaises(ValueError): validate_resource_fds(*unpack(self.proof))
        self.proof['guestLines'] = [l for l in self.proof['guestLines'] if not l.startswith('MESA_RESOURCE_FD_READBACK_PROFILE')]
        with self.assertRaises(ValueError): self.check()

    def test_wrong_layout_and_physical_budget_rejected(self):
        for key, value in (('vendorCpuSlotLimit', 128), ('vendorCpuSlotLimit', 512),
                           ('vendorCpuMappedByteLimit', 192*1024*1024),
                           ('peakVendorCpuBytes', 96*1024*1024+4096),
                           ('peakVendorCpuBytes', 33173504), ('vendorCpuSlotQuotaRejections', 1)):
            self.setUp(); self.cleanup(key, value)
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): self.check()

    def test_native_region_bounds_and_ownership_required(self):
        for key, value in (('status', 1), ('regionBytes', 33177600), ('state', 8192),
                           ('protection', 64), ('baseMatches', False), ('allocationBaseMatches', False),
                           ('regionBaseOffsetBytes', 4096), ('exportedRangeContained', False)):
            self.setUp()
            next(r for r in self.proof['records'] if r.get('vendorCpuRegionCandidate') is True and
                 r.get('expectedBytes') == 33226752)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): self.check()

    def test_full_buffer_lease_must_match_guest_native_lock(self):
        for before, after in (
                ('allocationCpuMapped=true direct=true loads=10000 bytes=33226752',
                 'allocationCpuMapped=true direct=true loads=10000 bytes=33177600'),
                ('nativeVendorCpuLocked=true bytes=33226752', 'nativeVendorCpuLocked=true bytes=33177600'),
                ('offset=110100480 generation=106', 'offset=267386880 generation=106')):
            self.setUp(); self.replace(before, after)
            with self.subTest(after=after), self.assertRaises(ValueError): self.check()
        self.setUp()
        lease = next(i for i,l in enumerate(self.proof['guestLines']) if 'kernelApertureLease=true kind=2 bytes=33226752' in l)
        mapping = self.proof['guestLines'][lease:lease+3]
        del self.proof['guestLines'][lease:lease+3]
        end = next(i for i,l in enumerate(self.proof['guestLines']) if l.startswith('MESA_RESOURCE_FD_COMPLETE'))
        self.proof['guestLines'][end:end] = mapping
        with self.assertRaises(ValueError): self.check()

    def test_banded_receipts_wrong_pixels_and_retirement_rejected(self):
        for before, after in (
                ('bands=1 readbackBytes=33177600', 'bands=5 readbackBytes=7864320'),
                ('fnv1a=95c9d10519374325', 'fnv1a=0000000000000000'),
                ('sourceReleased=true fdClosed=true', 'sourceReleased=false fdClosed=true'),
                ('nativeHwQueueSignalQueued=true flags=4 queues=1 target=2',
                 'nativeHwQueueSignalQueued=true flags=4 queues=1 target=3'),
                ('BRIDGE_RUNTIME_EXIT=0', 'BRIDGE_RUNTIME_SIGNAL number=11 coreDumped=true\nBRIDGE_RUNTIME_EXIT=0')):
            self.setUp(); self.replace(before, after)
            with self.subTest(after=after), self.assertRaises(ValueError): self.check()

    def test_comparison_must_keep_native_guest_and_kernel_inputs(self):
        for key in ('driverBridgeSha256', 'kernelSha256', 'initramfsSha256', 'qemuSha256'):
            evidence = copy.deepcopy(self.evidence)
            evidence['runs'][1]['proof']['report'][key] = '0'*64
            with self.subTest(key=key), self.assertRaises(ValueError): validate_cpu_capacity(evidence)

    def test_control_failure_and_cleanup_cannot_be_rewritten(self):
        for before, after in (('stage=ReadbackMap', 'stage=SharedPixels'), ('BRIDGE_RUNTIME_EXIT=1', 'BRIDGE_RUNTIME_EXIT=0'),
                              ('unloaded=true', 'unloaded=false')):
            evidence = copy.deepcopy(self.evidence)
            proof = evidence['oldLayoutControl']['proof']
            proof['guestLines'] = [l.replace(before,after) for l in proof['guestLines']]
            with self.subTest(after=after), self.assertRaises(ValueError): validate_cpu_capacity(evidence)


if __name__ == '__main__': unittest.main()
