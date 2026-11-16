"""Audit GPU buffer data separately from early import failures and recovery."""
import json
import re
from async_cpu_wait_evidence import fields, one, exact
from verify_4k_capture import require, LIVE_COUNTERS
from vulkan_compute_evidence import validate_compute_cleanup
from vulkan_memory_fd_evidence import REQUIRED_NATIVE_COUNTERS, REQUIRED_VM_COUNTERS
from vulkan_memory_roundtrip_reference import expected_round

INITIAL = ('create-instance', 'count-devices', 'enumerate-devices', 'create-device')
ROUND = ('roundtrip-creator-buffer', 'roundtrip-creator-memory', 'roundtrip-creator-bind',
    'roundtrip-command-pool', 'roundtrip-command-buffer', 'roundtrip-fence', 'roundtrip-begin-command',
    'roundtrip-end-command', 'roundtrip-submit', 'roundtrip-wait', 'roundtrip-export',
    'roundtrip-import-buffer', 'roundtrip-import-memory', 'roundtrip-import-bind',
    'roundtrip-readback-buffer', 'roundtrip-readback-memory', 'roundtrip-readback-bind',
    'roundtrip-reset-command', 'roundtrip-reset-fence', 'roundtrip-begin-command', 'roundtrip-end-command',
    'roundtrip-submit', 'roundtrip-wait', 'roundtrip-map-readback')
PROFILES = ('pass', 'map-refusal', 'early-refusal', 'corrupt-last')


def validate_receipts(log, profile):
    require(profile in PROFILES, 'Unknown roundtrip profile')
    exact(one(log, 'EVENTFD_CONTRACT_PASS'), dict(checks=31, published=0, notified=0, gpu='false',
        partialRollback='true', foreignOwnerRejected='true'))
    exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_BEGIN'), dict(bufferBytes=65536, stripes=16, rounds=2, cpuWrites='false', window='false'))
    stages = INITIAL + (ROUND * 2 + ('device-idle',) if profile == 'pass' else
        ROUND[:13] if profile == 'map-refusal' else ROUND[:2] if profile == 'early-refusal' else ROUND)
    expected = [dict(stage=stage, result='0') for stage in stages]
    if profile in ('early-refusal', 'map-refusal'): expected[-1]['result'] = '-1000072003'
    actual = [fields(line, 'VULKAN_BRIDGE_RESULT') for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_RESULT ')]
    require(actual == expected, 'Missing, failed or reordered roundtrip API calls')
    require(len(re.findall(r'^VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 \(NVIDIA GeForce RTX 5090 Laptop GPU\)$', log, re.M)) == 1,
        'Actual NVIDIA device missing')
    count = 2 if profile == 'pass' else 0 if profile == 'early-refusal' else 1
    freed = [fields(line, 'VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED') for line in log.splitlines()
        if line.startswith('VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED ')]
    require(freed == [dict(round=str(i), beforeImport='true', gpuFillRetired='true') for i in range(count)], 'Creator lifetime proof missing')
    # The exact marker must occur between each successful export and its import.
    exports = [m.end() for m in re.finditer(r'^VULKAN_BRIDGE_RESULT stage=roundtrip-export result=0$', log, re.M)]
    imports = [m.start() for m in re.finditer(r'^VULKAN_BRIDGE_RESULT stage=roundtrip-import-buffer result=0$', log, re.M)]
    require(len(exports) == len(imports) == count, 'Export/import lifetime count mismatch')
    for i in range(count):
        marker = f'VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED round={i} beforeImport=true gpuFillRetired=true'
        require(exports[i] < log.index(marker) < imports[i], 'Creator was not freed before import')
    rounds = [fields(line, 'VULKAN_MEMORY_ROUNDTRIP_ROUND') for line in log.splitlines() if line.startswith('VULKAN_MEMORY_ROUNDTRIP_ROUND ')]
    failures = [line for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_FAIL ')]
    mismatches = [line for line in log.splitlines() if line.startswith('VULKAN_MEMORY_ROUNDTRIP_MISMATCH ')]
    if profile == 'pass':
        require(not failures and not mismatches and len(rounds) == 2, 'Incomplete/failed GPU payload rounds')
        for i, row in enumerate(rounds):
            reference = expected_round(i)
            exact(row, dict(round=i, verifiedWords=16384, fnv1a=reference['fnv1a'], referenceFnv1a=reference['fnv1a'],
                gpuOnlyStorage='true', creatorFreed='true', importOwnsFd='true', gpuTransferReadback='true', cpuWrites='false'))
        exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_COMPLETE'), dict(verified='true', rounds=2, wordsPerRound=16384,
            gpuOnlyStorage='true', creatorFreedBeforeImport='true', gpuTransferReadback='true', nvidiaUmdLoaded='true', window='false', cpuWrites='false'))
        exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_DEVICE_READY'), dict(verified='true', vendor=4318, computeQueue='true',
            nvidiaUmdLoaded='true', submissions=4, window='false'))
    else:
        require(not rounds and 'VULKAN_MEMORY_ROUNDTRIP_COMPLETE' not in log and 'VULKAN_MEMORY_ROUNDTRIP_DEVICE_READY' not in log,
            'Failed control promoted to payload acceptance')
        failure = {'map-refusal': 'roundtrip-import-memory', 'early-refusal': 'roundtrip-creator-memory',
            'corrupt-last': 'imported GPU payload differs from integer oracle'}[profile]
        require(failures == ['VULKAN_BRIDGE_FAIL ' + failure], 'Wrong failure boundary')
        require(mismatches == (['VULKAN_MEMORY_ROUNDTRIP_MISMATCH round=0 word=16383 actual=0 expected=2353232594']
            if profile == 'corrupt-last' else []), 'Wrong GPU oracle rejection')
    notifications = 6 if profile == 'pass' else 3 if profile == 'corrupt-last' else 1
    exact(one(log, 'KERNEL_EVENTFD_EXIT'), dict(held=0, pending=0, created=notifications, notified=notifications,
        errors=0, released=notifications, cleanup='true'))
    exact(one(log, 'BROKER_OBJECT_SUMMARY'), dict(exported=count, published=count, resolved=count * 6, reaped=count, closed=count, retained=0))
    require(log.count('asyncCpuFenceWaitRegistered=true') == notifications and
        not any(marker in log for marker in ('EVENTFD_CONTRACT_FAIL', 'APERTURE_CONTRACT_FAIL')), 'Kernel contract/count failure')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.M) == ['0' if profile == 'pass' else '1'], 'Wrong guest exit')
    return dict(profile=profile, payloadRoundsVerified=2 if profile == 'pass' else 0,
        wordsVerifiedPerRound=16384 if profile == 'pass' else 0, kernelNotifications=notifications,
        creatorFreedBeforeImport=count > 0, gpuLastWordCorruptionRejected=profile == 'corrupt-last',
        descriptorClosureDuringLiveApplicationVerified=False, windowPresentationVerified=False, dailyUseAccepted=False)


def validate_native_imports(records, cleanup, profile, *, probe_runtime_bytes=264):
    require(profile in PROFILES, 'Unknown native import profile')
    count = 2 if profile == 'pass' else 0 if profile == 'early-refusal' else 1
    return validate_native_rounds(records, cleanup, count, probe_runtime_bytes=probe_runtime_bytes,
        early_refusal=profile == 'early-refusal', refused_mapping=profile == 'map-refusal')


def validate_native_rounds(records, cleanup, count, *, probe_runtime_bytes=264, early_refusal=False, refused_mapping=False):
    require(type(count) is int and count in (0, 1, 2) and type(early_refusal) is type(refused_mapping) is bool, 'Unknown native round scope')
    require(type(probe_runtime_bytes) is int and probe_runtime_bytes in (264, 280), 'Unknown explicit native metadata profile')
    probes = [row for row in records if 'nativeSharedBufferImportProbe' in row]
    expected_probe = dict(nativeSharedBufferImportProbe=True, verified=True, ntstatus=0, deviceHresult=0, importHresult=0,
        runtimeBytes=probe_runtime_bytes, dimension=1, alignment=65536, width=65536, height=1, layers=1, levels=1, format=0,
        samples=1, quality=0, layout=1, flags=0, sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)
    require(json.dumps(probes, sort_keys=True) == json.dumps([expected_probe] * count, sort_keys=True), 'Wrong native buffer probe')
    opens = [row for row in records if 'nativeResourceFdOpenReturned' in row]
    expected_open = dict(nativeResourceFdOpenReturned=True, ntstatus=0, hasResource=True, hasAllocation=True,
        hasKeyedMutex=False, hasSync=False, runtimeBytes=probe_runtime_bytes, driverBytes=586, allocationDriverBytes=586, resourceDriverBytes=0)
    require(json.dumps(opens, sort_keys=True) == json.dumps([expected_open] * count, sort_keys=True), 'Native NT import did not succeed')
    if early_refusal:
        failed = [row for row in records if 'nativeSharedResourceImportProbe' in row]
        expected_failure = dict(nativeSharedResourceImportProbe=True, verified=False, ntstatus=0, deviceHresult=0,
            importHresult=-2147467262, runtimeBytes=264, heapHresult=-2147467262, heapBytes=0, width=0, height=0,
            format=0, dimension=0, flags=0, sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)
        require(json.dumps(failed, sort_keys=True) == json.dumps([expected_failure], sort_keys=True), 'Early E_NOINTERFACE failure erased')
    maps = [row for row in records if 'nativeImportedResourceGpuVaMapped' in row]
    require(len(maps) == (0 if refused_mapping else count), 'Missing/extraneous owned-import GPU maps')
    last_fence = 0
    for row in maps:
        wanted = dict(nativeImportedResourceGpuVaMapped=True, driverProtection=268435465, resourceOpened=True,
            bytes=65536, ntstatus=259, unchanged=True, addressRangeValidated=True)
        require(all(type(row.get(k)) is type(v) and row[k] == v for k, v in wanted.items()) and
            type(row.get('pagingFence')) is int and row['pagingFence'] > last_fence, 'Wrong owned native mapping/protection/fence')
        last_fence = row['pagingFence']
    for key in ('sharedBufferProbes', 'importedSharedBuffers', 'exportedResourceHandles', 'closedResourceHandles', 'importedResourceFdObjects'):
        require(type(cleanup.get(key)) is int and cleanup[key] == count, 'Native import/reference count differs: ' + key)
    require(type(cleanup.get('failedSharedBufferProbes')) is int and cleanup['failedSharedBufferProbes'] == 0 and
        type(cleanup.get('completedVendorGpuVaMaps')) is int and type(cleanup.get('completedVendorGpuVaWaits')) is int and
        0 < cleanup['completedVendorGpuVaMaps'] == cleanup['completedVendorGpuVaWaits'], 'Buffer or paging retirement failed')


def validate_cleanup(report, records, profile):
    require(profile in PROFILES, 'Unknown cleanup profile')
    c, vm = report['disconnectCleanup'], report['ownedQemuControl']
    for section, keys in ((c, REQUIRED_NATIVE_COUNTERS), (vm, REQUIRED_VM_COUNTERS)):
        require(all(type(section.get(key)) is int for key in keys), 'Missing debt counter')
    if profile in ('pass', 'corrupt-last'):
        result = validate_compute_cleanup(report, records, 256, 'pass')  # generic GPU/lifetime checks only
    else:
        require(type(report.get('hostBridgeExit')) is int and report['hostBridgeExit'] == 0 and
            c['driverCleanupVerified'] is True and type(c['cleanupFailures']) is int and c['cleanupFailures'] == 0,
            'Failed control did not eventually clean up')
        validate_control_debt(c, vm, profile)
        for key in LIVE_COUNTERS: require(type(c.get('live' + key)) is int and c['live' + key] == 0, 'Missing/live native object')
        require(vm['ownedQemuExited'] is True and vm['qemuExit'] == 0 and vm['qemuForcedStop'] is vm['fenceControlFailed'] is False and
            vm['allocationApertureSlots'] == 256 and vm['fenceApertureSlots'] == 128, 'Control VM lifetime differs')
        for created, ack in (('fenceMappingsCreated', 'fenceUnmapAcknowledgements'), ('allocationMappingsCreated', 'allocationUnmapAcknowledgements'),
            ('allocationSlotMappingsCreated', 'allocationSlotUnmapAcknowledgements')):
            require(type(vm.get(created)) is int and type(vm.get(ack)) is int and 0 < vm[created] == vm[ack], 'Control mapping debt')
        for key, marker in (('disconnectCleanup', 'driverCleanupVerified'), ('ownedQemuControl', 'ownedQemuExited')):
            require(json.dumps(report[key], sort_keys=True) == json.dumps(one_record(records, marker), sort_keys=True), 'Raw control cleanup differs')
        for key, value in (('vendorCpuSlotLimit', 256), ('vendorCpuMappedByteLimit', 100663296), ('reportedGpuUsageLimitBytes', 268435456)):
            require(type(c.get(key)) is int and c[key] == value, 'Control byte guard changed')
        try: validate_compute_cleanup(report, records, 256, 'pass')
        except ValueError as error:
            require(str(error) == ('Failed or deferred work: gpuReservationsReleasedAfterVmExit' if profile == 'map-refusal' else
                'Failed or deferred work: failedSharedResourceProbes'), 'Unexpected strict control rejection')
        else: raise ValueError('Failed control was accepted')
        result = dict(eventualCleanupVerified=True, allNativeMappingsRetiredBeforeVmExit=True,
            allNativeObjectsRetiredBeforeVmExit=profile != 'map-refusal', fullRunAccepted=False,
            gpuReservationsReleasedAfterVmExit=4 if profile == 'map-refusal' else 0)
    validate_native_imports(records, c, profile)
    expected = {'pass': (10, 12, 6), 'corrupt-last': (8, 6, 3), 'map-refusal': (6, 2, 3), 'early-refusal': (5, 2, 1)}[profile]
    for key, value in zip(('completedNativeSubmissions', 'completedHwQueueSignals', 'completedDeviceExecutionQueries'), expected):
        require(type(c.get(key)) is int and c[key] == value, 'Native workload count differs: ' + key)
    return result


def one_record(records, marker):
    selected = [row for row in records if marker in row]
    require(len(selected) == 1, 'Missing/duplicate raw receipt')
    return selected[0]


def validate_control_debt(cleanup, vm, profile):
    require(profile in ('map-refusal', 'early-refusal'), 'Unknown retained failure')
    for section, keys in ((cleanup, REQUIRED_NATIVE_COUNTERS), (vm, REQUIRED_VM_COUNTERS)):
        require(all(type(section.get(key)) is int for key in keys), 'Missing control debt counter')
        for key, value in section.items():
            if key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith(('Timeouts', 'ReleasedAfterVmExit')):
                wanted = 4 if profile == 'map-refusal' and key == 'gpuReservationsReleasedAfterVmExit' else \
                    1 if profile == 'early-refusal' and key == 'failedSharedResourceProbes' else 0
                require(type(value) is int and value == wanted, 'Retained failure/debt differs: ' + key)
