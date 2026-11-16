"""Keep 4K shader execution, slot refusal and oracle rejection as distinct scopes."""
import json
import re
from async_cpu_wait_evidence import fields, one, exact
from background_shader_evidence import FORBIDDEN
from broker_runtime_evidence import broker_runtime_complete
from verify_4k_capture import require, LIVE_COUNTERS, validate_cleanup
from test_owned_runtime_qemu import async_submissions_complete, hw_queue_signals_complete

EXPECTED_HASHES = ('650630d8645436e5', '3c21b7a6986969e5')
BASE_STAGES = ('create-instance', 'count-devices', 'enumerate-devices', 'create-device',
    'compute-output-buffer', 'compute-output-memory', 'compute-output-bind',
    'compute-readback-buffer', 'compute-readback-memory', 'compute-readback-bind',
    'compute-shader-module', 'compute-descriptor-layout', 'compute-pipeline-layout', 'compute-pipeline',
    'compute-descriptor-pool', 'compute-descriptor-set', 'compute-command-pool', 'compute-command-buffer', 'compute-fence')
ROUND_STAGES = ('compute-reset-fence', 'compute-begin-command', 'compute-end-command',
    'compute-queue-submit', 'compute-wait-fence', 'compute-map-readback')


def validate_compute_receipts(log, profile):
    require(profile in ('pass', 'slot-refusal', 'corrupt-last'), 'Unknown compute profile')
    exact(one(log, 'EVENTFD_CONTRACT_PASS'), dict(checks=31, published=0, notified=0, gpu='false',
        partialRollback='true', foreignOwnerRejected='true'))
    exact(one(log, 'VULKAN_COMPUTE_BEGIN'), dict(width=3840, height=2160, words=8294400,
        rounds=2, cpuStorageWrites='false', window='false'))
    results = [fields(line, 'VULKAN_BRIDGE_RESULT') for line in log.splitlines()
               if line.startswith('VULKAN_BRIDGE_RESULT ')]
    stages = BASE_STAGES + ROUND_STAGES
    if profile == 'pass': stages += ('compute-reset-command',) + ROUND_STAGES + ('device-idle',)
    expected = [dict(stage=stage, result='0') for stage in stages]
    if profile == 'slot-refusal': expected[-1]['result'] = '-5'
    require(results == expected, 'Incomplete, failed or reordered Vulkan API stages')
    memories = [fields(line, 'VULKAN_COMPUTE_MEMORY') for line in log.splitlines()
                if line.startswith('VULKAN_COMPUTE_MEMORY ')]
    require(memories == [dict(role='output', type='3', flags='1', bytes='33177600', cpuInitialized='false'),
                         dict(role='readback', type='1', flags='6', bytes='33177600', cpuInitialized='false')],
            'Wrong GPU-only storage or coherent readback memory')
    device = r'^VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 \(NVIDIA GeForce RTX 5090 Laptop GPU\)$'
    require(len(re.findall(device, log, re.M)) == 1, 'Missing actual NVIDIA discrete device')
    rounds = [fields(line, 'VULKAN_COMPUTE_ROUND') for line in log.splitlines() if line.startswith('VULKAN_COMPUTE_ROUND ')]
    failure = [line for line in log.splitlines() if line.startswith('VULKAN_BRIDGE_FAIL ')]
    mismatches = [line for line in log.splitlines() if line.startswith('VULKAN_COMPUTE_MISMATCH ')]
    expected_exit = '0' if profile == 'pass' else '1'
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.M) == [expected_exit], 'Wrong guest exit')
    if profile == 'pass':
        require(not failure and not mismatches and len(rounds) == 2, 'Shader failed or rounds missing')
        for index, row in enumerate(rounds):
            exact(row, dict(round=index, verifiedWords=8294400, fnv1a=EXPECTED_HASHES[index],
                referenceFnv1a=EXPECTED_HASHES[index], coherent='true', gpuOnlyStorage='true', cpuStorageWrites='false'))
        exact(one(log, 'VULKAN_COMPUTE_COMPLETE'), dict(verified='true', width=3840, height=2160, rounds=2,
            wordsPerRound=8294400, gpuOnlyStorage='true', gpuTransferReadback='true', nvidiaUmdLoaded='true',
            window='false', cpuStorageWrites='false'))
        exact(one(log, 'VULKAN_COMPUTE_DEVICE_READY'), dict(verified='true', vendor=4318, computeQueue='true',
            nvidiaUmdLoaded='true', computeSubmissions=2, window='false'))
    else:
        require(not rounds and 'VULKAN_COMPUTE_COMPLETE' not in log and 'VULKAN_COMPUTE_DEVICE_READY' not in log,
                'Rejected control cannot become compute acceptance')
        if profile == 'slot-refusal':
            require(failure == ['VULKAN_BRIDGE_FAIL compute-map-readback'] and not mismatches,
                    'Wrong allocation refusal boundary')
        else:
            require(failure == ['VULKAN_BRIDGE_FAIL compute readback differs from integer oracle'] and
                    mismatches == ['VULKAN_COMPUTE_MISMATCH round=0 x=3839 y=2159 actual=0 expected=4280660732'],
                    'Last-word GPU corruption was not rejected by the oracle')
    events = one(log, 'KERNEL_EVENTFD_EXIT')
    created = events.get('created', '')
    require(re.fullmatch(r'[1-9][0-9]*', created), 'Missing real kernel GPU notification')
    exact(events, dict(held=0, pending=0, created=created, notified=created, errors=0, released=created, cleanup='true'))
    require(log.count('asyncCpuFenceWaitRegistered=true') == int(created), 'Kernel notification count mismatch')
    require(not any(marker in log for marker in ('EVENTFD_CONTRACT_FAIL', 'APERTURE_CONTRACT_FAIL', 'KERNEL_EVENTFD_EXIT cleanup=false')),
            'Kernel contract failed')
    return dict(profile=profile, shaderRoundsVerified=2 if profile == 'pass' else 0,
        wordsVerifiedPerRound=8294400 if profile == 'pass' else 0,
        gpuOnlyStorage=True, cpuStorageWrites=False, kernelNotifications=int(created),
        finalWordCorruptionRejected=profile == 'corrupt-last', slotRefusalRetained=profile == 'slot-refusal',
        windowPresentationVerified=False, dailyUseAccepted=False)


def validate_compute_controls(source, log, records, cpu_slots):
    require(type(cpu_slots) is int and cpu_slots in (128, 256), 'Unknown CPU slot profile')
    command = source.get('command')
    require(type(command) is list and command and all(type(arg) is str for arg in command), 'Malformed command')
    require(not FORBIDDEN.intersection(command), 'Foreground/input/disk/CPU-store option present')
    for option, value in (('--runtime-workload', 'init'), ('--driver-cpu-slots', str(cpu_slots)), ('--driver-fence-slots', '128')):
        require(command.count(option) == 1, 'Missing/duplicate ' + option)
        index = command.index(option)
        require(index + 1 < len(command) and command[index + 1] == value, 'Wrong ' + option)
    require(source.get('sourceDiskAttached') is False and source.get('guiFlagsAbsent') is True and
            source.get('inputFlagsAbsent') is True and broker_runtime_complete(log), 'Background broker isolation/lifetime differs')
    require(not any(key.startswith(('nativeSdl', 'nativeSharedTexturePresent')) or key == 'nativeInputChannelOpened'
                    for row in records for key in row), 'Presentation/input receipt in compute run')


def validate_compute_cleanup(report, records, cpu_slots, profile):
    """Exact 128/256 geometry, with the original 4K/128-slot oracle unchanged."""
    require(type(cpu_slots) is int and cpu_slots in (128, 256) and profile in ('pass', 'slot-refusal', 'corrupt-last'), 'Unknown cleanup profile')
    cleanup, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(type(report.get('hostBridgeExit')) is int and report['hostBridgeExit'] == 0 and
            cleanup.get('driverCleanupVerified') is True and type(cleanup.get('cleanupFailures')) is int and
            cleanup['cleanupFailures'] == 0, 'Native cleanup not verified')
    for key in LIVE_COUNTERS:
        require(type(cleanup.get('live' + key)) is int and cleanup['live' + key] == 0, 'Missing/live native object: ' + key)
    for section in (cleanup, vm):
        for key, value in section.items():
            if key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith(('Timeouts', 'ReleasedAfterVmExit')):
                require(type(value) is int and value == 0, 'Failed or deferred work: ' + key)
    require(vm.get('ownedQemuExited') is True and type(vm.get('qemuExit')) is int and vm['qemuExit'] == 0 and
            vm.get('qemuForcedStop') is False and vm.get('fenceControlFailed') is False and
            type(vm.get('fenceApertureSlots')) is int and vm['fenceApertureSlots'] == 128 and
            type(vm.get('allocationApertureSlots')) is int and vm['allocationApertureSlots'] == cpu_slots,
            'VM lifetime or aperture geometry differs')
    for created, acknowledged in (('fenceMappingsCreated', 'fenceUnmapAcknowledgements'),
        ('allocationMappingsCreated', 'allocationUnmapAcknowledgements'), ('allocationSlotMappingsCreated', 'allocationSlotUnmapAcknowledgements')):
        require(type(vm.get(created)) is int and type(vm.get(acknowledged)) is int and 0 < vm[created] == vm[acknowledged],
                'Unacknowledged before-VM mapping retirement')
    for key, marker in (('disconnectCleanup', 'driverCleanupVerified'), ('ownedQemuControl', 'ownedQemuExited')):
        selected = [row for row in records if marker in row]
        require(len(selected) == 1 and json.dumps(report[key], sort_keys=True) == json.dumps(selected[0], sort_keys=True),
                'Report/native cleanup mismatch')
    for key, expected in (('vendorCpuSlotLimit', cpu_slots), ('vendorCpuMappedByteLimit', 100663296),
                          ('reportedGpuUsageLimitBytes', 268435456), ('vendorCpuSlotQuotaRejections', 1 if profile == 'slot-refusal' else 0)):
        require(type(cleanup.get(key)) is int and cleanup[key] == expected, 'Resource guard/profile differs: ' + key)
    for used, limit in (('peakVendorCpuBytes', 'vendorCpuMappedByteLimit'), ('peakReportedGpuUsageBytes', 'reportedGpuUsageLimitBytes')):
        require(type(cleanup.get(used)) is int and 0 < cleanup[used] <= cleanup[limit], 'Wrong/unused memory guard')
    require(report.get('diagnosticCpuStoresRequested') is False and cleanup.get('cpuStoreTestRequested') is False and
            type(cleanup.get('completedCpuStoreTests')) is int and cleanup['completedCpuStoreTests'] == 0, 'CPU-store diagnostic requested')
    for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired'), ('nativeHwQueueSignalReturns', 'nativeHwQueueSignalReturned'),
        ('nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalAccepted'), ('nativeHwQueueSignalRetirements', 'nativeHwQueueSignalRetired')):
        require(json.dumps(report[key], sort_keys=True) == json.dumps([row for row in records if marker in row], sort_keys=True),
                'Report/native GPU receipt mismatch: ' + key)
    require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
        report['nativeAsyncSubmissionRetirements'], cleanup) and type(cleanup.get('completedNativeSubmissions')) is int and
        cleanup['completedNativeSubmissions'] >= 6, 'Unretired/unused GPU submissions')
    require(hw_queue_signals_complete(report['guestHwQueueSignals'], report['nativeHwQueueSignalReturns'],
        report['nativeHwQueueSignalAcceptances'], report['nativeHwQueueSignalRetirements'], cleanup) and
        type(cleanup.get('completedHwQueueSignals')) is int and cleanup['completedHwQueueSignals'] >= 2, 'Unretired/unused GPU signals')
    require(type(cleanup.get('completedDeviceExecutionQueries')) is int and cleanup['completedDeviceExecutionQueries'] >= 2,
            'No actual device-health checks')
    refused = [row for row in records if 'nativeCpuSlotAdmissionRejected' in row]
    if profile == 'slot-refusal':
        require(cpu_slots == 128 and len(refused) == 1, 'Missing slot refusal')
        expected = dict(nativeCpuSlotAdmissionRejected=True, requestedBytes=33554432, liveChunks=98, slotLimit=128,
            largestFreeRunSlots=30, mappedBytes=26456064, runtimeStopped=False, cpuEnabled=True)
        require(json.dumps(refused[0], sort_keys=True) == json.dumps(expected, sort_keys=True), 'Wrong slot-refusal evidence')
    else:
        require(cpu_slots == 256 and not refused, 'Unexpected slot refusal')
    return dict(allNativeMappingsRetiredBeforeVmExit=True, naturalHostAndVmExit=True,
        cpuApertureSlots=cpu_slots, fenceApertureSlots=128, cpuMappedByteLimit=100663296,
        gpuUsageByteLimit=268435456, nativeSubmissions=cleanup['completedNativeSubmissions'],
        retiredHardwareQueueSignals=cleanup['completedHwQueueSignals'], deviceHealthQueries=cleanup['completedDeviceExecutionQueries'],
        peakCpuMappedBytes=cleanup['peakVendorCpuBytes'], peakReportedGpuUsageBytes=cleanup['peakReportedGpuUsageBytes'])


def validate_refusal_retention(report, records):
    """Preserve the failed 128-slot recovery; prove eventual cleanup separately."""
    # This historical oracle proves mapping retirement at its original 128-slot
    # geometry. It does not establish native reservation retirement before exit.
    validate_cleanup(report, records)
    cleanup, vm = report['disconnectCleanup'], report['ownedQemuControl']
    expected_deferred = {'gpuReservationsReleasedAfterVmExit': 4}
    deferred = {}
    for section in (cleanup, vm):
        for key, value in section.items():
            if key.endswith('ReleasedAfterVmExit'):
                require(type(value) is int and value == expected_deferred.get(key, 0), 'Refusal retention differs: ' + key)
                if value: deferred[key] = value
            elif key.startswith(('failed', 'live', 'pending', 'forced')) or key.endswith('Timeouts'):
                require(type(value) is int and value == 0, 'Failed/live native work: ' + key)
    require(deferred == expected_deferred and type(cleanup.get('vendorCpuSlotQuotaRejections')) is int and
            cleanup['vendorCpuSlotQuotaRejections'] == 1 and cleanup['vendorCpuSlotLimit'] == 128 and
            cleanup['vendorCpuMappedByteLimit'] == 100663296 and cleanup['reportedGpuUsageLimitBytes'] == 268435456,
            'Observed slot refusal/retention was erased')
    refused = [row for row in records if 'nativeCpuSlotAdmissionRejected' in row]
    expected = dict(nativeCpuSlotAdmissionRejected=True, requestedBytes=33554432, liveChunks=98, slotLimit=128,
        largestFreeRunSlots=30, mappedBytes=26456064, runtimeStopped=False, cpuEnabled=True)
    require(len(refused) == 1 and json.dumps(refused[0], sort_keys=True) == json.dumps(expected, sort_keys=True),
            'Missing exact slot admission failure')
    try: validate_compute_cleanup(report, records, 128, 'slot-refusal')
    except ValueError as error:
        require(str(error) == 'Failed or deferred work: gpuReservationsReleasedAfterVmExit', 'Unexpected strict cleanup rejection')
    else: raise ValueError('Failed before-VM reservation recovery was promoted')
    return dict(eventualNativeAndVmCleanupVerified=True, allNativeMappingsRetiredBeforeVmExit=True,
        allNativeObjectsRetiredBeforeVmExit=False, releasedAfterVmExit=deferred,
        liveReservationRecoveryVerified=False, fullComputeCleanupAccepted=False)
