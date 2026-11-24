"""Synthetic negative controls; these fixtures do not prove GPU execution."""
import copy
import unittest
from vulkan_memory_fd_evidence import (BASE_STAGES, REQUIRED_NATIVE_COUNTERS, REQUIRED_VM_COUNTERS,
    validate_fd_receipts, validate_fd_cleanup)
from verify_4k_capture import LIVE_COUNTERS


def log_fixture(profile='pass'):
    stages = list(BASE_STAGES)
    if profile != 'buffer-refusal': stages += ['memory-fd-bind'] + ['memory-fd-export'] * (32 if profile == 'pass' else 2)
    if profile == 'pass': stages += ['memory-fd-idle-before-free', 'device-idle']
    lines = ['EVENTFD_CONTRACT_PASS checks=31 published=0 notified=0 gpu=false partialRollback=true foreignOwnerRejected=true',
        'VULKAN_MEMORY_FD_BEGIN bufferBytes=65536 exports=32 cpuWrites=false window=false',
        'VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)']
    lines += ['VULKAN_BRIDGE_RESULT stage=' + stage + ' result=' +
        ('-1000072003' if profile == 'buffer-refusal' and i == len(stages) - 1 else '0') for i, stage in enumerate(stages)]
    if profile == 'pass':
        lines += ['VULKAN_MEMORY_FD_COMPLETE verified=true exports=32 unique=true cloexec=true validAfterMemoryFree=true nvidiaUmdLoaded=true cpuWrites=false window=false',
            'VULKAN_MEMORY_FD_DEVICE_READY verified=true vendor=4318 computeQueue=true nvidiaUmdLoaded=true submissions=0 window=false']
    else:
        lines += ['VULKAN_BRIDGE_FAIL ' + ('memory-fd-allocate' if profile == 'buffer-refusal' else 'Vulkan export did not return a fresh valid descriptor')]
    count = 3 if profile == 'pass' else 1
    exports = 0 if profile == 'buffer-refusal' else 1
    lines += ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=1 any=0 kernelEventfd=true'] * count
    lines += [f'KERNEL_EVENTFD_EXIT held=0 pending=0 created={count} notified={count} errors=0 released={count} cleanup=true',
        f'BROKER_OBJECT_SUMMARY exported={exports} published={exports} resolved=0 reaped={exports} closed={exports} retained=0',
        'BRIDGE_RUNTIME_EXIT=' + ('0' if profile == 'pass' else '1')]
    return '\n'.join(lines) + '\n'


def cleanup_fixture():
    c = {'live' + key: 0 for key in LIVE_COUNTERS}
    c.update({key: 0 for key in REQUIRED_NATIVE_COUNTERS})
    c.update(driverCleanupVerified=True, cleanupFailures=0, vendorCpuSlotLimit=256, vendorCpuMappedByteLimit=100663296,
        reportedGpuUsageLimitBytes=268435456, vendorCpuSlotQuotaRejections=0, completedNativeSubmissions=5,
        completedHwQueueSignals=6, completedDeviceExecutionQueries=3, peakVendorCpuBytes=25341952,
        peakReportedGpuUsageBytes=40448000, cpuStoreTestRequested=False, completedCpuStoreTests=0,
        asynchronousSubmissionOptIn=True, acceptedAsyncSubmissions=5, pendingNativeSubmissions=0, peakPendingNativeSubmissions=1,
        nativeHwQueueSignalAttempts=6, acceptedHwQueueSignals=6, failedHwQueueSignals=0, pendingHwQueueSignals=0,
        peakPendingHwQueueSignals=1, sharedBufferProbes=1, importedSharedBuffers=1, failedSharedBufferProbes=0,
        exportedResourceHandles=1, closedResourceHandles=1, importedResourceFdObjects=0)
    vm = dict(ownedQemuExited=True, qemuExit=0, qemuForcedStop=False, fenceControlFailed=False,
        fenceApertureSlots=128, allocationApertureSlots=256, fenceMappingsCreated=92, fenceUnmapAcknowledgements=92,
        allocationMappingsCreated=119, allocationUnmapAcknowledgements=119,
        allocationSlotMappingsCreated=141, allocationSlotUnmapAcknowledgements=141)
    vm.update({key: 0 for key in REQUIRED_VM_COUNTERS})
    accepted = [dict(nativeAsyncSubmissionAccepted=True, index=i, bytes=4096, privateBytes=1984, target=i, observed=0) for i in range(1, 6)]
    retired = [dict(nativeAsyncSubmissionRetired=True, index=i, bytes=4096, privateBytes=1984, target=i, observed=i) for i in range(1, 6)]
    returned = [dict(nativeHwQueueSignalReturned=True, flags=4, queues=1, target=i, observed=0, ntstatus=0) for i in range(1, 7)]
    signals = [dict(nativeHwQueueSignalAccepted=True, index=i, flags=4, queues=1, target=i, observed=0,
        noGpuAccess=True, cpuValueWrittenByBridge=False) for i in range(1, 7)]
    done = [dict(nativeHwQueueSignalRetired=True, index=i, flags=4, queues=1, target=i, observed=i,
        noGpuAccess=True, cpuValueWrittenByBridge=False) for i in range(1, 7)]
    report = dict(hostBridgeExit=0, disconnectCleanup=c, ownedQemuControl=vm, diagnosticCpuStoresRequested=False,
        guestCommandEnqueues=[dict(bytes=4096, privateBytes=1984, target=i, observedAtReturn=0) for i in range(1, 6)],
        nativeAsyncSubmissionAcceptances=accepted, nativeAsyncSubmissionRetirements=retired,
        guestHwQueueSignals=[dict(flags=4, queues=1, target=i, observedAtReturn=0) for i in range(1, 7)],
        nativeHwQueueSignalReturns=returned, nativeHwQueueSignalAcceptances=signals, nativeHwQueueSignalRetirements=done)
    buffer = dict(nativeSharedBufferImportProbe=True, verified=True, ntstatus=0, deviceHresult=0, importHresult=0,
        runtimeBytes=264, dimension=1, alignment=65536, width=65536, height=1, layers=1, levels=1, format=0,
        samples=1, quality=0, layout=1, flags=0, sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)
    return report, copy.deepcopy([c, vm] + accepted + retired + returned + signals + done + [buffer])


class MemoryFdEvidence(unittest.TestCase):
    def test_scopes_are_distinct(self):
        for profile in ('pass', 'bad-export', 'buffer-refusal'):
            proof = validate_fd_receipts(log_fixture(profile), profile)
            self.assertEqual(proof['descriptorsVerified'], 32 if profile == 'pass' else 0)
            self.assertFalse(proof['gpuPayloadContentsVerified'])
            self.assertFalse(proof['dailyUseAccepted'])
        for profile in ('bad-export', 'buffer-refusal', 'unknown'):
            with self.assertRaises(ValueError): validate_fd_receipts(log_fixture(), profile)

    def test_partial_duplicate_and_failed_api_are_rejected(self):
        good = log_fixture()
        row = 'VULKAN_BRIDGE_RESULT stage=memory-fd-export result=0\n'
        for bad in (good.replace(row, '', 1), good + row, good.replace(row, row.replace('result=0', 'result=-1'), 1)):
            with self.subTest(bad=bad), self.assertRaises(ValueError): validate_fd_receipts(bad, 'pass')

    def test_lifetime_cloexec_identity_and_hardware_claims_are_required(self):
        good = log_fixture()
        for token in ('unique=true', 'cloexec=true', 'validAfterMemoryFree=true', 'nvidiaUmdLoaded=true', 'cpuWrites=false', 'window=false'):
            with self.subTest(token=token), self.assertRaises(ValueError):
                validate_fd_receipts(good.replace(token, token + 'BROKEN'), 'pass')

    def test_wrong_boundary_or_false_acceptance_of_controls_is_rejected(self):
        for profile in ('bad-export', 'buffer-refusal'):
            good = log_fixture(profile)
            for bad in (good.replace('BRIDGE_RUNTIME_EXIT=1', 'BRIDGE_RUNTIME_EXIT=0'),
                        good.replace('VULKAN_BRIDGE_FAIL ', 'OTHER_FAIL '), good + 'VULKAN_MEMORY_FD_COMPLETE verified=true\n'):
                with self.subTest(profile=profile), self.assertRaises(ValueError): validate_fd_receipts(bad, profile)

    def test_kernel_and_broker_debt_are_rejected(self):
        good = log_fixture()
        for a, b in (('pending=0', 'pending=1'), ('released=3', 'released=2'), ('retained=0', 'retained=1'),
                     ('closed=1', 'closed=0'), ('resolved=0', 'resolved=1')):
            with self.subTest(a=a), self.assertRaises(ValueError): validate_fd_receipts(good.replace(a, b), 'pass')

    def test_native_lifetime_proof_has_no_application_shader(self):
        d, rows = cleanup_fixture()
        proof = validate_fd_cleanup(d, rows, 'pass')
        self.assertEqual(proof['nativeInitializationSubmissions'], 5)
        self.assertEqual(proof['applicationShaderSubmissions'], 0)
        self.assertFalse(proof['guestVulkanFdImportVerified'])

    def test_native_failure_retention_and_quota_increase_are_rejected(self):
        for key, value in (('liveDevices', 1), ('failedSharedBufferProbes', 1), ('gpuReservationsReleasedAfterVmExit', 1),
            ('reportedGpuUsageLimitBytes', 536870912), ('vendorCpuMappedByteLimit', 201326592), ('pendingNativeSubmissions', 1),
            ('completedNativeSubmissions', 6), ('importedResourceFdObjects', 1), ('closedResourceHandles', 0)):
            d, rows = cleanup_fixture(); d['disconnectCleanup'][key] = value; rows[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_fd_cleanup(d, rows, 'pass')

    def test_mapping_debt_or_forced_vm_is_rejected(self):
        for key, value in (('allocationUnmapAcknowledgements', 118), ('qemuForcedStop', True), ('qemuExit', 1),
            ('allocationApertureSlots', 512), ('fenceMappingsReleasedAfterVmExit', 1)):
            d, rows = cleanup_fixture(); d['ownedQemuControl'][key] = value; rows[1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_fd_cleanup(d, rows, 'pass')

    def test_omitted_debt_counters_are_rejected(self):
        for section, index, key in (('disconnectCleanup', 0, 'failedNativeSubmissions'),
                ('ownedQemuControl', 1, 'allocationMappingsReleasedAfterVmExit')):
            d, rows = cleanup_fixture(); del d[section][key]; del rows[index][key]
            with self.subTest(key=key), self.assertRaises(ValueError): validate_fd_cleanup(d, rows, 'pass')

    def test_forged_raw_report_or_cpu_signal_is_rejected(self):
        d, rows = cleanup_fixture(); d['disconnectCleanup']['peakVendorCpuBytes'] += 1
        with self.assertRaises(ValueError): validate_fd_cleanup(d, rows, 'pass')
        d, rows = cleanup_fixture(); d['nativeHwQueueSignalRetirements'][0]['cpuValueWrittenByBridge'] = True
        with self.assertRaises(ValueError): validate_fd_cleanup(d, rows, 'pass')

    def test_wrong_or_unreleased_buffer_is_rejected(self):
        for key, value in (('width', 33554432), ('dimension', 3), ('flags', 4), ('sharedNtHandleClosed', False),
            ('importedResourceReleased', False), ('importHresult', -1), ('verified', 1)):
            d, rows = cleanup_fixture(); rows[-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_fd_cleanup(d, rows, 'pass')


if __name__ == '__main__': unittest.main()
