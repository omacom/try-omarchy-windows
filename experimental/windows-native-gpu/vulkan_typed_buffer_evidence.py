"""Separate typed Windows metadata and FD edge controls from GPU payload proof."""
import json
import re
from async_cpu_wait_evidence import fields, one, exact
from verify_4k_capture import require
from vulkan_memory_roundtrip_evidence import INITIAL, ROUND, validate_native_imports, validate_native_rounds, validate_control_debt
from vulkan_memory_roundtrip_reference import expected_round


def validate_fd_control(log, control, rounds=2):
    require(control in ('none', 'zero', 'exhaustion') and type(rounds) is int and rounds in (1, 2), 'Unknown FD control scope')
    zero = [fields(line, 'VULKAN_MEMORY_ROUNDTRIP_IMPORT_FD') for line in log.splitlines()
        if line.startswith('VULKAN_MEMORY_ROUNDTRIP_IMPORT_FD ')]
    pressure = [fields(line, 'VULKAN_MEMORY_ROUNDTRIP_FD_PRESSURE') for line in log.splitlines()
        if line.startswith('VULKAN_MEMORY_ROUNDTRIP_FD_PRESSURE ')]
    expected_zero = [dict(round=str(i), descriptorZero='true', movedWhileCallerOwned='true') for i in range(rounds)]
    expected_pressure = [dict(round=str(i), result='-1', callerRetained='true', outputNull='true', limitRestored='true', retry='true') for i in range(rounds)]
    require(zero == (expected_zero if control == 'zero' else []) and
        pressure == (expected_pressure if control == 'exhaustion' else []), 'Wrong/missing FD edge control')
    if control != 'none':
        buffers = [m.end() for m in re.finditer(r'^VULKAN_BRIDGE_RESULT stage=roundtrip-import-buffer result=0$', log, re.M)]
        imports = [m.start() for m in re.finditer(r'^VULKAN_BRIDGE_RESULT stage=roundtrip-import-memory result=0$', log, re.M)]
        prefix = 'VULKAN_MEMORY_ROUNDTRIP_IMPORT_FD' if control == 'zero' else 'VULKAN_MEMORY_ROUNDTRIP_FD_PRESSURE'
        markers = [m.start() for m in re.finditer(r'^' + prefix + r' ', log, re.M)]
        require(len(buffers) == len(imports) == len(markers) == rounds and
            all(buffers[i] < markers[i] < imports[i] for i in range(rounds)), 'FD edge control is outside the actual import attempt')
    return dict(control=control, verifiedRounds=rounds if control != 'none' else 0,
        callerRetainedOnFailedImport=control == 'exhaustion', descriptorZeroImported=control == 'zero')


def validate_typed_metadata(records, cleanup, profile, enabled):
    require(profile in ('pass', 'corrupt-last') and type(enabled) is bool, 'Unknown typed metadata scope')
    count = 2 if profile == 'pass' else 1
    generated = [row for row in records if 'nativeGpuBufferMetadataGenerated' in row]
    applied = [row for row in records if 'nativeGpuBufferMetadataApplied' in row]
    require(not any('nativeGpuBufferAdmissionRejected' in row for row in records), 'Typed GPU buffer admission failed')
    template = dict(nativeGpuBufferMetadataGenerated=True, bytes=65536, flags=0, allocationBytes=65536,
        allocationAlignment=65536, runtimeBytes=280, adapterNtstatus=0, deviceNtstatus=0,
        queryNtstatus=0, openNtstatus=0, resourceDestroyedNtstatus=0, deviceDestroyedNtstatus=0,
        adapterClosedNtstatus=0, sharedHandleClosed=True, prototypeReleased=True,
        reportedUsageChecked=True, privateBytesExported=False)
    application = dict(nativeGpuBufferMetadataApplied=True, bytes=65536, flags=0,
        guestRuntimeBytes=264, nativeRuntimeBytes=280, guestRuntimePreserved=True,
        driverPrivateDataReused=True, ntstatus=0)
    same = lambda a, b: json.dumps(a, sort_keys=True) == json.dumps(b, sort_keys=True)
    require(same(generated, [template] * (count if enabled else 0)) and
        same(applied, [application] * (count if enabled else 0)), 'Native metadata provenance, bounds or temporary ownership differs')
    validate_native_imports(records, cleanup, profile, probe_runtime_bytes=280 if enabled else 264)
    if enabled:
        queries = [row for row in records if 'nativeResourceInfoQueried' in row]
        imported = [row for row in records if 'nativeResourceFdImported' in row]
        require(same(queries, [dict(nativeResourceInfoQueried=True, guestRuntimeBytes=264, hostRuntimeBytes=280,
            driverBytes=586, resourceDriverBytes=0, allocations=1)] * (count * 2)) and
            same(imported, [dict(nativeResourceFdImported=True, allocations=1, guestRuntimeBytes=264,
                driverBytes=586, rawHandleExposed=False)] * count), 'Guest/native runtime separation was lost during import')
        # Each actual allocation must follow its own completed template cleanup.
        indices = [i for i, row in enumerate(records) if 'nativeGpuBufferMetadataGenerated' in row]
        applications = [i for i, row in enumerate(records) if 'nativeGpuBufferMetadataApplied' in row]
        probes = [i for i, row in enumerate(records) if 'nativeSharedBufferImportProbe' in row]
        require(all(indices[i] < applications[i] < probes[i] and
            (i == count - 1 or probes[i] < indices[i + 1]) for i in range(count)), 'Native metadata allocation sequence differs')
    return dict(typedWindowsMetadataVerified=enabled, windowsRuntimeBytes=280 if enabled else None,
        guestRuntimeBytes=264, originalGuestRuntimePreserved=True, temporaryWindowsOwnershipRetired=enabled,
        allocationsVerified=count, descriptorCompatibleBufferBytes=65536,
        broaderBufferCompatibilityVerified=False, performanceMeasured=False, dailyUseAccepted=False)


def validate_partial_creator_refusal(log, records, report, control='none'):
    """One real payload round followed by creator failure is never a full pass."""
    expected = [dict(stage=stage, result='0') for stage in INITIAL + ROUND + ROUND[:2]]
    expected[-1]['result'] = '-1000072003'
    actual = [fields(line, 'VULKAN_BRIDGE_RESULT') for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_RESULT ')]
    require(actual == expected, 'Partial creator failure has wrong API boundary')
    reference = expected_round(0)['fnv1a']
    exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_ROUND'), dict(round=0, verifiedWords=16384,
        fnv1a=reference, referenceFnv1a=reference, gpuOnlyStorage='true', creatorFreed='true',
        importOwnsFd='true', gpuTransferReadback='true', cpuWrites='false'))
    exact(one(log, 'VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED'), dict(round=0, beforeImport='true', gpuFillRetired='true'))
    require(log.index('stage=roundtrip-export result=0') < log.index('VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED ') <
        log.index('stage=roundtrip-import-buffer result=0'), 'Partial round lifetime order differs')
    require([line for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_FAIL ')] ==
        ['VULKAN_BRIDGE_FAIL roundtrip-creator-memory'] and not any(marker in log for marker in
            ('VULKAN_MEMORY_ROUNDTRIP_COMPLETE', 'VULKAN_MEMORY_ROUNDTRIP_DEVICE_READY', 'VULKAN_MEMORY_ROUNDTRIP_MISMATCH')),
        'Partial failure promoted to full acceptance')
    exact(one(log, 'KERNEL_EVENTFD_EXIT'), dict(held=0, pending=0, created=3, notified=3, errors=0, released=3, cleanup='true'))
    exact(one(log, 'BROKER_OBJECT_SUMMARY'), dict(exported=1, published=1, resolved=6, reaped=1, closed=1, retained=0))
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.M) == ['1'] and
        log.count('asyncCpuFenceWaitRegistered=true') == 3, 'Partial failure exit/wait count differs')
    validate_fd_control(log, control, 1)
    cleanup = report['disconnectCleanup']
    validate_native_rounds(records, cleanup, 1, early_refusal=True)
    validate_control_debt(cleanup, report['ownedQemuControl'], 'early-refusal')
    for key, value in zip(('completedNativeSubmissions', 'completedHwQueueSignals', 'completedDeviceExecutionQueries'), (8, 6, 3)):
        require(type(cleanup.get(key)) is int and cleanup[key] == value, 'Partial native work count differs')
    require(cleanup['driverCleanupVerified'] is True and report['hostBridgeExit'] == 0 and
        report['ownedQemuControl']['qemuExit'] == 0 and report['ownedQemuControl']['qemuForcedStop'] is False,
        'Partial control cleanup/VM exit differs')
    return dict(payloadRoundsVerified=1, verifiedWords=16384, fnv1a=reference, laterCreatorFailureRetained=True,
        wholeRunAccepted=False, dailyUseAccepted=False)
