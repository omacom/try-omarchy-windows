"""Verify guest shared texture drawing and the separate native Windows import.

Importing and releasing a resource does not prove GPU copying or presentation.
"""
BEGIN = 'GPU_SHARED_RESOURCE_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM'
END = 'GPU_SHARED_RESOURCE_TEST_COMPLETE verified=true width=130 height=73 rounds=2'


def shared_resource_import_complete(log, probes, cleanup, triangle_verified, *, runtime_bytes=264):
    if type(runtime_bytes) is not int or not 0 < runtime_bytes <= 1024:
        return False
    lines = log.splitlines()
    records = [line for line in lines if line.startswith('GPU_SHARED_RESOURCE_')]
    if not triangle_verified or records != [BEGIN, END] or len(probes) != 1:
        return False
    triangle_begin = 'GPU_TRIANGLE_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2 vertices=3 tolerance=2'
    triangle_end = 'GPU_TRIANGLE_TEST_COMPLETE verified=true width=130 height=73 rounds=2'
    if lines.count(triangle_begin) != 1 or lines.count(triangle_end) != 1:
        return False
    if not (lines.index(BEGIN) < lines.index(triangle_begin) < lines.index(triangle_end) < lines.index(END)):
        return False
    expected = {'nativeSharedResourceImportProbe': True, 'verified': True, 'ntstatus': 0,
                'deviceHresult': 0, 'importHresult': 0, 'runtimeBytes': runtime_bytes,
                'width': 130, 'height': 73, 'format': 28, 'dimension': 3, 'flags': 1,
                'sharedNtHandleClosed': True, 'importedResourceReleased': True,
                'gpuCopy': False, 'presented': False}
    probe = probes[0]
    if not isinstance(probe, dict):
        return False
    if any(type(probe.get(k)) is not type(v) or probe[k] != v for k, v in expected.items()):
        return False
    clean_expected = dict(sharedResourceProbes=1, importedSharedResources=1, failedSharedResourceProbes=0,
                          driverCleanupVerified=True, liveVendorResources=0, liveVendorAllocations=0, cleanupFailures=0)
    return (all(type(cleanup.get(k)) is type(v) and cleanup[k] == v for k, v in clean_expected.items()) and
            lines.count('LINUX_BRIDGE nativeVendorResourceCreated=true allocationCount=1 shared=true systemMemory=false') == 1)
