"""Synthetic negative evidence controls; these fixtures never establish GPU work."""
import copy
import unittest
from test_vulkan_memory_roundtrip_evidence import log_fixture, native_fixture, REQUIRED_NATIVE_COUNTERS, REQUIRED_VM_COUNTERS
from vulkan_typed_buffer_evidence import validate_fd_control, validate_typed_metadata, validate_partial_creator_refusal


def typed_fixture():
    cleanup, old = native_fixture()
    generated = dict(nativeGpuBufferMetadataGenerated=True, bytes=65536, flags=0, allocationBytes=65536,
        allocationAlignment=65536, runtimeBytes=280, adapterNtstatus=0, deviceNtstatus=0, queryNtstatus=0,
        openNtstatus=0, resourceDestroyedNtstatus=0, deviceDestroyedNtstatus=0, adapterClosedNtstatus=0,
        sharedHandleClosed=True, prototypeReleased=True, reportedUsageChecked=True, privateBytesExported=False)
    applied = dict(nativeGpuBufferMetadataApplied=True, bytes=65536, flags=0, guestRuntimeBytes=264,
        nativeRuntimeBytes=280, guestRuntimePreserved=True, driverPrivateDataReused=True, ntstatus=0)
    rows = []
    for i in range(2):
        probe = copy.deepcopy(old[i]); probe['runtimeBytes'] = 280
        opened = copy.deepcopy(old[2+i]); opened['runtimeBytes'] = 280
        rows.extend([copy.deepcopy(generated), copy.deepcopy(applied), probe, opened, old[4+i]])
        rows.extend([dict(nativeResourceInfoQueried=True, guestRuntimeBytes=264, hostRuntimeBytes=280,
            driverBytes=586, resourceDriverBytes=0, allocations=1)] * 2)
        rows.append(dict(nativeResourceFdImported=True, allocations=1, guestRuntimeBytes=264, driverBytes=586, rawHandleExposed=False))
    return cleanup, rows


def fd_log(control):
    text = log_fixture()
    anchor = 'VULKAN_BRIDGE_RESULT stage=roundtrip-import-memory result=0'
    for i in range(2):
        marker = (f'VULKAN_MEMORY_ROUNDTRIP_IMPORT_FD round={i} descriptorZero=true movedWhileCallerOwned=true' if control == 'zero' else
            f'VULKAN_MEMORY_ROUNDTRIP_FD_PRESSURE round={i} result=-1 callerRetained=true outputNull=true limitRestored=true retry=true')
        position = text.index(anchor, 0 if i == 0 else text.index('VULKAN_MEMORY_ROUNDTRIP_ROUND round=0'))
        text = text[:position] + marker + '\n' + text[position:]
    return text


class TypedBufferEvidence(unittest.TestCase):
    def test_one_good_round_cannot_hide_a_later_creator_failure(self):
        lines = log_fixture().splitlines()
        end = next(i for i, row in enumerate(lines) if row.startswith('VULKAN_MEMORY_ROUNDTRIP_ROUND round=0 '))
        lines = lines[:end+1] + ['VULKAN_BRIDGE_RESULT stage=roundtrip-creator-buffer result=0',
            'VULKAN_BRIDGE_RESULT stage=roundtrip-creator-memory result=-1000072003',
            'VULKAN_BRIDGE_FAIL roundtrip-creator-memory',
            'KERNEL_EVENTFD_EXIT held=0 pending=0 created=3 notified=3 errors=0 released=3 cleanup=true',
            'BROKER_OBJECT_SUMMARY exported=1 published=1 resolved=6 reaped=1 closed=1 retained=0', 'BRIDGE_RUNTIME_EXIT=1']
        lines += ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=1 any=0 kernelEventfd=true'] * 3
        log = '\n'.join(lines) + '\n'
        _, native = native_fixture()
        records = [native[0], native[2], native[4], dict(nativeSharedResourceImportProbe=True, verified=False,
            ntstatus=0, deviceHresult=0, importHresult=-2147467262, runtimeBytes=264, heapHresult=-2147467262,
            heapBytes=0, width=0, height=0, format=0, dimension=0, flags=0, sharedNtHandleClosed=True,
            importedResourceReleased=True, gpuCopy=False, presented=False)]
        cleanup = {name: 0 for name in REQUIRED_NATIVE_COUNTERS}
        cleanup.update(sharedBufferProbes=1, importedSharedBuffers=1, exportedResourceHandles=1,
            closedResourceHandles=1, importedResourceFdObjects=1, failedSharedBufferProbes=0,
            completedVendorGpuVaMaps=150, completedVendorGpuVaWaits=150, failedSharedResourceProbes=1,
            completedNativeSubmissions=8, completedHwQueueSignals=6, completedDeviceExecutionQueries=3,
            driverCleanupVerified=True)
        vm = {name: 0 for name in REQUIRED_VM_COUNTERS}; vm.update(qemuExit=0, qemuForcedStop=False)
        report = dict(disconnectCleanup=cleanup, ownedQemuControl=vm, hostBridgeExit=0)
        result = validate_partial_creator_refusal(log, records, report)
        self.assertEqual(result['payloadRoundsVerified'], 1)
        self.assertFalse(result['wholeRunAccepted'])
        for old, new in (('a39e4f675023a325', '0000000000000000'), ('result=-1000072003', 'result=0'),
            ('BRIDGE_RUNTIME_EXIT=1', 'BRIDGE_RUNTIME_EXIT=0'), ('verifiedWords=16384', 'verifiedWords=1')):
            with self.subTest(old=old), self.assertRaises(ValueError): validate_partial_creator_refusal(log.replace(old, new), records, report)
        bad = copy.deepcopy(report); bad['disconnectCleanup']['failedSharedResourceProbes'] = 0
        with self.assertRaises(ValueError): validate_partial_creator_refusal(log, records, bad)

    def test_native_and_guest_runtime_profiles_remain_distinct(self):
        cleanup, rows = typed_fixture()
        self.assertTrue(validate_typed_metadata(rows, cleanup, 'pass', True)['typedWindowsMetadataVerified'])
        for key, value in (('runtimeBytes', 264), ('sharedHandleClosed', False), ('resourceDestroyedNtstatus', -1),
            ('reportedUsageChecked', False), ('prototypeReleased', False), ('privateBytesExported', True)):
            c, r = typed_fixture(); r[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_typed_metadata(r, c, 'pass', True)
        c, r = typed_fixture(); r[1]['guestRuntimePreserved'] = False
        with self.assertRaises(ValueError): validate_typed_metadata(r, c, 'pass', True)

    def test_missing_duplicate_reordered_and_optout_claims_are_rejected(self):
        cleanup, rows = typed_fixture()
        for bad in (rows[1:], rows + [rows[0]], [rows[1], rows[0]] + rows[2:]):
            with self.assertRaises(ValueError): validate_typed_metadata(bad, cleanup, 'pass', True)
        with self.assertRaises(ValueError): validate_typed_metadata(rows, cleanup, 'pass', False)
        cleanup, rows = native_fixture()
        self.assertFalse(validate_typed_metadata(rows, cleanup, 'pass', False)['typedWindowsMetadataVerified'])
        with self.assertRaises(ValueError): validate_typed_metadata(rows, cleanup, 'pass', True)

    def test_fd_zero_and_exhaustion_need_both_real_import_scopes(self):
        for control in ('zero', 'exhaustion'):
            text = fd_log(control); self.assertEqual(validate_fd_control(text, control)['verifiedRounds'], 2)
            with self.assertRaises(ValueError): validate_fd_control(log_fixture(), control)
            with self.assertRaises(ValueError): validate_fd_control(text, 'none')
            with self.assertRaises(ValueError): validate_fd_control(text.replace('round=1 descriptorZero', 'round=2 descriptorZero') if control == 'zero' else text.replace('callerRetained=true', 'callerRetained=false'), control)
            marker = next(line for line in text.splitlines() if line.startswith('VULKAN_MEMORY_ROUNDTRIP_IMPORT_FD ' if control == 'zero' else 'VULKAN_MEMORY_ROUNDTRIP_FD_PRESSURE '))
            with self.assertRaises(ValueError): validate_fd_control(text.replace(marker + '\n', '') + marker + '\n', control)
        self.assertEqual(validate_fd_control(log_fixture(), 'none')['verifiedRounds'], 0)


if __name__ == '__main__':
    unittest.main()
