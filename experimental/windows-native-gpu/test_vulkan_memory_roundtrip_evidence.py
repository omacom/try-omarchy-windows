"""Synthetic parser controls only; these logs are not GPU execution evidence."""
import copy
import unittest
from vulkan_memory_roundtrip_evidence import (INITIAL, ROUND, REQUIRED_NATIVE_COUNTERS, REQUIRED_VM_COUNTERS,
    validate_receipts, validate_native_imports, validate_control_debt)
from vulkan_memory_roundtrip_reference import expected_round


def log_fixture(profile='pass'):
    lines = ['EVENTFD_CONTRACT_PASS checks=31 published=0 notified=0 gpu=false partialRollback=true foreignOwnerRejected=true',
        'VULKAN_MEMORY_ROUNDTRIP_BEGIN bufferBytes=65536 stripes=16 rounds=2 cpuWrites=false window=false',
        'VULKAN_BRIDGE_DEVICE vendor=4318 device=11352 type=2 api=4202831 name=Microsoft Direct3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)']
    lines += ['VULKAN_BRIDGE_RESULT stage=' + stage + ' result=0' for stage in INITIAL]
    for i in range(2 if profile == 'pass' else 1):
        stages = ROUND[:2] if profile == 'early-refusal' else ROUND[:13] if profile == 'map-refusal' else ROUND
        for index, stage in enumerate(stages):
            result = '-1000072003' if profile in ('early-refusal', 'map-refusal') and index == len(stages) - 1 else '0'
            lines += ['VULKAN_BRIDGE_RESULT stage=' + stage + ' result=' + result]
            if stage == 'roundtrip-export':
                lines += [f'VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED round={i} beforeImport=true gpuFillRetired=true']
        if profile == 'pass':
            value = expected_round(i)['fnv1a']
            lines += [f'VULKAN_MEMORY_ROUNDTRIP_ROUND round={i} verifiedWords=16384 fnv1a={value} referenceFnv1a={value} gpuOnlyStorage=true creatorFreed=true importOwnsFd=true gpuTransferReadback=true cpuWrites=false']
    if profile == 'pass':
        lines += ['VULKAN_BRIDGE_RESULT stage=device-idle result=0',
            'VULKAN_MEMORY_ROUNDTRIP_COMPLETE verified=true rounds=2 wordsPerRound=16384 gpuOnlyStorage=true creatorFreedBeforeImport=true gpuTransferReadback=true nvidiaUmdLoaded=true window=false cpuWrites=false',
            'VULKAN_MEMORY_ROUNDTRIP_DEVICE_READY verified=true vendor=4318 computeQueue=true nvidiaUmdLoaded=true submissions=4 window=false']
    else:
        if profile == 'corrupt-last': lines += ['VULKAN_MEMORY_ROUNDTRIP_MISMATCH round=0 word=16383 actual=0 expected=2353232594']
        failure = {'early-refusal': 'roundtrip-creator-memory', 'map-refusal': 'roundtrip-import-memory',
            'corrupt-last': 'imported GPU payload differs from integer oracle'}[profile]
        lines += ['VULKAN_BRIDGE_FAIL ' + failure]
    count = 6 if profile == 'pass' else 3 if profile == 'corrupt-last' else 1
    exports = 2 if profile == 'pass' else 0 if profile == 'early-refusal' else 1
    lines += ['LINUX_BRIDGE asyncCpuFenceWaitRegistered=true count=1 any=0 kernelEventfd=true'] * count
    lines += [f'KERNEL_EVENTFD_EXIT held=0 pending=0 created={count} notified={count} errors=0 released={count} cleanup=true',
        f'BROKER_OBJECT_SUMMARY exported={exports} published={exports} resolved={exports * 6} reaped={exports} closed={exports} retained=0',
        'BRIDGE_RUNTIME_EXIT=' + ('0' if profile == 'pass' else '1')]
    return '\n'.join(lines) + '\n'


def native_fixture():
    c = dict(sharedBufferProbes=2, importedSharedBuffers=2, exportedResourceHandles=2, closedResourceHandles=2,
        importedResourceFdObjects=2, failedSharedBufferProbes=0, completedVendorGpuVaMaps=150, completedVendorGpuVaWaits=150)
    probe = dict(nativeSharedBufferImportProbe=True, verified=True, ntstatus=0, deviceHresult=0, importHresult=0,
        runtimeBytes=264, dimension=1, alignment=65536, width=65536, height=1, layers=1, levels=1, format=0,
        samples=1, quality=0, layout=1, flags=0, sharedNtHandleClosed=True, importedResourceReleased=True, gpuCopy=False, presented=False)
    opened = dict(nativeResourceFdOpenReturned=True, ntstatus=0, hasResource=True, hasAllocation=True,
        hasKeyedMutex=False, hasSync=False, runtimeBytes=264, driverBytes=586, allocationDriverBytes=586, resourceDriverBytes=0)
    rows = [copy.deepcopy(probe), copy.deepcopy(probe), copy.deepcopy(opened), copy.deepcopy(opened)]
    rows += [dict(nativeImportedResourceGpuVaMapped=True, driverProtection=268435465, resourceOpened=True,
        bytes=65536, ntstatus=259, unchanged=True, addressRangeValidated=True, pagingFence=i) for i in (7130, 7136)]
    return c, rows


class RoundtripEvidence(unittest.TestCase):
    def test_success_refusals_and_corruption_have_separate_scopes(self):
        for profile in ('pass', 'map-refusal', 'early-refusal', 'corrupt-last'):
            value = validate_receipts(log_fixture(profile), profile)
            self.assertEqual(value['payloadRoundsVerified'], 2 if profile == 'pass' else 0)
            self.assertFalse(value['dailyUseAccepted'])
            self.assertFalse(value['descriptorClosureDuringLiveApplicationVerified'])
            if profile != 'pass':
                with self.assertRaises(ValueError): validate_receipts(log_fixture(profile), 'pass')

    def test_missing_duplicate_or_failed_api_is_rejected(self):
        good = log_fixture(); row = 'VULKAN_BRIDGE_RESULT stage=roundtrip-import-memory result=0\n'
        for bad in (good.replace(row, '', 1), good + row, good.replace(row, row.replace('result=0', 'result=-1'), 1)):
            with self.assertRaises(ValueError): validate_receipts(bad, 'pass')

    def test_creator_must_be_freed_between_export_and_import(self):
        good = log_fixture(); marker = 'VULKAN_MEMORY_ROUNDTRIP_CREATOR_FREED round=0 beforeImport=true gpuFillRetired=true\n'
        for bad in (good.replace(marker, ''), good.replace(marker, '') + marker, good + marker):
            with self.assertRaises(ValueError): validate_receipts(bad, 'pass')

    def test_independent_hash_full_word_count_and_gpu_provenance_required(self):
        good = log_fixture()
        for old, new in (('a39e4f675023a325', '0000000000000000'), ('verifiedWords=16384', 'verifiedWords=16383'),
            ('gpuOnlyStorage=true', 'gpuOnlyStorage=false'), ('cpuWrites=false', 'cpuWrites=true'),
            ('vendor=4318', 'vendor=1234'), ('nvidiaUmdLoaded=true', 'nvidiaUmdLoaded=false')):
            with self.subTest(old=old), self.assertRaises(ValueError): validate_receipts(good.replace(old, new), 'pass')

    def test_kernel_broker_debt_and_wrong_guest_exit_are_rejected(self):
        for a, b in (('released=6', 'released=5'), ('pending=0', 'pending=1'), ('closed=2', 'closed=1'),
            ('retained=0', 'retained=1'), ('BRIDGE_RUNTIME_EXIT=0', 'BRIDGE_RUNTIME_EXIT=1')):
            with self.subTest(a=a), self.assertRaises(ValueError): validate_receipts(log_fixture().replace(a, b), 'pass')

    def test_corruption_must_reach_the_actual_last_word(self):
        good = log_fixture('corrupt-last')
        for old, new in (('word=16383', 'word=16382'), ('actual=0', 'actual=1'), ('expected=2353232594', 'expected=0')):
            with self.assertRaises(ValueError): validate_receipts(good.replace(old, new), 'corrupt-last')
        with self.assertRaises(ValueError): validate_receipts(log_fixture('early-refusal'), 'corrupt-last')

    def test_native_mapping_requires_opened_resource_and_exact_unmodified_value(self):
        c, rows = native_fixture(); validate_native_imports(rows, c, 'pass')
        for key, value in (('resourceOpened', False), ('driverProtection', 268435457), ('bytes', 131072),
            ('ntstatus', -1), ('unchanged', False), ('pagingFence', 0), ('addressRangeValidated', 1)):
            d, records = native_fixture(); records[-1][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): validate_native_imports(records, d, 'pass')

    def test_missing_native_open_and_unreleased_reference_are_rejected(self):
        c, rows = native_fixture()
        with self.assertRaises(ValueError): validate_native_imports(rows[:3] + rows[4:], c, 'pass')
        for key, value in (('closedResourceHandles', 1), ('importedResourceFdObjects', True), ('completedVendorGpuVaWaits', 149)):
            d, records = native_fixture(); d[key] = value
            with self.assertRaises(ValueError): validate_native_imports(records, d, 'pass')
        c, rows = native_fixture(); rows[0]['sharedNtHandleClosed'] = False
        with self.assertRaises(ValueError): validate_native_imports(rows, c, 'pass')

    def test_retained_refusal_cannot_be_erased_or_mislabeled(self):
        for profile, key, value in (('map-refusal', 'gpuReservationsReleasedAfterVmExit', 4),
            ('early-refusal', 'failedSharedResourceProbes', 1)):
            c = {name: 0 for name in REQUIRED_NATIVE_COUNTERS}; vm = {name: 0 for name in REQUIRED_VM_COUNTERS}
            c[key] = value; validate_control_debt(c, vm, profile)
            c[key] = 0
            with self.assertRaises(ValueError): validate_control_debt(c, vm, profile)
            del c[key]
            with self.assertRaises(ValueError): validate_control_debt(c, vm, profile)


if __name__ == '__main__': unittest.main()
