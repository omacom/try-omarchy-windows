"""Audit actual preflight refusal, same-buffer retry, GPU data and live retirement."""
import json
import re
from async_cpu_wait_evidence import fields, one, exact
from verify_4k_capture import require, LIVE_COUNTERS
from test_owned_runtime_qemu import async_submissions_complete, hw_queue_signals_complete
from vulkan_memory_roundtrip_evidence import INITIAL, ROUND
from vulkan_memory_roundtrip_reference import expected_round

SMALL, LARGE, LIMIT = 65536, 67108864, 268435456
FILL = tuple('pressure-' + s for s in ('reset-command', 'reset-fence', 'begin-command', 'end-command', 'submit', 'wait'))
PRESSURE_STAGES = (('pressure-buffer', 'pressure-bind') + FILL) * 2 + (
    'pressure-buffer', 'pressure-device-idle', 'pressure-retry-memory', 'pressure-retry-bind') + FILL


def same(actual, expected, reason):
    require(json.dumps(actual, sort_keys=True) == json.dumps(expected, sort_keys=True), reason)


def validate_pressure_receipts(log):
    exact(one(log, 'EVENTFD_CONTRACT_PASS'), dict(checks=31, published=0, notified=0, gpu='false',
        partialRollback='true', foreignOwnerRejected='true'))
    exact(one(log, 'VULKAN_GPU_MEMORY_PRESSURE_CAPACITY'), dict(version=2, reservations=16,
        cpuByteGuardUnchanged='true', gpuByteGuardUnchanged='true', renderApi='Vulkan'))
    exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_BEGIN'), dict(bufferBytes=SMALL, stripes=16, rounds=2,
        cpuWrites='false', window='false'))
    exact(one(log, 'VULKAN_GPU_MEMORY_PRESSURE_BEGIN'), dict(allocationBytes=LARGE, maxAttempts=6,
        cpuWrites='false', hostGuardChanged='false'))
    held = [fields(s, 'VULKAN_GPU_MEMORY_PRESSURE_HELD') for s in log.splitlines()
        if s.startswith('VULKAN_GPU_MEMORY_PRESSURE_HELD ')]
    same(held, [dict(index=str(i), allocationBytes=str(LARGE), gpuFillRetired='true') for i in range(2)],
        'Pressure did not retain two actual GPU-filled buffers')
    exact(one(log, 'VULKAN_GPU_MEMORY_PRESSURE_REFUSED'), dict(heldBuffers=2, result=-2,
        allocationBytes=LARGE, outputNull='true', callerFdRetained='true'))
    exact(one(log, 'VULKAN_GPU_MEMORY_PRESSURE_RECOVERED'), dict(freedBuffers=2, retrySameBuffer='true',
        retrySameSize='true', retrySameMemoryType='true', retryGpuFillRetired='true', callerFdRetained='true'))
    stages = INITIAL + ROUND[:11] + PRESSURE_STAGES + ROUND[11:] + ROUND + ('device-idle',)
    results = [fields(s, 'VULKAN_BRIDGE_RESULT') for s in log.splitlines() if s.startswith('VULKAN_BRIDGE_RESULT ')]
    same(results, [dict(stage=s, result='0') for s in stages], 'Failed, missing or reordered pressure API call')
    require(len(re.findall(r'^VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 \(NVIDIA GeForce RTX 5090 Laptop GPU\)$', log, re.M)) == 1,
        'Actual NVIDIA device missing')
    freed = [fields(s, 'VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED') for s in log.splitlines()
        if s.startswith('VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED ')]
    same(freed, [dict(round=str(i), beforeImport='true', gpuFillRetired='true') for i in range(2)], 'Creator lifetime differs')
    exports = [m.end() for m in re.finditer(r'^VULKAN_BRIDGE_RESULT stage=roundtrip-export result=0$', log, re.M)]
    imports = [m.start() for m in re.finditer(r'^VULKAN_BRIDGE_RESULT stage=roundtrip-import-buffer result=0$', log, re.M)]
    require(len(exports) == len(imports) == 2, 'Wrong import/export count')
    for i in range(2):
        require(exports[i] < log.index('VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED round=' + str(i)) < imports[i], 'Creator freed outside export/import')
    require(exports[0] < log.index('VULKAN_GPU_MEMORY_PRESSURE_BEGIN ') < log.index('VULKAN_GPU_MEMORY_PRESSURE_REFUSED ') <
        log.index('stage=pressure-device-idle result=0') < log.index('stage=pressure-retry-memory result=0') <
        log.index('VULKAN_GPU_MEMORY_PRESSURE_RECOVERED ') < imports[0], 'Recovery is outside original FD lifetime')
    rounds = [fields(s, 'VULKAN_MEMORY_ROUNDTRIP_ROUND') for s in log.splitlines() if s.startswith('VULKAN_MEMORY_ROUNDTRIP_ROUND ')]
    require(len(rounds) == 2, 'Missing complete post-pressure GPU data')
    for i, row in enumerate(rounds):
        reference = expected_round(i)['fnv1a']
        exact(row, dict(round=i, verifiedWords=16384, fnv1a=reference, referenceFnv1a=reference,
            gpuOnlyStorage='true', creatorFreed='true', importOwnsFd='true', gpuTransferReadback='true', cpuWrites='false'))
    exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_COMPLETE'), dict(verified='true', rounds=2, wordsPerRound=16384,
        gpuOnlyStorage='true', creatorFreedBeforeImport='true', gpuTransferReadback='true', nvidiaUmdLoaded='true', window='false', cpuWrites='false'))
    exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_DEVICE_READY'), dict(verified='true', vendor=4318, computeQueue='true',
        nvidiaUmdLoaded='true', submissions=4, window='false'))
    exact(one(log, 'KERNEL_EVENTFD_EXIT'), dict(held=0, pending=0, created=10, notified=10, errors=0, released=10, cleanup='true'))
    exact(one(log, 'BROKER_OBJECT_SUMMARY'), dict(exported=5, published=5, resolved=12, reaped=5, closed=5, retained=0))
    require(log.count('asyncCpuFenceWaitRegistered=true') == 10 and
        not any(s in log for s in ('VULKAN_BRIDGE_FAIL', 'VULKAN_MEMORY_ROUNDTRIP_MISMATCH', 'Removing Device', 'RemovingDevice', 'EVENTFD_CONTRACT_FAIL', 'APERTURE_CONTRACT_FAIL')) and
        re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.M) == ['0'], 'Device loss, failed data or wrong exit')
    return dict(recoverableVulkanOutOfDeviceMemory=True, heldGpuBuffers=2, bufferBytes=LARGE,
        sameBufferRetryGpuFillRetired=True, payloadRoundsVerified=2, wordsVerifiedPerRound=16384,
        completeLargeBufferContentsVerified=False, hardQuotaImplemented=False, concurrentAdmissionVerified=False,
        windowPresentationVerified=False, dailyUseAccepted=False)


def validate_admission_queries(records, cleanup, pressure=False, rounds=2):
    require(type(pressure) is bool and type(rounds) is int and rounds in (1, 2), 'Unknown admission scope')
    queries = [r for r in records if 'nativeGpuBufferAdmissionQueried' in r]
    widths = [SMALL, LARGE, LARGE, LARGE, LARGE, SMALL] if pressure else [SMALL] * rounds
    require(len(queries) == len(widths), 'Admission query missing or duplicated')
    for i, (r, width) in enumerate(zip(queries, widths)):
        refused = pressure and i == 3
        usage = r.get('usageBytes')
        require(type(usage) is int and 0 < usage <= LIMIT, 'Missing reported usage')
        same(r, dict(nativeGpuBufferAdmissionQueried=True, bytes=width, flags=0, usageKnown=True,
            usageBytes=usage, limitBytes=LIMIT, accepted=not refused, ntstatus=-1073741801 if refused else 0,
            advisoryOnly=True, capacityReserved=False, gpuObjectCreated=False), 'Admission status, scope or authority differs')
        require((usage + width > LIMIT) is refused, 'Admission outcome contradicts unchanged guard')
    wanted = dict(nativeGpuBufferAdmissionQueries=len(widths), nativeGpuBufferAdmissionRefusals=1 if pressure else 0,
        failedNativeGpuBufferAdmissionQueries=0, nativeGpuBufferMetadataMemoryRefusals=0)
    require(all(type(cleanup.get(k)) is int and cleanup[k] == v for k, v in wanted.items()), 'Admission cleanup counters differ')
    require(not any('nativeGpuBufferMetadataMemoryRefused' in r or 'nativeGpuBufferAdmissionRejected' in r for r in records),
        'Late NVIDIA allocation refusal instead of frontend recovery')
    if pressure:
        rejected = [r for r in records if 'nativeReportedGpuUsageRejected' in r]
        require(len(rejected) == 1, 'Known pressure boundary missing')
        r = rejected[0]
        usage = queries[3]['usageBytes']
        require(r.get('usageBytes') == usage and r.get('additionalBytes') == LARGE and r.get('limitBytes') == LIMIT and
            r.get('currentProcessOnly') is True and r.get('policyChanged') is False and r.get('hardAllocationQuota') is False and
            type(r.get('localUsageBytes')) is int and type(r.get('nonlocalUsageBytes')) is int and
            r['localUsageBytes'] + r['nonlocalUsageBytes'] == usage, 'Refusal is not the observed process budget')
    else:
        require(not any('nativeReportedGpuUsageRejected' in r for r in records), 'Unexpected pressure in normal profile')
    return dict(queriesVerified=len(widths), knownPreflightRefusals=1 if pressure else 0, advisoryOnly=True,
        capacityReserved=False, concurrentAdmissionVerified=False)


def validate_pressure_metadata(records, cleanup):
    widths = [SMALL, LARGE, LARGE, LARGE, SMALL]
    generated = [r for r in records if 'nativeGpuBufferMetadataGenerated' in r]
    applied = [r for r in records if 'nativeGpuBufferMetadataApplied' in r]
    probes = [r for r in records if 'nativeSharedBufferImportProbe' in r]
    same(generated, [dict(nativeGpuBufferMetadataGenerated=True, bytes=w, flags=0, allocationBytes=w,
        allocationAlignment=65536, runtimeBytes=280, adapterNtstatus=0, deviceNtstatus=0, queryNtstatus=0,
        openNtstatus=0, resourceDestroyedNtstatus=0, deviceDestroyedNtstatus=0, adapterClosedNtstatus=0,
        sharedHandleClosed=True, prototypeReleased=True, reportedUsageChecked=True, privateBytesExported=False) for w in widths],
        'Template cleanup, widths or Windows metadata differs')
    same(applied, [dict(nativeGpuBufferMetadataApplied=True, bytes=w, flags=0, guestRuntimeBytes=264,
        nativeRuntimeBytes=280, guestRuntimePreserved=True, driverPrivateDataReused=True, ntstatus=0) for w in widths],
        'Guest and Windows metadata ownership differs')
    same(probes, [dict(nativeSharedBufferImportProbe=True, verified=True, ntstatus=0, deviceHresult=0,
        importHresult=0, runtimeBytes=280, dimension=1, alignment=65536, width=w, height=1, layers=1, levels=1,
        format=0, samples=1, quality=0, layout=1, flags=0, sharedNtHandleClosed=True, importedResourceReleased=True,
        gpuCopy=False, presented=False) for w in widths], 'Native metadata did not describe actual buffers')
    # A refused preflight must not generate/apply metadata or create a buffer.
    events = []
    for r in records:
        for marker, kind, key in (('nativeGpuBufferAdmissionQueried', 'query', 'bytes'),
            ('nativeGpuBufferMetadataGenerated', 'generated', 'bytes'), ('nativeGpuBufferMetadataApplied', 'applied', 'bytes'),
            ('nativeSharedBufferImportProbe', 'probe', 'width')):
            if marker in r: events.append((kind, r[key], r['accepted'] if kind == 'query' else True))
    expected = []
    for i, w in enumerate([SMALL, LARGE, LARGE, LARGE, LARGE, SMALL]):
        expected.append(('query', w, i != 3))
        if i != 3: expected.extend((kind, w, True) for kind in ('generated', 'applied', 'probe'))
    same(events, expected, 'Refusal published an allocation or metadata lifetime was reordered')
    same([r for r in records if 'nativeResourceInfoQueried' in r], [dict(nativeResourceInfoQueried=True,
        guestRuntimeBytes=264, hostRuntimeBytes=280, driverBytes=586, resourceDriverBytes=0, allocations=1)] * 4,
        'FD query lost independent guest metadata')
    same([r for r in records if 'nativeResourceFdImported' in r], [dict(nativeResourceFdImported=True,
        allocations=1, guestRuntimeBytes=264, driverBytes=586, rawHandleExposed=False)] * 2, 'FD import ownership differs')
    same([r for r in records if 'nativeResourceFdOpenReturned' in r], [dict(nativeResourceFdOpenReturned=True,
        ntstatus=0, hasResource=True, hasAllocation=True, hasKeyedMutex=False, hasSync=False, runtimeBytes=280,
        driverBytes=586, allocationDriverBytes=586, resourceDriverBytes=0)] * 2, 'Native FD open failed')
    maps = [r for r in records if 'nativeImportedResourceGpuVaMapped' in r]
    require(len(maps) == 2, 'Missing imported GPU mappings')
    fence = 0
    for r in maps:
        require(type(r.get('pagingFence')) is int and r['pagingFence'] > fence, 'Paging fence not advanced')
        fence = r['pagingFence']
        same(r, dict(nativeImportedResourceGpuVaMapped=True, driverProtection=268435465, resourceOpened=True,
            bytes=SMALL, pagingFence=fence, ntstatus=259, unchanged=True, addressRangeValidated=True), 'Import mapping lacks provenance')
    for k, v in dict(sharedBufferProbes=5, importedSharedBuffers=5, failedSharedBufferProbes=0,
        exportedResourceHandles=5, closedResourceHandles=5, importedResourceFdObjects=2,
        completedNativeSubmissions=13, completedHwQueueSignals=20, completedDeviceExecutionQueries=10).items():
        require(type(cleanup.get(k)) is int and cleanup[k] == v, 'Wrong GPU pressure retirement counter: ' + k)
    require(0 < cleanup['completedVendorGpuVaMaps'] == cleanup['completedVendorGpuVaWaits'], 'GPU mapping did not retire')
    return dict(windowsMetadataBytes=280, independentGuestMetadataBytes=264,
        verifiedBufferMetadataWidths=[SMALL, LARGE], unpublishedRefusalVerified=True, liveCleanupRequired=True)


def validate_pressure_cleanup(report, records):
    """The pressure workload has exactly 20 total signals, at most 16 pending.

    The historical compute validator's default 16-total-signal scope remains
    unchanged. Correlate every pressure signal using its existing bounded API.
    """
    cleanup, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(type(report.get('hostBridgeExit')) is int and report['hostBridgeExit'] == 0 and
        cleanup.get('driverCleanupVerified') is True and type(cleanup.get('cleanupFailures')) is int and
        cleanup['cleanupFailures'] == 0, 'Native pressure cleanup failed')
    for key in LIVE_COUNTERS:
        require(type(cleanup.get('live' + key)) is int and cleanup['live' + key] == 0, 'Missing/live pressure object: ' + key)
    for section in (cleanup, vm):
        for key, value in section.items():
            if key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith(('Timeouts', 'ReleasedAfterVmExit')):
                require(type(value) is int and value == 0, 'Failed or deferred pressure work: ' + key)
    require(vm.get('ownedQemuExited') is True and type(vm.get('qemuExit')) is int and vm['qemuExit'] == 0 and
        vm.get('qemuForcedStop') is False and vm.get('fenceControlFailed') is False and
        type(vm.get('allocationApertureSlots')) is int and vm['allocationApertureSlots'] == 256 and
        type(vm.get('fenceApertureSlots')) is int and vm['fenceApertureSlots'] == 128, 'Pressure VM lifetime/geometry differs')
    for created, acknowledged in (('fenceMappingsCreated', 'fenceUnmapAcknowledgements'),
        ('allocationMappingsCreated', 'allocationUnmapAcknowledgements'), ('allocationSlotMappingsCreated', 'allocationSlotUnmapAcknowledgements')):
        require(type(vm.get(created)) is int and type(vm.get(acknowledged)) is int and 0 < vm[created] == vm[acknowledged],
            'Unretired pressure aperture mapping')
    for key, marker in (('disconnectCleanup', 'driverCleanupVerified'), ('ownedQemuControl', 'ownedQemuExited')):
        same(report[key], next_record(records, marker), 'Raw pressure cleanup/report mismatch')
    for key, expected in dict(vendorCpuSlotLimit=256, vendorCpuMappedByteLimit=100663296,
        reportedGpuUsageLimitBytes=LIMIT, vendorGpuMappedByteLimit=LIMIT, standaloneGpuMappedByteLimit=LIMIT,
        vendorCpuSlotQuotaRejections=0, gpuReservationCountLimit=16, gpuReservationByteLimit=17179869184,
        completedNativeSubmissions=13, completedHwQueueSignals=20, completedDeviceExecutionQueries=10).items():
        require(type(cleanup.get(key)) is int and cleanup[key] == expected, 'Pressure guard/work count differs: ' + key)
    for used, limit in (('peakVendorCpuBytes', 'vendorCpuMappedByteLimit'), ('peakReportedGpuUsageBytes', 'reportedGpuUsageLimitBytes')):
        require(type(cleanup.get(used)) is int and 0 < cleanup[used] <= cleanup[limit], 'Unused/exceeded pressure guard')
    require(report.get('diagnosticCpuStoresRequested') is False and cleanup.get('cpuStoreTestRequested') is False and
        type(cleanup.get('completedCpuStoreTests')) is int and cleanup['completedCpuStoreTests'] == 0, 'CPU-store pressure diagnostic requested')
    for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired'), ('nativeHwQueueSignalReturns', 'nativeHwQueueSignalReturned'),
        ('nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalAccepted'), ('nativeHwQueueSignalRetirements', 'nativeHwQueueSignalRetired')):
        same(report[key], [r for r in records if marker in r], 'Pressure GPU report/raw mismatch: ' + key)
    require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
        report['nativeAsyncSubmissionRetirements'], cleanup), 'Unretired pressure command')
    require(len(report['guestHwQueueSignals']) == 20 and hw_queue_signals_complete(report['guestHwQueueSignals'],
        report['nativeHwQueueSignalReturns'], report['nativeHwQueueSignalAcceptances'], report['nativeHwQueueSignalRetirements'],
        cleanup, maximum=20, pending_limit=16), 'Unretired pressure signal')
    require(not any('nativeCpuSlotAdmissionRejected' in r for r in records), 'Unexpected pressure CPU slot refusal')
    return dict(allNativeMappingsRetiredBeforeVmExit=True, allNativeObjectsRetiredBeforeVmExit=True,
        naturalHostAndVmExit=True, nativeSubmissions=13, correlatedHardwareQueueSignals=20, deviceHealthQueries=10,
        cpuMappedByteLimit=100663296, reportedGpuUsageByteLimit=LIMIT, gpuMappedByteLimit=LIMIT,
        peakCpuMappedBytes=cleanup['peakVendorCpuBytes'], peakReportedGpuUsageBytes=cleanup['peakReportedGpuUsageBytes'])


def next_record(records, marker):
    selected = [r for r in records if marker in r]
    require(len(selected) == 1, 'Missing or duplicate raw pressure record: ' + marker)
    return selected[0]
