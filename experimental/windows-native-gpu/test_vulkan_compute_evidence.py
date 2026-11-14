"""Synthetic receipt tests only; these fixtures never establish GPU execution."""
import copy
import unittest
from vulkan_compute_evidence import (BASE_STAGES, ROUND_STAGES, EXPECTED_HASHES,
    validate_compute_receipts, validate_compute_controls, validate_compute_cleanup, validate_refusal_retention)
from verify_4k_capture import LIVE_COUNTERS
from test_broker_runtime_evidence import BROKER
from background_shader_evidence import FORBIDDEN


def log_fixture(profile='pass'):
    stages = list(BASE_STAGES + ROUND_STAGES)
    if profile == 'pass': stages += ['compute-reset-command'] + list(ROUND_STAGES) + ['device-idle']
    results = ['VULKAN_BRIDGE_RESULT stage=' + stage + ' result=' +
        ('-5' if profile == 'slot-refusal' and index == len(stages) - 1 else '0') for index, stage in enumerate(stages)]
    lines = ['EVENTFD_CONTRACT_PASS checks=31 published=0 notified=0 gpu=false partialRollback=true foreignOwnerRejected=true',
        'VULKAN_COMPUTE_BEGIN width=3840 height=2160 words=8294400 rounds=2 cpuStorageWrites=false window=false',
        'VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)',
        'VULKAN_COMPUTE_MEMORY role=output type=3 flags=1 bytes=33177600 cpuInitialized=false',
        'VULKAN_COMPUTE_MEMORY role=readback type=1 flags=6 bytes=33177600 cpuInitialized=false'] + results
    if profile == 'pass':
        lines += [f'VULKAN_COMPUTE_ROUND round={i} verifiedWords=8294400 fnv1a={value} referenceFnv1a={value} coherent=true gpuOnlyStorage=true cpuStorageWrites=false'
                  for i, value in enumerate(EXPECTED_HASHES)]
        lines += ['VULKAN_COMPUTE_COMPLETE verified=true width=3840 height=2160 rounds=2 wordsPerRound=8294400 gpuOnlyStorage=true gpuTransferReadback=true nvidiaUmdLoaded=true window=false cpuStorageWrites=false',
            'VULKAN_COMPUTE_DEVICE_READY verified=true vendor=4318 computeQueue=true nvidiaUmdLoaded=true computeSubmissions=2 window=false']
    elif profile == 'slot-refusal': lines += ['VULKAN_BRIDGE_FAIL compute-map-readback']
    else: lines += ['VULKAN_COMPUTE_MISMATCH round=0 x=3839 y=2159 actual=0 expected=4280660732',
        'VULKAN_BRIDGE_FAIL compute readback differs from integer oracle']
    count = 4 if profile == 'pass' else 1 if profile == 'slot-refusal' else 2
    lines += ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=1 any=0 kernelEventfd=true'] * count
    lines += [f'KERNEL_EVENTFD_EXIT held=0 pending=0 created={count} notified={count} errors=0 released={count} cleanup=true',
        'BRIDGE_RUNTIME_EXIT=' + ('0' if profile == 'pass' else '1')]
    return '\n'.join(lines) + '\n'


def cleanup_fixture():
    cleanup = {'live' + key: 0 for key in LIVE_COUNTERS}
    cleanup.update(driverCleanupVerified=True, cleanupFailures=0, vendorCpuSlotLimit=256,
        vendorCpuMappedByteLimit=100663296, reportedGpuUsageLimitBytes=268435456, vendorCpuSlotQuotaRejections=0,
        peakVendorCpuBytes=61452288, peakReportedGpuUsageBytes=110112768, cpuStoreTestRequested=False,
        completedCpuStoreTests=0, asynchronousSubmissionOptIn=True, acceptedAsyncSubmissions=6,
        completedNativeSubmissions=6, pendingNativeSubmissions=0, peakPendingNativeSubmissions=1,
        nativeHwQueueSignalAttempts=2, acceptedHwQueueSignals=2, completedHwQueueSignals=2,
        failedHwQueueSignals=0, pendingHwQueueSignals=0, peakPendingHwQueueSignals=1, completedDeviceExecutionQueries=4,
        gpuReservationsReleasedAfterVmExit=0)
    vm = dict(ownedQemuExited=True, qemuExit=0, qemuForcedStop=False, fenceControlFailed=False,
        fenceApertureSlots=128, allocationApertureSlots=256, fenceMappingsCreated=92, fenceUnmapAcknowledgements=92,
        allocationMappingsCreated=126, allocationUnmapAcknowledgements=126,
        allocationSlotMappingsCreated=179, allocationSlotUnmapAcknowledgements=179)
    accepted = [dict(nativeAsyncSubmissionAccepted=True, index=i, bytes=4096, privateBytes=1984, target=i, observed=0) for i in range(1, 7)]
    retired = [dict(nativeAsyncSubmissionRetired=True, index=i, bytes=4096, privateBytes=1984, target=i, observed=i) for i in range(1, 7)]
    returned = [dict(nativeHwQueueSignalReturned=True, flags=4, queues=1, target=i, observed=0, ntstatus=0) for i in (1, 2)]
    signals = [dict(nativeHwQueueSignalAccepted=True, index=i, flags=4, queues=1, target=i, observed=0,
        noGpuAccess=True, cpuValueWrittenByBridge=False) for i in (1, 2)]
    done = [dict(nativeHwQueueSignalRetired=True, index=i, flags=4, queues=1, target=i, observed=i,
        noGpuAccess=True, cpuValueWrittenByBridge=False) for i in (1, 2)]
    report = dict(hostBridgeExit=0, disconnectCleanup=cleanup, ownedQemuControl=vm, diagnosticCpuStoresRequested=False,
        guestCommandEnqueues=[dict(bytes=4096, privateBytes=1984, target=i, observedAtReturn=0) for i in range(1, 7)],
        nativeAsyncSubmissionAcceptances=accepted, nativeAsyncSubmissionRetirements=retired,
        guestHwQueueSignals=[dict(flags=4, queues=1, target=i, observedAtReturn=0) for i in (1, 2)],
        nativeHwQueueSignalReturns=returned, nativeHwQueueSignalAcceptances=signals, nativeHwQueueSignalRetirements=done)
    return report, copy.deepcopy([cleanup, vm] + accepted + retired + returned + signals + done)


class ComputeReceipts(unittest.TestCase):
    def test_positive_and_retained_controls_have_distinct_scopes(self):
        for profile, rounds in (('pass', 2), ('slot-refusal', 0), ('corrupt-last', 0)):
            value = validate_compute_receipts(log_fixture(profile), profile)
            self.assertEqual(value['shaderRoundsVerified'], rounds)
            self.assertFalse(value['windowPresentationVerified'])
            self.assertFalse(value['dailyUseAccepted'])
        for profile in ('slot-refusal', 'corrupt-last', 'unknown'):
            with self.assertRaises(ValueError): validate_compute_receipts(log_fixture(), profile)

    def test_missing_duplicate_failed_or_reordered_api_call_is_rejected(self):
        text = log_fixture(); line = 'VULKAN_BRIDGE_RESULT stage=compute-wait-fence result=0\n'
        for bad in (text.replace(line, '', 1), text + line, text.replace('stage=compute-wait-fence result=0', 'stage=compute-wait-fence result=2'),
                    text.replace('stage=compute-queue-submit', 'stage=SWAP').replace('stage=compute-wait-fence', 'stage=compute-queue-submit').replace('stage=SWAP', 'stage=compute-wait-fence')):
            with self.assertRaises(ValueError): validate_compute_receipts(bad, 'pass')

    def test_wrong_or_repeated_hash_count_and_duplicate_rounds_are_rejected(self):
        text = log_fixture()
        for old, new in (('verifiedWords=8294400', 'verifiedWords=8294399'), (EXPECTED_HASHES[1], EXPECTED_HASHES[0]),
            ('gpuOnlyStorage=true', 'gpuOnlyStorage=false'), ('cpuStorageWrites=false', 'cpuStorageWrites=true'),
            ('nvidiaUmdLoaded=true', 'nvidiaUmdLoaded=false'), ('vendor=4318', 'vendor=65541')):
            with self.subTest(old=old), self.assertRaises(ValueError): validate_compute_receipts(text.replace(old, new), 'pass')
        round_line = next(line for line in text.splitlines() if line.startswith('VULKAN_COMPUTE_ROUND '))
        with self.assertRaises(ValueError): validate_compute_receipts(text + round_line + '\n', 'pass')

    def test_cpu_visible_storage_and_wrong_readback_size_or_flags_are_rejected(self):
        text = log_fixture()
        for old, new in (('type=3 flags=1', 'type=3 flags=3'), ('type=1 flags=6', 'type=1 flags=2'),
                         ('bytes=33177600', 'bytes=1048576'), ('cpuInitialized=false', 'cpuInitialized=true')):
            with self.assertRaises(ValueError): validate_compute_receipts(text.replace(old, new), 'pass')

    def test_last_word_control_must_fail_at_the_actual_final_word(self):
        text = log_fixture('corrupt-last')
        for old, new in (('x=3839', 'x=0'), ('y=2159', 'y=0'), ('actual=0', 'actual=4280660732'),
                         ('BRIDGE_RUNTIME_EXIT=1', 'BRIDGE_RUNTIME_EXIT=0')):
            with self.assertRaises(ValueError): validate_compute_receipts(text.replace(old, new), 'corrupt-last')
        with self.assertRaises(ValueError): validate_compute_receipts(text, 'pass')

    def test_missing_notifications_bad_kernel_contract_and_mixed_success_are_rejected(self):
        text = log_fixture()
        for bad in (text.replace('notified=4', 'notified=3'), text.replace('checks=31', 'checks=0'),
                    text + 'BRIDGE_RUNTIME_EXIT=0\n', text + 'EVENTFD_CONTRACT_FAIL line=1\n'):
            with self.assertRaises(ValueError): validate_compute_receipts(bad, 'pass')
        control = log_fixture('slot-refusal') + next(line for line in text.splitlines() if line.startswith('VULKAN_COMPUTE_COMPLETE ')) + '\n'
        with self.assertRaises(ValueError): validate_compute_receipts(control, 'slot-refusal')


class ComputeCleanup(unittest.TestCase):
    def test_refusal_recovery_debt_must_remain_visible(self):
        report, records = cleanup_fixture()
        report['disconnectCleanup'].update(vendorCpuSlotLimit=128, vendorCpuSlotQuotaRejections=1,
            gpuReservationsReleasedAfterVmExit=4)
        report['ownedQemuControl'].update(allocationApertureSlots=128, liveFenceMappings=0,
            liveAllocationMappings=0, liveAllocationMappedBytes=0, liveAllocationSlotMappings=0)
        records[0] = copy.deepcopy(report['disconnectCleanup']); records[1] = copy.deepcopy(report['ownedQemuControl'])
        records.append(dict(nativeCpuSlotAdmissionRejected=True, requestedBytes=33554432, liveChunks=98,
            slotLimit=128, largestFreeRunSlots=30, mappedBytes=26456064, runtimeStopped=False, cpuEnabled=True))
        value = validate_refusal_retention(report, records)
        self.assertFalse(value['liveReservationRecoveryVerified'])
        self.assertFalse(value['fullComputeCleanupAccepted'])
        for debt in (0, False, 3):
            bad, raw = copy.deepcopy((report, records)); bad['disconnectCleanup']['gpuReservationsReleasedAfterVmExit'] = debt
            raw[0] = copy.deepcopy(bad['disconnectCleanup'])
            with self.assertRaises(ValueError): validate_refusal_retention(bad, raw)

    def test_geometry_guard_limits_and_native_receipts_are_verified(self):
        report, records = cleanup_fixture()
        value = validate_compute_cleanup(report, records, 256, 'pass')
        self.assertEqual(value['cpuMappedByteLimit'], 100663296)
        self.assertTrue(value['allNativeMappingsRetiredBeforeVmExit'])
        with self.assertRaises(ValueError): validate_compute_cleanup(report, records, 128, 'pass')

    def test_native_vm_exit_debt_failed_work_forced_exit_and_missing_ack_are_rejected(self):
        for section, key, value in (('disconnectCleanup', 'gpuReservationsReleasedAfterVmExit', 4),
            ('disconnectCleanup', 'failedNativeSubmissions', 1), ('disconnectCleanup', 'pendingNativeSubmissions', 1),
            ('ownedQemuControl', 'allocationUnmapAcknowledgements', 125), ('ownedQemuControl', 'qemuForcedStop', True)):
            report, records = cleanup_fixture(); report[section][key] = value
            records[0 if section == 'disconnectCleanup' else 1] = copy.deepcopy(report[section])
            with self.subTest(key=key), self.assertRaises(ValueError): validate_compute_cleanup(report, records, 256, 'pass')

    def test_cpu_writes_wrong_limits_and_boolean_counter_types_are_rejected(self):
        for key, value in (('completedCpuStoreTests', 1), ('cpuStoreTestRequested', True),
                           ('vendorCpuMappedByteLimit', 201326592), ('reportedGpuUsageLimitBytes', 536870912),
                           ('cleanupFailures', False), ('peakVendorCpuBytes', True), ('vendorCpuSlotQuotaRejections', False)):
            report, records = cleanup_fixture(); report['disconnectCleanup'][key] = value; records[0] = copy.deepcopy(report['disconnectCleanup'])
            with self.subTest(key=key), self.assertRaises(ValueError): validate_compute_cleanup(report, records, 256, 'pass')

    def test_native_gpu_receipt_loss_fabrication_and_unretired_signal_are_rejected(self):
        report, records = cleanup_fixture()
        for bad in (records[:-1], records + records[-1:], records[1:]):
            with self.assertRaises(ValueError): validate_compute_cleanup(report, bad, 256, 'pass')
        report, records = cleanup_fixture(); report['nativeHwQueueSignalRetirements'][0]['cpuValueWrittenByBridge'] = True
        with self.assertRaises(ValueError): validate_compute_cleanup(report, records, 256, 'pass')

    def test_background_controls_reject_all_input_presentation_disk_and_cpu_store_flags(self):
        source = dict(command=['harness', '--runtime-workload', 'init', '--driver-cpu-slots', '256', '--driver-fence-slots', '128'],
            sourceDiskAttached=False, guiFlagsAbsent=True, inputFlagsAbsent=True)
        validate_compute_controls(source, BROKER, [], 256)
        for flag in FORBIDDEN:
            bad = copy.deepcopy(source); bad['command'].append(flag)
            with self.subTest(flag=flag), self.assertRaises(ValueError): validate_compute_controls(bad, BROKER, [], 256)
        with self.assertRaises(ValueError): validate_compute_controls(source, BROKER, [{'nativeSdlGraphicsChannelOpened': False}], 256)


if __name__ == '__main__':
    unittest.main()
