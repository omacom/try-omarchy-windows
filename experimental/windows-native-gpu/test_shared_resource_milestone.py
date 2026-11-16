"""Synthetic rejection tests, never physical GPU evidence."""
import unittest
from shared_resource_evidence import BEGIN, END, shared_resource_import_complete


class SharedImportTests(unittest.TestCase):
    def setUp(self):
        self.log = '\n'.join((BEGIN,
            'GPU_TRIANGLE_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2 vertices=3 tolerance=2',
            'LINUX_BRIDGE nativeVendorResourceCreated=true allocationCount=1 shared=true systemMemory=false',
            'GPU_TRIANGLE_TEST_COMPLETE verified=true width=130 height=73 rounds=2', END))
        self.probes = [dict(nativeSharedResourceImportProbe=True, verified=True, ntstatus=0, deviceHresult=0,
            importHresult=0, runtimeBytes=264, width=130, height=73, format=28, dimension=3, flags=1,
            sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)]
        self.cleanup = dict(sharedResourceProbes=1, importedSharedResources=1, failedSharedResourceProbes=0,
            driverCleanupVerified=True, liveVendorResources=0, liveVendorAllocations=0, cleanupFailures=0)

    def check(self, log=None, probes=None, cleanup=None, drawn=True):
        return shared_resource_import_complete(self.log if log is None else log,
            self.probes if probes is None else probes, self.cleanup if cleanup is None else cleanup, drawn)

    def test_requires_both_drawing_and_import(self):
        self.assertTrue(self.check())
        self.assertFalse(self.check(drawn=False))
        self.assertFalse(self.check(probes=[]))
        self.assertFalse(self.check(probes=self.probes * 2))

    def test_native_status_metadata_and_types_are_exact(self):
        for key, value in self.probes[0].items():
            for wrong in (None, not value if type(value) is bool else value + 1,
                          int(value) if type(value) is bool else str(value)):
                with self.subTest(key=key, wrong=wrong):
                    probe = dict(self.probes[0]); probe[key] = wrong
                    self.assertFalse(self.check(probes=[probe]))
        probe = dict(self.probes[0], importHresult=-2147467262)
        self.assertFalse(self.check(probes=[probe]))

    def test_requires_native_release_and_matching_counts(self):
        for key, value in self.cleanup.items():
            cleanup = dict(self.cleanup); del cleanup[key]
            self.assertFalse(self.check(cleanup=cleanup))
            cleanup[key] = not value if type(value) is bool else value + 1
            self.assertFalse(self.check(cleanup=cleanup))
            cleanup[key] = int(value) if type(value) is bool else bool(value)
            self.assertFalse(self.check(cleanup=cleanup))

    def test_plain_triangle_does_not_satisfy_shared(self):
        self.assertFalse(self.check(log=self.log.replace(BEGIN + '\n', '').replace('\n' + END, '')))
        self.assertFalse(self.check(log=self.log.replace('shared=true', 'shared=false')))
        self.assertFalse(self.check(log=self.log + '\n' + self.log.splitlines()[2]))

    def test_missing_duplicate_and_reordered_markers_rejected(self):
        for line in self.log.splitlines():
            self.assertFalse(self.check(log=self.log.replace(line, '')))
        for line in (BEGIN, END, self.log.splitlines()[1], self.log.splitlines()[3]):
            self.assertFalse(self.check(log=self.log + '\n' + line))
        self.assertFalse(self.check(log=self.log.replace(BEGIN, 'GPU_SHARED_RESOURCE_BOGUS')))
        self.assertFalse(self.check(log='\n'.join(reversed(self.log.splitlines()))))


if __name__ == '__main__':
    unittest.main()
