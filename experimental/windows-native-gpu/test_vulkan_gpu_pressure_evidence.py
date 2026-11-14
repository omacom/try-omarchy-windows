"""Synthetic rejection controls only; these fixtures never establish GPU execution."""
import copy
import unittest
from test_vulkan_memory_roundtrip_evidence import log_fixture
from test_vulkan_typed_buffer_evidence import typed_fixture
from test_vulkan_compute_evidence import cleanup_fixture
from vulkan_gpu_pressure_evidence import (SMALL, LARGE, LIMIT, FILL, validate_pressure_receipts,
    validate_admission_queries, validate_pressure_metadata, validate_pressure_cleanup)


def pressure_log():
    text = log_fixture()
    lines = ['VULKAN_GPU_MEMORY_PRESSURE_BEGIN allocationBytes=67108864 maxAttempts=6 cpuWrites=false hostGuardChanged=false']
    def calls(stages):
        lines.extend('VULKAN_BRIDGE_RESULT stage=' + stage + ' result=0' for stage in stages)
    for i in range(2):
        calls(('pressure-buffer', 'pressure-bind') + FILL)
        lines.append(f'VULKAN_GPU_MEMORY_PRESSURE_HELD index={i} allocationBytes=67108864 gpuFillRetired=true')
    calls(('pressure-buffer',))
    lines.append('VULKAN_GPU_MEMORY_PRESSURE_REFUSED heldBuffers=2 result=-2 allocationBytes=67108864 outputNull=true callerFdRetained=true')
    calls(('pressure-device-idle', 'pressure-retry-memory', 'pressure-retry-bind') + FILL)
    lines.append('VULKAN_GPU_MEMORY_PRESSURE_RECOVERED freedBuffers=2 retrySameBuffer=true retrySameSize=true retrySameMemoryType=true retryGpuFillRetired=true callerFdRetained=true')
    anchor = 'VULKAN_BRIDGE_RESULT stage=roundtrip-import-buffer result=0'
    text = text.replace(anchor, '\n'.join(lines) + '\n' + anchor, 1)
    text = 'VULKAN_GPU_MEMORY_PRESSURE_CAPACITY version=2 reservations=16 cpuByteGuardUnchanged=true gpuByteGuardUnchanged=true renderApi=Vulkan\n' + text
    text = text.replace('created=6 notified=6 errors=0 released=6', 'created=10 notified=10 errors=0 released=10')
    text = text.replace('exported=2 published=2 resolved=12 reaped=2 closed=2', 'exported=5 published=5 resolved=12 reaped=5 closed=5')
    return text + 'LINUX_BRIDGE asyncCpuFenceWaitRegistered=true\n' * 4


def admission_fixture():
    cleanup, typed = typed_fixture()
    records = []
    for i, width in enumerate((SMALL, LARGE, LARGE, LARGE, LARGE, SMALL)):
        refused = i == 3
        records.append(dict(nativeGpuBufferAdmissionQueried=True, bytes=width, flags=0, usageKnown=True,
            usageBytes=244264960 if refused else 40382464, limitBytes=LIMIT, accepted=not refused,
            ntstatus=-1073741801 if refused else 0, advisoryOnly=True, capacityReserved=False, gpuObjectCreated=False))
        if refused: continue
        for marker in ('nativeGpuBufferMetadataGenerated', 'nativeGpuBufferMetadataApplied', 'nativeSharedBufferImportProbe'):
            row = copy.deepcopy(next(r for r in typed if marker in r))
            if marker == 'nativeSharedBufferImportProbe': row['width'] = width
            else: row['bytes'] = width
            if marker == 'nativeGpuBufferMetadataGenerated': row['allocationBytes'] = width
            records.append(row)
    records.append(dict(nativeReportedGpuUsageRejected=True, usageBytes=244264960, additionalBytes=LARGE,
        limitBytes=LIMIT, localUsageBytes=150605824, nonlocalUsageBytes=93659136,
        currentProcessOnly=True, policyChanged=False, hardAllocationQuota=False))
    records.extend(r for r in typed if any(k in r for k in ('nativeResourceInfoQueried', 'nativeResourceFdImported',
        'nativeResourceFdOpenReturned', 'nativeImportedResourceGpuVaMapped')))
    cleanup.update(nativeGpuBufferAdmissionQueries=6, nativeGpuBufferAdmissionRefusals=1, failedNativeGpuBufferAdmissionQueries=0,
        nativeGpuBufferMetadataMemoryRefusals=0, sharedBufferProbes=5, importedSharedBuffers=5,
        exportedResourceHandles=5, closedResourceHandles=5, completedNativeSubmissions=13,
        completedHwQueueSignals=20, completedDeviceExecutionQueries=10)
    return records, cleanup


def pressure_cleanup_fixture():
    report, _ = cleanup_fixture()
    c = report['disconnectCleanup']
    c.update(vendorGpuMappedByteLimit=LIMIT, standaloneGpuMappedByteLimit=LIMIT,
        gpuReservationCountLimit=16, gpuReservationByteLimit=17179869184, completedNativeSubmissions=13,
        acceptedAsyncSubmissions=13, nativeHwQueueSignalAttempts=20, acceptedHwQueueSignals=20,
        completedHwQueueSignals=20, completedDeviceExecutionQueries=10)
    for key, count in (('guestCommandEnqueues', 13), ('nativeAsyncSubmissionAcceptances', 13), ('nativeAsyncSubmissionRetirements', 13),
        ('guestHwQueueSignals', 20), ('nativeHwQueueSignalReturns', 20), ('nativeHwQueueSignalAcceptances', 20), ('nativeHwQueueSignalRetirements', 20)):
        template = report[key][0]
        report[key] = []
        for i in range(1, count + 1):
            row = copy.deepcopy(template); row['target'] = i
            if 'index' in row: row['index'] = i
            if 'Retirements' in key: row['observed'] = i
            report[key].append(row)
    records = [c, report['ownedQemuControl']]
    for key in ('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionRetirements', 'nativeHwQueueSignalReturns',
        'nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalRetirements'):
        records.extend(report[key])
    return report, copy.deepcopy(records)


class PressureEvidence(unittest.TestCase):
    def test_full_pressure_lifetime_and_gpu_data_are_required(self):
        text = pressure_log()
        result = validate_pressure_receipts(text)
        self.assertEqual(result['payloadRoundsVerified'], 2)
        self.assertFalse(result['dailyUseAccepted'])
        for old, new in (('stage=pressure-device-idle result=0', 'stage=pressure-device-idle result=-2'),
            ('stage=pressure-retry-memory result=0', 'stage=pressure-retry-memory result=-2'),
            ('retrySameBuffer=true', 'retrySameBuffer=false'), ('callerFdRetained=true', 'callerFdRetained=false'),
            ('outputNull=true', 'outputNull=false'), ('result=-2 allocationBytes', 'result=-1 allocationBytes'),
            ('gpuFillRetired=true', 'gpuFillRetired=false'), ('verifiedWords=16384', 'verifiedWords=1'),
            ('62b2ec7741bea325', '0000000000000000'), ('BRIDGE_RUNTIME_EXIT=0', 'BRIDGE_RUNTIME_EXIT=1'),
            ('notified=10', 'notified=9'), ('retained=0', 'retained=1')):
            with self.subTest(old=old), self.assertRaises(ValueError): validate_pressure_receipts(text.replace(old, new))
        for extra in ('D3D12: Removing Device.\n', 'VULKAN_GPU_MEMORY_PRESSURE_REFUSED heldBuffers=2\n'):
            with self.assertRaises(ValueError): validate_pressure_receipts(text + extra)
        anchor = 'VULKAN_GPU_MEMORY_PRESSURE_RECOVERED '
        line = next(s for s in text.splitlines() if s.startswith(anchor))
        with self.assertRaises(ValueError): validate_pressure_receipts(text.replace(line + '\n', '') + line + '\n')

    def test_unknown_usage_changed_limits_and_reserved_authority_are_rejected(self):
        records, cleanup = admission_fixture()
        self.assertEqual(validate_admission_queries(records, cleanup, True)['knownPreflightRefusals'], 1)
        for index, key, value in ((0, 'usageKnown', False), (0, 'limitBytes', LIMIT * 2), (0, 'capacityReserved', True),
            (0, 'gpuObjectCreated', True), (0, 'advisoryOnly', False), (12, 'ntstatus', 0)):
            bad = copy.deepcopy(records)
            # Select the rejected query directly, independent of inserted metadata.
            row = next(r for r in bad if r.get('accepted') is False) if index == 12 else bad[index]
            row[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_admission_queries(bad, cleanup, True)
        for key in ('nativeGpuBufferAdmissionRefusals', 'failedNativeGpuBufferAdmissionQueries', 'nativeGpuBufferMetadataMemoryRefusals'):
            bad = dict(cleanup); bad[key] += 1
            with self.assertRaises(ValueError): validate_admission_queries(records, bad, True)

    def test_refused_allocation_cannot_publish_metadata(self):
        records, cleanup = admission_fixture()
        validate_pressure_metadata(records, cleanup)
        query = next(i for i, r in enumerate(records) if r.get('accepted') is False)
        for bad in (records[:query+1] + [copy.deepcopy(records[1])] + records[query+1:], records + [records[1]]):
            with self.assertRaises(ValueError): validate_pressure_metadata(bad, cleanup)
        for marker, key, value in (('nativeGpuBufferMetadataGenerated', 'sharedHandleClosed', False),
            ('nativeGpuBufferMetadataApplied', 'guestRuntimeBytes', 280), ('nativeSharedBufferImportProbe', 'runtimeBytes', 264),
            ('nativeResourceFdImported', 'rawHandleExposed', True), ('nativeImportedResourceGpuVaMapped', 'driverProtection', 0)):
            bad = copy.deepcopy(records); next(r for r in bad if marker in r)[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_pressure_metadata(bad, cleanup)

    def test_all_twenty_signals_and_live_cleanup_are_required(self):
        report, rows = pressure_cleanup_fixture()
        self.assertEqual(validate_pressure_cleanup(report, rows)['correlatedHardwareQueueSignals'], 20)
        for key, value in (('gpuReservationsReleasedAfterVmExit', 1), ('failedNativeSubmissions', 1),
            ('liveVendorCpuBytes', 4096), ('vendorCpuMappedByteLimit', 200000000), ('completedHwQueueSignals', 19)):
            bad = copy.deepcopy(report); bad['disconnectCleanup'][key] = value
            badrows = copy.deepcopy(rows); badrows[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_pressure_cleanup(bad, badrows)
        bad = copy.deepcopy(report); bad['guestHwQueueSignals'].pop()
        with self.assertRaises(ValueError): validate_pressure_cleanup(bad, rows)
        bad = copy.deepcopy(report); bad['nativeHwQueueSignalRetirements'][-1]['observed'] = 0
        with self.assertRaises(ValueError): validate_pressure_cleanup(bad, rows)
        bad = copy.deepcopy(report); bad['ownedQemuControl']['qemuForcedStop'] = True
        with self.assertRaises(ValueError): validate_pressure_cleanup(bad, rows)


if __name__ == '__main__': unittest.main()
