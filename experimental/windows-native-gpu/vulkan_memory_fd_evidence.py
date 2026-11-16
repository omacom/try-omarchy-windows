"""Keep FD lifetime, native buffer admission and GPU data execution distinct."""
import json
import re
from async_cpu_wait_evidence import fields, one, exact
from verify_4k_capture import require, LIVE_COUNTERS
from test_owned_runtime_qemu import async_submissions_complete, hw_queue_signals_complete

BASE_STAGES = ('create-instance', 'count-devices', 'enumerate-devices', 'create-device',
    'memory-fd-buffer', 'memory-fd-allocate')
REQUIRED_NATIVE_COUNTERS = ('liveNativeGpuWorkers', 'forcedNativeGpuWorkers', 'failedNativeGpuWorkers',
    'failedDeviceExecutionQueries', 'liveDeviceExecutionStates', 'liveSharedResourceHandles', 'liveSharedSyncHandles',
    'failedSharedTextureConsumes', 'failedCompositorFrames', 'failedCompositorCaptures', 'failedSharedResourceProbes',
    'failedContextPriorityChanges', 'failedHwQueues', 'hwQueuesReleasedAfterVmExit', 'failedSyncObjects',
    'failedNativeContextSignals', 'nativeContextSignalTimeouts', 'pendingAsyncContextSignals', 'syncObjectsReleasedAfterVmExit',
    'failedGpuReservations', 'gpuReservationsReleasedAfterVmExit', 'failedGpuStateMaps', 'pendingNativeSubmissions',
    'failedHwQueueSignals', 'pendingHwQueueSignals', 'failedGpuQueueWaits', 'pendingGpuQueueWaits',
    'failedNativeSubmissions', 'nativeSubmissionTimeouts', 'failedAdapterQueries', 'failedVendorAllocations',
    'failedVendorGpuVaMaps', 'failedMemoryBudgetQueries', 'liveRetainedResidencyReferences', 'failedVendorEvictions',
    'failedVendorResidencyRequests', 'failedVendorCpuLocks', 'failedVendorCpuUnlocks', 'gpuStateRetirementTimeouts',
    'failedCpuStoreTests', 'cpuLocksReleasedAfterVmExit', 'failedAllocationTranslations')
REQUIRED_VM_COUNTERS = ('liveFenceMappings', 'fenceMappingsReleasedAfterVmExit', 'liveAllocationMappings',
    'liveAllocationMappedBytes', 'allocationMappingsReleasedAfterVmExit', 'liveAllocationSlotMappings',
    'allocationSlotMappingsReleasedAfterVmExit')


def validate_fd_receipts(log, profile):
    require(profile in ('pass', 'bad-export', 'buffer-refusal'), 'Unknown FD profile')
    exact(one(log, 'EVENTFD_CONTRACT_PASS'), dict(checks=31, published=0, notified=0, gpu='false',
        partialRollback='true', foreignOwnerRejected='true'))
    exact(one(log, 'VULKAN_MEMORY_FD_BEGIN'), dict(bufferBytes=65536, exports=32, cpuWrites='false', window='false'))
    stages = list(BASE_STAGES)
    if profile != 'buffer-refusal': stages += ['memory-fd-bind'] + ['memory-fd-export'] * (32 if profile == 'pass' else 2)
    if profile == 'pass': stages += ['memory-fd-idle-before-free', 'device-idle']
    results = [fields(line, 'VULKAN_BRIDGE_RESULT') for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_RESULT ')]
    expected = [dict(stage=stage, result='0') for stage in stages]
    if profile == 'buffer-refusal': expected[-1]['result'] = '-1000072003'
    require(results == expected, 'Missing, failed or reordered Vulkan FD stages')
    failures = [line for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_FAIL ')]
    if profile == 'pass':
        require(not failures, 'Vulkan FD workload failed')
        exact(one(log, 'VULKAN_MEMORY_FD_COMPLETE'), dict(verified='true', exports=32, unique='true', cloexec='true',
            validAfterMemoryFree='true', nvidiaUmdLoaded='true', cpuWrites='false', window='false'))
        exact(one(log, 'VULKAN_MEMORY_FD_DEVICE_READY'), dict(verified='true', vendor=4318, computeQueue='true',
            nvidiaUmdLoaded='true', submissions=0, window='false'))
    else:
        expected_failure = 'memory-fd-allocate' if profile == 'buffer-refusal' else 'Vulkan export did not return a fresh valid descriptor'
        require(failures == ['VULKAN_BRIDGE_FAIL ' + expected_failure] and
            'VULKAN_MEMORY_FD_COMPLETE' not in log and 'VULKAN_MEMORY_FD_DEVICE_READY' not in log,
            'Wrong FD rejection boundary or rejected control promoted')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.M) == ['0' if profile == 'pass' else '1'], 'Wrong guest exit')
    require(len(re.findall(r'^VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 \(NVIDIA GeForce RTX 5090 Laptop GPU\)$', log, re.M)) == 1,
        'Missing NVIDIA hardware device')
    count = 3 if profile == 'pass' else 1
    exact(one(log, 'KERNEL_EVENTFD_EXIT'), dict(held=0, pending=0, created=count, notified=count, errors=0, released=count, cleanup='true'))
    require(log.count('asyncCpuFenceWaitRegistered=true') == count and not any(marker in log for marker in
        ('EVENTFD_CONTRACT_FAIL', 'APERTURE_CONTRACT_FAIL', 'VULKAN_COMPUTE_COMPLETE')), 'Wrong notification/workload evidence')
    exported = 0 if profile == 'buffer-refusal' else 1
    exact(one(log, 'BROKER_OBJECT_SUMMARY'), dict(exported=exported, published=exported, resolved=0,
        reaped=exported, closed=exported, retained=0))
    return dict(profile=profile, descriptorsVerified=32 if profile == 'pass' else 0,
        lifetimeAfterCreatorFreeVerified=profile == 'pass', kernelNotifications=count,
        gpuPayloadImportVerified=False, gpuPayloadContentsVerified=False,
        windowPresentationVerified=False, dailyUseAccepted=False)


def validate_fd_cleanup(report, records, profile):
    """FD workload has five initialization submits, no application shader submit."""
    require(profile in ('pass', 'bad-export', 'buffer-refusal'), 'Unknown FD cleanup profile')
    cleanup, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(type(report.get('hostBridgeExit')) is int and report['hostBridgeExit'] == 0 and
        cleanup.get('driverCleanupVerified') is True and type(cleanup.get('cleanupFailures')) is int and
        cleanup['cleanupFailures'] == 0, 'Native cleanup unverified')
    for key in LIVE_COUNTERS:
        require(type(cleanup.get('live' + key)) is int and cleanup['live' + key] == 0, 'Missing/live native object: ' + key)
    for section, keys in ((cleanup, REQUIRED_NATIVE_COUNTERS), (vm, REQUIRED_VM_COUNTERS)):
        require(all(type(section.get(key)) is int for key in keys), 'Missing native/VM debt counter')
    for section in (cleanup, vm):
        for key, value in section.items():
            if key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith(('Timeouts', 'ReleasedAfterVmExit')):
                expected = 1 if profile == 'buffer-refusal' and key == 'failedSharedResourceProbes' else 0
                require(type(value) is int and value == expected, 'Failed or deferred work: ' + key)
    require(vm.get('ownedQemuExited') is True and type(vm.get('qemuExit')) is int and vm['qemuExit'] == 0 and
        vm.get('qemuForcedStop') is False and vm.get('fenceControlFailed') is False and
        type(vm.get('fenceApertureSlots')) is int and vm['fenceApertureSlots'] == 128 and
        type(vm.get('allocationApertureSlots')) is int and vm['allocationApertureSlots'] == 256, 'VM lifetime/geometry differs')
    for created, acknowledged in (('fenceMappingsCreated', 'fenceUnmapAcknowledgements'),
        ('allocationMappingsCreated', 'allocationUnmapAcknowledgements'), ('allocationSlotMappingsCreated', 'allocationSlotUnmapAcknowledgements')):
        require(type(vm.get(created)) is int and type(vm.get(acknowledged)) is int and 0 < vm[created] == vm[acknowledged],
            'Mapping not retired before VM exit')
    for key, marker in (('disconnectCleanup', 'driverCleanupVerified'), ('ownedQemuControl', 'ownedQemuExited')):
        selected = [row for row in records if marker in row]
        require(len(selected) == 1 and json.dumps(report[key], sort_keys=True) == json.dumps(selected[0], sort_keys=True),
            'Report/raw cleanup mismatch')
    for key, value in (('vendorCpuSlotLimit', 256), ('vendorCpuMappedByteLimit', 100663296),
        ('reportedGpuUsageLimitBytes', 268435456), ('vendorCpuSlotQuotaRejections', 0), ('completedNativeSubmissions', 5),
        ('completedHwQueueSignals', 6 if profile == 'pass' else 2), ('completedDeviceExecutionQueries', 3 if profile == 'pass' else 1)):
        require(type(cleanup.get(key)) is int and cleanup[key] == value, 'FD guard/initialization differs: ' + key)
    for used, limit in (('peakVendorCpuBytes', 'vendorCpuMappedByteLimit'), ('peakReportedGpuUsageBytes', 'reportedGpuUsageLimitBytes')):
        require(type(cleanup.get(used)) is int and 0 < cleanup[used] <= cleanup[limit], 'Wrong/unused resource guard')
    require(report.get('diagnosticCpuStoresRequested') is False and cleanup.get('cpuStoreTestRequested') is False and
        type(cleanup.get('completedCpuStoreTests')) is int and cleanup['completedCpuStoreTests'] == 0, 'CPU-store diagnostic used')
    for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired'), ('nativeHwQueueSignalReturns', 'nativeHwQueueSignalReturned'),
        ('nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalAccepted'), ('nativeHwQueueSignalRetirements', 'nativeHwQueueSignalRetired')):
        require(json.dumps(report[key], sort_keys=True) == json.dumps([row for row in records if marker in row], sort_keys=True),
            'Report/raw initialization mismatch: ' + key)
    require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
        report['nativeAsyncSubmissionRetirements'], cleanup), 'Unretired initialization submissions')
    require(hw_queue_signals_complete(report['guestHwQueueSignals'], report['nativeHwQueueSignalReturns'],
        report['nativeHwQueueSignalAcceptances'], report['nativeHwQueueSignalRetirements'], cleanup), 'Unretired hardware queue signals')
    probes = [row for row in records if 'nativeSharedBufferImportProbe' in row]
    if profile == 'buffer-refusal':
        require(not probes and cleanup.get('failedSharedResourceProbes') == 1 and cleanup.get('exportedResourceHandles') == 0 and
            cleanup.get('closedResourceHandles') == 0, 'Buffer refusal erased')
        expected = dict(nativeSharedResourceImportProbe=True, verified=False, ntstatus=0, deviceHresult=0, importHresult=0,
            runtimeBytes=264, heapHresult=-2147467262, heapBytes=0, width=65536, height=1, format=0, dimension=1, flags=0,
            sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)
        require(json.dumps(report['nativeSharedResourceImportChecks'], sort_keys=True) == json.dumps([expected], sort_keys=True) and
            json.dumps([row for row in records if 'nativeSharedResourceImportProbe' in row], sort_keys=True) == json.dumps([expected], sort_keys=True),
            'Wrong retained texture-gate refusal')
    else:
        expected = dict(nativeSharedBufferImportProbe=True, verified=True, ntstatus=0, deviceHresult=0, importHresult=0,
            runtimeBytes=264, dimension=1, alignment=65536, width=65536, height=1, layers=1, levels=1, format=0,
            samples=1, quality=0, layout=1, flags=0, sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)
        require(json.dumps(probes, sort_keys=True) == json.dumps([expected], sort_keys=True), 'Missing actual bounded Windows buffer import')
        for key in ('sharedBufferProbes', 'importedSharedBuffers', 'exportedResourceHandles', 'closedResourceHandles'):
            require(type(cleanup.get(key)) is int and cleanup[key] == 1, 'Missing native handle lifetime: ' + key)
        require(type(cleanup.get('failedSharedBufferProbes')) is int and cleanup['failedSharedBufferProbes'] == 0 and
            cleanup['importedResourceFdObjects'] == 0, 'Failed Windows probe or unexpected guest import')
    return dict(allNativeMappingsRetiredBeforeVmExit=True, naturalHostAndVmExit=True,
        nativeInitializationSubmissions=5, applicationShaderSubmissions=0,
        windowsBufferProbeVerified=profile != 'buffer-refusal', guestVulkanFdImportVerified=False,
        peakCpuMappedBytes=cleanup['peakVendorCpuBytes'], peakReportedGpuUsageBytes=cleanup['peakReportedGpuUsageBytes'])
