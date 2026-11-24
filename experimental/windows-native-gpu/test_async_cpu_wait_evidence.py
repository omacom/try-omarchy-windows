"""Reject incomplete waits, invented GPU retirement and hidden VM-exit retention."""
import copy
import unittest
from async_cpu_wait_evidence import (CASES, SIGNAL_TARGETS, validate_wait_receipts,
    validate_exit_cleanup, validate_gpu_signals, validate_background_wait_controls, validate_exit_broker)
from background_shader_evidence import FORBIDDEN
from test_broker_runtime_evidence import BROKER
from verify_4k_capture import validate_cleanup


def log_fixture(profile='normal'):
    stages = ['device-interface']
    if profile == 'normal':
        stages += ['create-fence', 'pending-register', 'pending-gpu-signal',
            'create-fence', 'create-fence', 'all-register', 'all-first-gpu-signal', 'all-second-gpu-signal',
            'create-fence', 'create-fence', 'any-register', 'any-second-gpu-signal',
            'create-fence', 'reuse-register', 'reuse-gpu-signal', 'create-fence']
        stages += ['quota-register'] * 64 + ['quota-gpu-signal', 'create-fence',
            'full-counter-register', 'full-counter-gpu-signal']
        rows = ['ASYNC_CPU_WAIT_CASE ' + value for value in CASES]
        rows += ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=1 any=0 kernelEventfd=true'] * 67
        rows += ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=2 any=0 kernelEventfd=true',
            'LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=2 any=1 kernelEventfd=true',
            'ASYNC_CPU_WAIT_TEST_COMPLETE verified=true cases=6 created=69 notified=69 released=69 errors=0 held=0 pending=0 cpuFenceStores=false shaderExecution=false',
            'KERNEL_EVENTFD_EXIT held=0 pending=0 created=69 notified=69 errors=0 released=69 cleanup=true']
    else:
        stages += ['create-fence', 'exit-control-register']
        rows = ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=1 any=0 kernelEventfd=true',
            'ASYNC_CPU_WAIT_EXIT_CONTROL pending=1 notified=0 futureTarget=999 cpuFenceStores=false',
            'KERNEL_EVENTFD_EXIT held=0 pending=0 created=1 notified=0 errors=1 released=1 cleanup=true']
    return '\n'.join(['EVENTFD_CONTRACT_PASS checks=31 published=0 notified=0 gpu=false partialRollback=true foreignOwnerRejected=true'] +
        ['ASYNC_CPU_WAIT_HRESULT stage=' + stage + ' value=00000000' for stage in stages] + rows + ['BRIDGE_RUNTIME_EXIT=0', ''])


def cleanup_fixture():
    cleanup = dict(driverCleanupVerified=True, cleanupFailures=0, liveAllocations=0, pendingNativeSubmissions=0,
        failedNativeSubmissions=0, nativeSubmissionTimeouts=0, cpuLocksReleasedAfterVmExit=44)
    vm = dict(ownedQemuExited=True, qemuExit=0, qemuForcedStop=False, fenceControlFailed=False,
        liveFenceMappings=0, fenceMappingsCreated=60, fenceUnmapAcknowledgements=0, fenceMappingsReleasedAfterVmExit=60,
        allocationMappingsCreated=44, allocationUnmapAcknowledgements=0, allocationMappingsReleasedAfterVmExit=44,
        allocationSlotMappingsCreated=51, allocationSlotUnmapAcknowledgements=0, allocationSlotMappingsReleasedAfterVmExit=51,
        fenceApertureSlots=128, allocationApertureSlots=128)
    report = dict(hostBridgeExit=0, disconnectCleanup=cleanup, ownedQemuControl=vm)
    return report, [copy.deepcopy(cleanup), copy.deepcopy(vm)]


def signal_fixture():
    statuses = [dict(nativeContextSignalStatus=True, flags=4, target=target, ntstatus=0) for target in SIGNAL_TARGETS]
    retired = [dict(nativeContextSignalRetired=True, target=target, observed=target,
        noGpuAccess=True, cpuValueWrittenByBridge=False) for target in SIGNAL_TARGETS]
    counters = {key: 0 for key in ('failedNativeContextSignals', 'nativeContextSignalTimeouts',
        'acceptedAsyncContextSignals', 'retiredAsyncContextSignals', 'pendingAsyncContextSignals',
        'nativeHwQueueSignalAttempts', 'completedHwQueueSignals', 'completedCpuStoreTests')}
    counters.update(nativeContextSignalAttempts=7, completedNativeContextSignals=7, cpuStoreTestRequested=False)
    return dict(nativeContextSignals=[dict(flags=4, target=target, observed=target) for target in SIGNAL_TARGETS],
        nativeContextSignalRetirementChecks=copy.deepcopy(retired), disconnectCleanup=counters,
        diagnosticCpuStoresRequested=False), statuses + retired


class WaitReceipts(unittest.TestCase):
    def test_normal_and_cancellation_are_separate_scopes(self):
        normal = validate_wait_receipts(log_fixture(), 'normal')
        exited = validate_wait_receipts(log_fixture('process-exit'), 'process-exit')
        self.assertEqual(normal['gpuRetirementNotifications'], 69)
        self.assertEqual(exited['expectedExitCancellations'], 1)
        self.assertFalse(normal['desktopAccepted'])
        self.assertFalse(exited['shaderExecutionVerified'])
        for text, profile in ((log_fixture(), 'process-exit'), (log_fixture('process-exit'), 'normal'),
                              (log_fixture(), 'unknown')):
            with self.subTest(profile=profile), self.assertRaises(ValueError): validate_wait_receipts(text, profile)

    def test_each_missing_or_duplicate_case_and_reordered_cases_are_rejected(self):
        original = log_fixture()
        for case in CASES:
            line = 'ASYNC_CPU_WAIT_CASE ' + case + '\n'
            for text in (original.replace(line, ''), original + line):
                with self.subTest(case=case), self.assertRaises(ValueError): validate_wait_receipts(text, 'normal')
        text = original.replace(CASES[0], 'SWAP').replace(CASES[1], CASES[0]).replace('SWAP', CASES[1])
        with self.assertRaises(ValueError): validate_wait_receipts(text, 'normal')

    def test_bad_api_result_missing_call_and_duplicate_receipts_are_rejected(self):
        text = log_fixture()
        line = 'ASYNC_CPU_WAIT_HRESULT stage=quota-register value=00000000\n'
        for bad in (text.replace(line, '', 1), text + line, text.replace('value=00000000', 'value=80004005', 1),
                    text.replace('checks=31', 'checks=0'), text + 'BRIDGE_RUNTIME_EXIT=0\n',
                    text + 'EVENTFD_CONTRACT_FAIL line=1\n', text + 'ASYNC_CPU_WAIT_TEST_FAIL reason=test\n'):
            with self.subTest(bad=bad[-100:]), self.assertRaises(ValueError): validate_wait_receipts(bad, 'normal')

    def test_counts_any_all_witnesses_and_cpu_store_policy_cannot_be_laundered(self):
        text = log_fixture()
        for old, new in (('created=69', 'created=68'), ('notified=69', 'notified=68'),
                         ('errors=0', 'errors=1'), ('count=2 any=1', 'count=2 any=0'),
                         ('cpuFenceStores=false', 'cpuFenceStores=true'),
                         ('replacementNotSignaled=true', 'replacementNotSignaled=false'),
                         ('otherFenceRemainedZero=true', 'otherFenceRemainedZero=false'),
                         ('partialRollback=true', 'partialRollback=false'), ('gpu=false', 'gpu=true')):
            with self.subTest(old=old), self.assertRaises(ValueError): validate_wait_receipts(text.replace(old, new), 'normal')
        exit_log = log_fixture('process-exit')
        for bad in (exit_log.replace('notified=0 errors=1', 'notified=1 errors=0'),
                    exit_log.replace('futureTarget=999', 'futureTarget=0'),
                    exit_log.replace('released=1', 'released=0')):
            with self.assertRaises(ValueError): validate_wait_receipts(bad, 'process-exit')

    def test_duplicate_or_unknown_receipt_fields_are_rejected(self):
        for suffix in (' checks=31', ' extra=true', ' bad'):
            bad = log_fixture().replace('foreignOwnerRejected=true', 'foreignOwnerRejected=true' + suffix)
            with self.assertRaises(ValueError): validate_wait_receipts(bad, 'normal')


class NativeSignals(unittest.TestCase):
    def test_gpu_context_signals_are_correlated_without_claiming_hw_or_async_signals(self):
        report, records = signal_fixture()
        value = validate_gpu_signals(report, records, 'normal')
        self.assertEqual(value['retiredContextGpuSignals'], 7)
        self.assertFalse(value['hardwareQueueSignalsExercised'])
        self.assertFalse(value['asynchronousContextSignalsExercised'])

    def test_cpu_store_failed_unretired_sentinel_wrong_target_and_duplicates_are_rejected(self):
        for section, key, value in ((0, 'ntstatus', 1), (0, 'flags', True), (7, 'observed', 0),
                                   (7, 'observed', (1 << 64) - 1), (7, 'target', 2),
                                   (7, 'cpuValueWrittenByBridge', True), (7, 'noGpuAccess', False)):
            report, records = signal_fixture(); records[section][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): validate_gpu_signals(report, records, 'normal')
        report, records = signal_fixture()
        for bad in (records[:-1], records + records[:1], []):
            with self.assertRaises(ValueError): validate_gpu_signals(report, bad, 'normal')

    def test_report_disagreement_and_fake_counter_types_are_rejected(self):
        for key, value in (('completedNativeContextSignals', True), ('nativeContextSignalTimeouts', False),
                           ('nativeHwQueueSignalAttempts', 1), ('completedCpuStoreTests', 1)):
            report, records = signal_fixture(); report['disconnectCleanup'][key] = value
            with self.assertRaises(ValueError): validate_gpu_signals(report, records, 'normal')
        report, records = signal_fixture(); report['nativeContextSignals'][0]['observed'] = 2
        with self.assertRaises(ValueError): validate_gpu_signals(report, records, 'normal')


class ExitCleanup(unittest.TestCase):
    def test_eventual_cleanup_keeps_native_vm_exit_debt_visible(self):
        report, records = cleanup_fixture()
        value = validate_exit_cleanup(report, records)
        self.assertTrue(value['eventualNativeAndVmCleanupVerified'])
        self.assertFalse(value['liveNativeCrashRecoveryBeforeVmExitVerified'])
        self.assertEqual(value['releasedAfterVmExit']['fenceMappingsReleasedAfterVmExit'], 60)
        with self.assertRaises(ValueError): validate_cleanup(report, records)

    def test_erasing_retention_without_real_unmap_acknowledgements_is_rejected(self):
        report, records = cleanup_fixture()
        report['ownedQemuControl']['fenceMappingsReleasedAfterVmExit'] = 0
        records[1] = copy.deepcopy(report['ownedQemuControl'])
        with self.assertRaises(ValueError): validate_exit_cleanup(report, records)

    def test_bad_cleanup_types_failures_timeouts_and_forced_exit_are_rejected(self):
        for section, key, value in (('disconnectCleanup', 'cleanupFailures', False),
            ('disconnectCleanup', 'pendingNativeSubmissions', 1), ('disconnectCleanup', 'nativeSubmissionTimeouts', 1),
            ('disconnectCleanup', 'cpuLocksReleasedAfterVmExit', False), ('ownedQemuControl', 'qemuForcedStop', True),
            ('ownedQemuControl', 'fenceUnmapAcknowledgements', True), ('ownedQemuControl', 'fenceMappingsReleasedAfterVmExit', -1)):
            report, records = cleanup_fixture(); report[section][key] = value
            records[0 if section == 'disconnectCleanup' else 1] = copy.deepcopy(report[section])
            with self.subTest(key=key, value=value), self.assertRaises(ValueError): validate_exit_cleanup(report, records)

    def test_raw_receipt_disagreement_or_duplicate_cannot_be_hidden(self):
        report, records = cleanup_fixture()
        for bad in (records[:1], records + records[:1], [dict(records[0], cleanupFailures=False), records[1]]):
            with self.assertRaises(ValueError): validate_exit_cleanup(report, bad)


class BackgroundIsolation(unittest.TestCase):
    def test_process_exit_broker_retention_stays_rejected_by_normal_teardown(self):
        text = BROKER.replace('retainedViews=0 nativeCalls=1242 viewsCreated=173 viewsReleased=173',
                              'retainedViews=104 nativeCalls=562 viewsCreated=104 viewsReleased=0')
        value = validate_exit_broker(text)
        self.assertFalse(value['brokerCleanTeardownVerified'])
        self.assertEqual(value['brokerRetainedViewsBeforeVmExit'], 104)
        for bad in (BROKER, text.replace('controllerOpen=denied', 'controllerOpen=allowed'),
                    text.replace('nativePortOpen=denied', 'nativePortOpen=allowed'),
                    text.replace('activeLeases=0', 'activeLeases=1'),
                    text.replace('viewsReleased=0', 'viewsReleased=1'),
                    text.replace('retainedViews=104', 'retainedViews=0')):
            with self.assertRaises(ValueError): validate_exit_broker(bad)

    def test_all_foreground_input_disk_and_cpu_store_options_are_rejected(self):
        source = dict(command=['harness', '--runtime-workload', 'init', '--driver-cpu-slots', '128',
            '--driver-fence-slots', '128'], sourceDiskAttached=False, guiFlagsAbsent=True, inputFlagsAbsent=True)
        validate_background_wait_controls(source, BROKER, [])
        for flag in FORBIDDEN:
            bad = copy.deepcopy(source); bad['command'].append(flag)
            with self.subTest(flag=flag), self.assertRaises(ValueError): validate_background_wait_controls(bad, BROKER, [])
        for row in ({'nativeInputChannelOpened': False}, {'nativeSharedTexturePresented': False}):
            with self.assertRaises(ValueError): validate_background_wait_controls(source, BROKER, [row])
        with self.assertRaises(ValueError): validate_background_wait_controls(source, '', [])


if __name__ == '__main__':
    unittest.main()
