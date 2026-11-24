"""Validate scoped GPU wait controls; keep exit-time native retention visible."""
from collections import Counter
import json
import re
from verify_4k_capture import require
from background_shader_evidence import FORBIDDEN
from broker_runtime_evidence import broker_runtime_complete

CASES = (
    'name=pending passed=true gpuSignal=true',
    'name=all passed=true firstAloneDidNotNotify=true',
    'name=any passed=true otherFenceRemainedZero=true',
    'name=fd-reuse passed=true replacementNotSignaled=true',
    'name=quota passed=true accepted=64 refused=1 allReleased=true',
    'name=full-counter passed=true kernelDidNotBlock=true',
)
SIGNAL_TARGETS = (1, 3, 7, 9, 11, 29, 43)


def fields(line, prefix):
    require(line.startswith(prefix + ' '), 'Wrong receipt prefix')
    result = {}
    for item in line[len(prefix) + 1:].split(' '):
        key, separator, value = item.partition('=')
        require(separator == '=' and re.fullmatch(r'[A-Za-z][A-Za-z0-9]*', key) and
                value and '=' not in value and key not in result, 'Malformed or duplicate receipt field')
        result[key] = value
    return result


def one(log, prefix):
    rows = [line for line in log.splitlines() if line.startswith(prefix + ' ')]
    require(len(rows) == 1, 'Missing/duplicate ' + prefix)
    return fields(rows[0], prefix)


def exact(value, expected):
    require(value == {key: str(item) for key, item in expected.items()}, 'Receipt fields differ')


def validate_wait_receipts(log, profile):
    require(profile in ('normal', 'process-exit'), 'Unknown wait profile')
    require(not any(marker in log for marker in ('ASYNC_CPU_WAIT_TEST_FAIL', 'APERTURE_CONTRACT_FAIL',
        'EVENTFD_CONTRACT_FAIL', 'KERNEL_EVENTFD_EXIT cleanup=false')), 'Wait or kernel contract failed')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.M) == ['0'], 'Guest runtime did not exit successfully')
    guard = one(log, 'EVENTFD_CONTRACT_PASS')
    require(guard.get('checks', '').isdigit() and int(guard['checks']) >= 31, 'Missing invalid-request controls')
    exact(guard, dict(checks=guard['checks'], published=0, notified=0, gpu='false',
        partialRollback='true', foreignOwnerRejected='true'))
    hresults = [fields(line, 'ASYNC_CPU_WAIT_HRESULT') for line in log.splitlines()
                if line.startswith('ASYNC_CPU_WAIT_HRESULT ')]
    require(all(set(row) == {'stage', 'value'} and row['value'] == '00000000' for row in hresults),
            'Failed or malformed API HRESULT')
    actual_cases = [line.removeprefix('ASYNC_CPU_WAIT_CASE ') for line in log.splitlines()
                    if line.startswith('ASYNC_CPU_WAIT_CASE ')]
    if profile == 'normal':
        require(actual_cases == list(CASES), 'Missing, duplicate, reordered or failed GPU wait case')
        require('ASYNC_CPU_WAIT_EXIT_CONTROL' not in log, 'Mixed wait profiles')
        expected_stages = {'device-interface': 1, 'create-fence': 8,
            'pending-register': 1, 'pending-gpu-signal': 1, 'all-register': 1,
            'all-first-gpu-signal': 1, 'all-second-gpu-signal': 1,
            'any-register': 1, 'any-second-gpu-signal': 1, 'reuse-register': 1, 'reuse-gpu-signal': 1,
            'quota-register': 64, 'quota-gpu-signal': 1, 'full-counter-register': 1, 'full-counter-gpu-signal': 1}
        exact(one(log, 'ASYNC_CPU_WAIT_TEST_COMPLETE'), dict(verified='true', cases=6, created=69,
            notified=69, released=69, errors=0, held=0, pending=0, cpuFenceStores='false', shaderExecution='false'))
        created, notified, errors = 69, 69, 0
    else:
        require(not actual_cases and 'ASYNC_CPU_WAIT_TEST_COMPLETE' not in log, 'Exit control is not normal-case acceptance')
        expected_stages = {'device-interface': 1, 'create-fence': 1, 'exit-control-register': 1}
        exact(one(log, 'ASYNC_CPU_WAIT_EXIT_CONTROL'), dict(pending=1, notified=0,
            futureTarget=999, cpuFenceStores='false'))
        created, notified, errors = 1, 0, 1
    require(Counter(row['stage'] for row in hresults) == Counter(expected_stages), 'Incomplete API call receipts')
    exact(one(log, 'KERNEL_EVENTFD_EXIT'), dict(held=0, pending=0, created=created,
        notified=notified, errors=errors, released=created, cleanup='true'))
    registrations = re.findall(r'^LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=(\d+) any=([01]) kernelEventfd=true$', log, re.M)
    require(len(registrations) == created and all(1 <= int(count) <= 64 for count, any_flag in registrations),
            'Missing or malformed kernel wait registrations')
    expected_pairs = Counter({('1', '0'): 67, ('2', '0'): 1, ('2', '1'): 1}) if profile == 'normal' else Counter({('1', '0'): 1})
    require(Counter(registrations) == expected_pairs, 'ANY/ALL registration contract differs')
    return dict(profile=profile, kernelContractChecks=int(guard['checks']),
        hardwareWaitCases=6 if profile == 'normal' else 0, registrations=created,
        gpuRetirementNotifications=notified, expectedExitCancellations=errors,
        allKernelNotificationsReleasedBeforeVmExit=True, cpuGpuFenceStores=False,
        shaderExecutionVerified=False, desktopAccepted=False)


def validate_exit_broker(log):
    """Record incomplete actor teardown; never promote it to broker acceptance."""
    require(not broker_runtime_complete(log) and 'transport=virtio-port' not in log,
            'Expected process-exit broker retention was erased or bypassed')
    for prefix, expected in (
        ('KERNEL_APERTURE_START', dict(loaded='true', contracts='true', rawSysfsFallback='false')),
        ('BROKER_RENDER_CLIENT', dict(uid=65534, gid=65534, effectiveCapabilities=0,
            noNewPrivs='true', controllerOpen='denied', nativePortOpen='denied')),
        ('BROKER_OBJECT_DRAIN', dict(producerExited='true', retained=0)),
        ('BROKER_ROUTE_REAP', dict(exit=0)),
        ('KERNEL_APERTURE_EXIT', dict(activeLeases=0, activeCpuBytes=0, mmioReads=0, mmioWrites=0, unloaded='true'))):
        exact(one(log, prefix), expected)
    start = one(log, 'BROKER_ROUTE_START')
    require(re.fullmatch(r'[1-9][0-9]*', start.get('clientPid', '')) and int(start['clientPid']) < 1 << 31,
            'Invalid broker client PID')
    exact(start, dict(protocol=3, clientPid=start['clientPid'], clientUid=65534,
        nativeWorkerClients=1, kernelControllersClient='false'))
    transport = 'LINUX_BRIDGE transport=authenticated-broker nativeWorkerClients=1'
    require([line for line in log.splitlines() if line.startswith('LINUX_BRIDGE transport=')] == [transport],
            'Unverified broker route')
    end = one(log, 'BROKER_ROUTE_EXIT')
    for key in ('retainedViews', 'nativeCalls', 'viewsCreated', 'viewsReleased'):
        require(re.fullmatch(r'0|[1-9][0-9]*', end.get(key, '')) and int(end[key]) < 1 << 64,
                'Invalid broker count')
    exact(end, dict(processExited='true', retainedViews=end['retainedViews'], nativeCalls=end['nativeCalls'],
        viewsCreated=end['viewsCreated'], viewsReleased=end['viewsReleased'], rejected=0, multiClient='false'))
    require(int(end['nativeCalls']) > 0 and int(end['retainedViews']) == int(end['viewsCreated']) > 0 and
            int(end['viewsReleased']) == 0, 'Expected actor retention differs')
    prefixes = ('KERNEL_APERTURE_START', 'BROKER_RENDER_CLIENT', 'BROKER_ROUTE_START',
        'LINUX_BRIDGE transport=', 'BROKER_OBJECT_DRAIN', 'BROKER_ROUTE_EXIT', 'BROKER_ROUTE_REAP', 'KERNEL_APERTURE_EXIT')
    lines = log.splitlines()
    positions = [next(index for index, line in enumerate(lines) if line.startswith(prefix)) for prefix in prefixes]
    require(positions == sorted(positions), 'Unordered broker lifecycle')
    return dict(brokerCleanTeardownVerified=False, brokerRetainedViewsBeforeVmExit=int(end['retainedViews']),
                kernelLeasesRetired=True)


def validate_background_wait_controls(source, guest, records, profile='normal'):
    require(profile in ('normal', 'process-exit'), 'Unknown wait profile')
    command = source.get('command')
    require(type(command) is list and command and all(type(arg) is str for arg in command), 'Invalid command')
    require(not FORBIDDEN.intersection(command), 'Foreground/input/disk option present')
    for option, expected in (('--runtime-workload', 'init'), ('--driver-cpu-slots', '128'),
                             ('--driver-fence-slots', '128')):
        require(command.count(option) == 1, 'Missing/duplicate ' + option)
        index = command.index(option)
        require(index + 1 < len(command) and command[index + 1] == expected, 'Wrong ' + option)
    require(source.get('sourceDiskAttached') is False and source.get('guiFlagsAbsent') is True and
            source.get('inputFlagsAbsent') is True, 'Background source contract differs')
    if profile == 'normal':
        require(broker_runtime_complete(guest), 'Broker isolation/teardown not verified')
        broker = dict(brokerCleanTeardownVerified=True, brokerRetainedViewsBeforeVmExit=0, kernelLeasesRetired=True)
    else:
        broker = validate_exit_broker(guest)
    require(not any(key.startswith(('nativeSdl', 'nativeSharedTexturePresent')) or
                    key == 'nativeInputChannelOpened' for row in records for key in row),
            'Presentation/input receipt in background run')
    return broker


def validate_gpu_signals(report, records, profile):
    """Match the real context signals; these runs do not exercise HW-queue signals."""
    require(profile in ('normal', 'process-exit'), 'Unknown wait profile')
    targets = list(SIGNAL_TARGETS) if profile == 'normal' else []
    statuses = [row for row in records if 'nativeContextSignalStatus' in row]
    retirements = [row for row in records if 'nativeContextSignalRetired' in row]
    require(len(statuses) == len(retirements) == len(targets), 'Missing/duplicate native GPU signal')
    projected = []
    for status, retired, target in zip(statuses, retirements, targets):
        require(status.get('nativeContextSignalStatus') is True and type(status.get('ntstatus')) is int and
                status['ntstatus'] == 0 and type(status.get('flags')) is int and status['flags'] == 4 and
                type(status.get('target')) is int and status['target'] == target, 'Failed/wrong native signal')
        require(retired.get('nativeContextSignalRetired') is True and type(retired.get('target')) is int and
                retired['target'] == target and type(retired.get('observed')) is int and
                target <= retired['observed'] < (1 << 64) - 1 and retired.get('noGpuAccess') is True and
                retired.get('cpuValueWrittenByBridge') is False, 'Unretired or CPU-signaled GPU fence')
        projected.append(dict(flags=4, target=target, observed=retired['observed']))
    for key, expected in (('nativeContextSignals', projected), ('nativeContextSignalRetirementChecks', retirements)):
        require(json.dumps(report.get(key), sort_keys=True) == json.dumps(expected, sort_keys=True),
                'Report/native context signal mismatch')
    cleanup = report['disconnectCleanup']
    for key in ('nativeContextSignalAttempts', 'completedNativeContextSignals'):
        require(type(cleanup.get(key)) is int and cleanup[key] == len(targets), 'Native signal count mismatch')
    for key in ('failedNativeContextSignals', 'nativeContextSignalTimeouts', 'acceptedAsyncContextSignals',
                'retiredAsyncContextSignals', 'pendingAsyncContextSignals', 'nativeHwQueueSignalAttempts',
                'completedHwQueueSignals', 'completedCpuStoreTests'):
        require(type(cleanup.get(key)) is int and cleanup[key] == 0, 'Unexpected native signal/test profile')
    require(report.get('diagnosticCpuStoresRequested') is False and cleanup.get('cpuStoreTestRequested') is False,
            'CPU store diagnostic requested')
    return dict(retiredContextGpuSignals=len(targets), hardwareQueueSignalsExercised=False,
                asynchronousContextSignalsExercised=False, cpuGpuFenceStores=False)


def validate_exit_cleanup(report, records):
    """Require complete eventual cleanup, recording the observed VM-exit debt."""
    cleanup, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(type(report.get('hostBridgeExit')) is int and report['hostBridgeExit'] == 0 and
            cleanup.get('driverCleanupVerified') is True and type(cleanup.get('cleanupFailures')) is int and
            cleanup['cleanupFailures'] == 0, 'Exit cleanup failed')
    for key, value in cleanup.items():
        if key.startswith(('live', 'pending', 'failed', 'forced')) or key.endswith('Timeouts'):
            require(type(value) is int and value == 0, 'Unreleased or failed native work: ' + key)
    require(vm.get('ownedQemuExited') is True and type(vm.get('qemuExit')) is int and vm['qemuExit'] == 0 and
            vm.get('qemuForcedStop') is False and vm.get('fenceControlFailed') is False,
            'VM did not exit naturally')
    for key, value in vm.items():
        if key.startswith('live'):
            require(type(value) is int and value == 0, 'Live VM mapping: ' + key)
    for created, acknowledged, deferred in (
        ('fenceMappingsCreated', 'fenceUnmapAcknowledgements', 'fenceMappingsReleasedAfterVmExit'),
        ('allocationMappingsCreated', 'allocationUnmapAcknowledgements', 'allocationMappingsReleasedAfterVmExit'),
        ('allocationSlotMappingsCreated', 'allocationSlotUnmapAcknowledgements', 'allocationSlotMappingsReleasedAfterVmExit')):
        require(all(type(vm.get(k)) is int and vm[k] >= 0 for k in (created, acknowledged, deferred)) and
                0 < vm[created] == vm[acknowledged] + vm[deferred], 'Missing or mismatched exit retirement')
    require(vm['fenceMappingsCreated'] > 0 and vm['fenceApertureSlots'] == vm['allocationApertureSlots'] == 128,
            'Wrong or unused aperture profile')
    for key, marker in (('disconnectCleanup', 'driverCleanupVerified'), ('ownedQemuControl', 'ownedQemuExited')):
        selected = [row for row in records if marker in row]
        require(len(selected) == 1 and json.dumps(report[key], sort_keys=True) == json.dumps(selected[0], sort_keys=True),
                'Report/cleanup receipt mismatch')
    require(all(type(value) is int and value >= 0 for section in (cleanup, vm) for key, value in section.items()
                if key.endswith('ReleasedAfterVmExit')), 'Invalid deferred retirement counter')
    debt = {key: value for section in (cleanup, vm) for key, value in section.items()
            if key.endswith('ReleasedAfterVmExit') and value}
    require(all(type(value) is int and value > 0 for value in debt.values()), 'Invalid deferred retirement counter')
    return dict(eventualNativeAndVmCleanupVerified=True, releasedAfterVmExit=debt,
                liveNativeCrashRecoveryBeforeVmExitVerified=not bool(debt))
