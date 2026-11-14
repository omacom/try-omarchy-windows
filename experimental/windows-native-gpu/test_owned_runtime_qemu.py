"""Verify live paging in QEMU owned by the Windows KMT driver process.

This records D3D12 initialization and strict cleanup; it does not signify a usable desktop GPU.
The supplied runtime image is private and must never be uploaded.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import threading
from triangle_evidence import triangle_workload_complete
from shared_resource_evidence import shared_resource_import_complete
from shared_texture_evidence import shared_texture_consume_complete
from native_metadata_evidence import native_texture_runtime_bytes


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def initialization_complete(log, owner_exit, control, cleanup):
    """Require guest device AND queue success with the genuine bridged UMD."""
    return (owner_exit == 0 and all(marker in log for marker in (
        'factory=00000000', 'list=00000000', 'nvidia vendor=4318',
        'nvidiaUmdPresentDuringPrivateQuery=true', 'LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true',
        'transport=virtio-port', 'realDxgForwarding=false')) and
        all(re.findall(r'^' + re.escape(prefix) + r'=([0-9a-f]{8})[ \t\r]*$', log, re.MULTILINE) == ['00000000']
            for prefix in ('device', 'copyQueue')) and
        re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.MULTILINE) == ['0'] and
        control.get('ownedQemuExited') is True and control.get('qemuExit') == 0 and
        control.get('qemuForcedStop') is False and control.get('fenceControlFailed') is False and
        cleanup.get('driverCleanupVerified') is True)


def copy_workload_complete(log, asynchronous_commands_verified=False):
    """Require two independently checked patterns after guest GPU submission."""
    if re.findall(r'^GPU_COPY_TEST_BEGIN bytes=(\d+) rounds=(\d+)[ \t\r]*$', log, re.MULTILINE) != [('65536', '2')]:
        return False
    if re.findall(r'^GPU_COPY_TEST_COMPLETE verified=true bytes=(\d+) rounds=(\d+)[ \t\r]*$', log, re.MULTILINE) != [('65536', '2')]:
        return False
    tail = log.split('GPU_COPY_TEST_BEGIN', 1)[1]
    if tail.count('nativeCommandSubmitted=true') < 2 and not (asynchronous_commands_verified and tail.count('nativeCommandQueued=true') >= 2):
        return False
    for stage, count in (('Upload', 1), ('Default', 1), ('Readback', 1), ('Allocator', 1), ('CommandList', 1),
                         ('Fence', 1), ('UploadMap', 2), ('ReadbackMap', 2), ('Close', 2), ('Signal', 2),
                         ('AllocatorReset', 1), ('CommandListReset', 1)):
        if re.findall(r'^gpuCopy' + stage + r'=([0-9a-f]{8})[ \t\r]*$', tail, re.MULTILINE) != ['00000000'] * count:
            return False
    rounds = re.findall(r'^GPU_COPY_ROUND round=(\d+) verifiedBytes=(\d+) expectedHash=([0-9a-f]{16}) observedHash=([0-9a-f]{16}) fenceTarget=(\d+) fenceObserved=(\d+)[ \t\r]*$', tail, re.MULTILINE)
    if len(rounds) != 2:
        return False
    for expected_round, (round_, bytes_, expected, observed, target, retired) in enumerate(rounds, 1):
        checksum = 14695981039346656037
        for offset in range(65536):
            checksum = ((checksum ^ ((offset * 37 + (offset >> 8) * 11 + expected_round * 73) & 255)) * 1099511628211) & ((1 << 64) - 1)
        if (int(round_) != expected_round or int(bytes_) != 65536 or expected != f'{checksum:016x}' or observed != expected or
                int(target) != expected_round or int(retired) < int(target) or int(retired) == (1 << 64) - 1):
            return False
    return True


def expected_clear_pixels(round_):
    """Dense RGBA oracle, independent of the guest's pitched readback addressing."""
    background, inner = ((b'\xff\x00\x00\xff', b'\x00\xff\xff\xff') if round_ == 1 else
                         (b'\x00\x00\xff\xff', b'\xff\xff\x00\xff'))
    return b''.join(inner if 11 <= x < 119 and 7 <= y < 65 else background for y in range(73) for x in range(130))


def clear_workload_complete(log, asynchronous_commands_verified=False):
    """Check graphics queue execution, both hashes and every final exported pixel."""
    if re.findall(r'^GPU_CLEAR_TEST_BEGIN width=(\d+) height=(\d+) format=(\w+) rounds=(\d+)[ \t\r]*$', log, re.MULTILINE) != [('130', '73', 'R8G8B8A8_UNORM', '2')]:
        return False
    if re.findall(r'^GPU_CLEAR_TEST_COMPLETE verified=true width=(\d+) height=(\d+) rounds=(\d+)[ \t\r]*$', log, re.MULTILINE) != [('130', '73', '2')]:
        return False
    tail = log.split('GPU_CLEAR_TEST_BEGIN', 1)[1]
    commands_verified = (tail.count('nativeCommandSubmitted=true') >= 2 or
                         (asynchronous_commands_verified and tail.count('nativeCommandQueued=true') >= 2))
    if not commands_verified or 'GPU_CLEAR_TIMEOUT' in tail or 'gpuClearDeviceRemoved=' in tail:
        return False
    for stage, count in (('DirectQueue', 1), ('RenderTarget', 1), ('Readback', 1), ('RtvHeap', 1), ('Allocator', 1),
                         ('CommandList', 1), ('Fence', 1), ('Close', 2), ('Signal', 2), ('ReadbackMap', 2),
                         ('AllocatorReset', 1), ('CommandListReset', 1)):
        if re.findall(r'^gpuClear' + stage + r'=([0-9a-f]{8})[ \t\r]*$', tail, re.MULTILINE) != ['00000000'] * count:
            return False
    footprints = re.findall(r'^GPU_CLEAR_FOOTPRINT width=(\d+) height=(\d+) offset=(\d+) rowPitch=(\d+) rowBytes=(\d+) rows=(\d+) totalBytes=(\d+)[ \t\r]*$', tail, re.MULTILINE)
    if len(footprints) != 1:
        return False
    width, height, offset, pitch, row_bytes, rows, total = map(int, footprints[0])
    if (width, height, offset, row_bytes, rows) != (130, 73, 512, 520, 73) or not 520 <= pitch <= 4096 or pitch % 256 or not offset + 72 * pitch + row_bytes <= total <= 1048576:
        return False
    rounds = re.findall(r'^GPU_CLEAR_ROUND round=(\d+) verifiedPixels=(\d+) expectedHash=([0-9a-f]{16}) observedHash=([0-9a-f]{16}) fenceTarget=(\d+) fenceObserved=(\d+)[ \t\r]*$', tail, re.MULTILINE)
    if len(rounds) != 2:
        return False
    for expected_round, (round_, pixels, expected, observed, target, retired) in enumerate(rounds, 1):
        checksum = 14695981039346656037
        for byte in expected_clear_pixels(expected_round):
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        if (int(round_) != expected_round or int(pixels) != 9490 or expected != f'{checksum:016x}' or observed != expected or
                int(target) != expected_round or not int(target) <= int(retired) < (1 << 64) - 1):
            return False
    pixel_rows = re.findall(r'^GPU_CLEAR_PIXEL_ROW round=(\d+) y=(\d+) rgba=([0-9a-f]+)[ \t\r]*$', tail, re.MULTILINE)
    if len(pixel_rows) != 73:
        return False
    expected = expected_clear_pixels(2)
    for y, (round_, row, rgba) in enumerate(pixel_rows):
        if int(round_) != 2 or int(row) != y or rgba != expected[y * 520:(y + 1) * 520].hex():
            return False
    return tail.index('GPU_CLEAR_ROUND round=1') < tail.index('GPU_CLEAR_ROUND round=2') < tail.index('GPU_CLEAR_PIXEL_ROW') < tail.index('GPU_CLEAR_TEST_COMPLETE')


def no_broadcast_queue_eof_complete(log, control, cleanup, target_flags=2):
    """Late EOF has acknowledged earlier teardown and retains the new queue.

    Keep early-EOF zero-ack rules separate. Count each successfully unmapped
    CPU span and fence rather than treating all created views as exit releases.
    This control cannot certify initialization or rendered pixels.
    """
    if target_flags not in (2, 6):
        return False
    marker = f'LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags={target_flags}'
    if log.count(marker) != 1 or re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', log, re.MULTILINE) != ['1']:
        return False
    if (log.count('GPU_CLEAR_TEST_BEGIN') != 1 or 'GPU_COPY_TEST_BEGIN' in log or 'gpuClearDirectQueue=' in log or
        'GPU_CLEAR_ROUND' in log or 'GPU_CLEAR_TEST_COMPLETE' in log):
        return False
    if any(re.findall(r'^' + prefix + r'=([0-9a-f]{8})[ \t\r]*$', log, re.MULTILINE) != ['00000000'] for prefix in ('device', 'copyQueue')):
        return False
    flags = re.findall(r'nativeHwQueueCreated=true privateBytes=\d+ progressFenceDirect=true flags=(\d+)', log)
    if (not flags or flags[-1] != str(target_flags) or flags.count(str(target_flags)) != 1 or
        any(f not in ('0','2','6') for f in flags) or
        cleanup.get('completedNoBroadcastSignalHwQueues') != sum(bool(int(f) & 2) for f in flags) or
        (target_flags == 2 and '6' in flags) or
        (target_flags == 6 and cleanup.get('completedNoBroadcastWaitHwQueues') != 1)):
        return False
    if log.index('GPU_CLEAR_TEST_BEGIN') > log.index(marker) or cleanup.get('hwQueuesReleasedAfterVmExit', 0) < 1:
        return False
    fences = log.count('LINUX_BRIDGE guestFenceUnmapped=true')
    cpu = re.findall(r'allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0 bytes=(\d+)', log)
    if len(cpu) != log.count('allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0'):
        return False
    sizes = list(map(int, cpu))
    if any(not 0 < b <= 4194304 or b % 4096 for b in sizes):
        return False
    slots = sum((b + 1048575) // 1048576 for b in sizes)
    return (control.get('ownedQemuExited') is True and control.get('qemuExit') == 0 and
            control.get('qemuForcedStop') is False and control.get('fenceControlFailed') is False and
            cleanup.get('driverCleanupVerified') is True and
            0 <= fences < control.get('fenceMappingsCreated', 0) and control.get('fenceUnmapAcknowledgements') == fences and
            control.get('allocationUnmapAcknowledgements') == len(sizes) and
            control.get('allocationSlotUnmapAcknowledgements') == slots)


def async_submissions_complete(queued, accepted, retired, cleanup, *, maximum=16, pending_limit=16):
    """Enqueue receipts alone never prove completion; correlate every fence."""
    count = len(queued)
    if not 0 < count <= maximum or len(accepted) != count or len(retired) != count:
        return False
    if (cleanup.get('asynchronousSubmissionOptIn') is not True or
        cleanup.get('acceptedAsyncSubmissions') != count or cleanup.get('completedNativeSubmissions') != count or
        cleanup.get('pendingNativeSubmissions') != 0 or not 0 < cleanup.get('peakPendingNativeSubmissions',0) <= min(count,pending_limit)):
        return False
    if [a.get('index') for a in accepted] != list(range(1,count+1)) or sorted(r.get('index',0) for r in retired) != list(range(1,count+1)):
        return False
    by_index = {r['index']:r for r in retired}
    for index,(q,a) in enumerate(zip(queued,accepted),1):
        r = by_index[index]
        if (any(q.get(k) != a.get(k) or q.get(k) != r.get(k) for k in ('bytes','privateBytes','target')) or
            q.get('observedAtReturn') != a.get('observed') or
            not 0 <= q.get('observedAtReturn',-1) < (1<<64)-1 or
            not 0 < q.get('target',0) <= r.get('observed',0) < (1<<64)-1):
            return False
    return True


def hw_queue_signals_complete(queued, returned, accepted, retired, cleanup, *, maximum=16, pending_limit=16):
    count=len(queued)
    if count == 0:
        return not returned and not accepted and not retired and all(cleanup.get(k,0) == 0 for k in
            ('nativeHwQueueSignalAttempts','acceptedHwQueueSignals','completedHwQueueSignals','failedHwQueueSignals','pendingHwQueueSignals','peakPendingHwQueueSignals'))
    if not 0 < count <= maximum or len(returned) != count or len(accepted) != count or len(retired) != count:
        return False
    if (any(cleanup.get(k) != count for k in ('nativeHwQueueSignalAttempts','acceptedHwQueueSignals','completedHwQueueSignals')) or
        cleanup.get('failedHwQueueSignals') != 0 or cleanup.get('pendingHwQueueSignals') != 0 or
        not 0 < cleanup.get('peakPendingHwQueueSignals',0) <= min(count,pending_limit)): return False
    if [a.get('index') for a in accepted] != list(range(1,count+1)) or sorted(r.get('index',0) for r in retired) != list(range(1,count+1)):
        return False
    by_index={r['index']:r for r in retired}
    for index,(q,n,a) in enumerate(zip(queued,returned,accepted),1):
        r=by_index[index]
        if (q.get('flags') not in (0,4) or not 0 < q.get('queues',0) <= 8 or n.get('ntstatus') != 0 or
            any(q.get(k) != n.get(k) or q.get(k) != a.get(k) or q.get(k) != r.get(k) for k in ('flags','queues','target')) or
            q.get('observedAtReturn') != n.get('observed') or q.get('observedAtReturn') != a.get('observed') or
            not 0 <= q.get('observedAtReturn',-1) < (1<<64)-1 or
            not 0 < q.get('target',0) <= r.get('observed',0) < (1<<64)-1 or
            any(v.get('noGpuAccess') is not True or v.get('cpuValueWrittenByBridge') is not False for v in (a,r))): return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'firmware', 'kernel', 'initramfs', 'bridge', 'report'):
        parser.add_argument('--' + name, type=pathlib.Path, required=True)
    parser.add_argument('--expected-unimplemented-ioctl', type=int, required=True)
    parser.add_argument('--driver-consume-shared', action='store_true', help='Opt into native GPU consumption after explicit COMMON/fence handoff')
    parser.add_argument('--driver-consume-shared-process', action='store_true', help='Use an owned separate Windows GPU consumer process')
    parser.add_argument('--driver-native-metadata', action='store_true', help='Enable typed texture allocation with Windows-generated runtime metadata')
    parser.add_argument('--driver-coordinator', action='store_true', help='Use the explicit privileged multi-client native coordinator in the owned 4K root lab')
    parser.add_argument('--driver-window-input', action='store_true', help='Enable the dedicated bounded input channel for the owned desktop window')
    parser.add_argument('--driver-input-self-test', action='store_true', help='Send the fixed disposable-guest PTY test sequence after its first GPU frame; does not prove physical Windows input')
    parser.add_argument('--driver-sdl-relay', action='store_true', help='Opt in to the NVIDIA compositor output through the existing QEMU SDL frontend')
    parser.add_argument('--driver-rootfs-readonly',type=pathlib.Path,help='Opt-in real-root boot through two read-only block nodes; private image, no persistent writes')
    parser.add_argument('--driver-present-shared', action='store_true', help='Present consumed guest frames in an owned diagnostic Windows window')
    parser.add_argument('--driver-shared-resources', action='store_true', help='Opt into bounded shareable NVIDIA resource creation and native import probing')
    parser.add_argument('--runtime-workload', choices=('init', 'copy', 'clear', 'triangle', 'shared', 'consume', 'native', 'native-stream', 'opengl'), default='init', help='Require the workload selected when packing the private guest image')
    parser.add_argument('--driver-allocations', action='store_true', help='Explicitly enable diagnostic vendor video-memory allocations')
    parser.add_argument('--minimum-vendor-allocations', type=int, default=0)
    parser.add_argument('--minimum-uninitialized-source-allocations', type=int, default=0)
    parser.add_argument('--driver-gpuva', action='store_true')
    parser.add_argument('--minimum-vendor-gpuva-maps', type=int, default=0)
    parser.add_argument('--driver-residency', action='store_true')
    parser.add_argument('--minimum-vendor-residency-requests', type=int, default=0)
    parser.add_argument('--driver-cpu', action='store_true')
    parser.add_argument('--driver-cpu-slots', type=int, choices=(16, 32, 64, 128, 256, 512, 1024), default=16)
    parser.add_argument('--driver-fence-slots', type=int, choices=(64, 128, 256, 512), default=None)
    parser.add_argument('--minimum-expanded-cpu-mappings', type=int, default=0)
    parser.add_argument('--expected-cpu-slot-quota-rejections', type=int)
    parser.add_argument('--minimum-vendor-cpu-locks', type=int, default=0)
    parser.add_argument('--minimum-spanning-cpu-mappings', type=int, default=0)
    parser.add_argument('--minimum-subrange-cpu-mappings', type=int, default=0)
    parser.add_argument('--driver-translation', action='store_true')
    parser.add_argument('--minimum-allocation-translations', type=int, default=0)
    parser.add_argument('--driver-hwqueues', action='store_true')
    parser.add_argument('--minimum-hwqueues', type=int, default=0)
    parser.add_argument('--driver-syncs', action='store_true')
    parser.add_argument('--minimum-monitored-fences', type=int, default=0)
    parser.add_argument('--minimum-sync-mutexes', type=int, default=0)
    parser.add_argument('--minimum-no-gpu-access-fences', type=int, default=0)
    parser.add_argument('--minimum-no-max-tdr-fences', type=int, default=0)
    parser.add_argument('--minimum-context-priority-changes', type=int, default=0)
    parser.add_argument('--driver-reservation', action='store_true')
    parser.add_argument('--minimum-gpu-reservations', type=int, default=0)
    parser.add_argument('--driver-gpu-state', action='store_true')
    parser.add_argument('--minimum-gpu-state-maps', type=int, default=0)
    parser.add_argument('--driver-submit', action='store_true')
    parser.add_argument('--minimum-submissions', type=int, default=0)
    parser.add_argument('--driver-retirement', action='store_true')
    parser.add_argument('--driver-async-submit', action='store_true', help='Return after native queue acceptance; require independently correlated retirement for every command')
    parser.add_argument('--minimum-retirements-with-queues', type=int, default=0)
    parser.add_argument('--expected-allocation-limit', type=int, default=0, help='Require the observed diagnostic allocation-count boundary')
    parser.add_argument('--cpu-store-test', action='store_true', help='Explicit first/last-word diagnostic stores, checked and restored by Windows')
    parser.add_argument('--cpu-eof-test', action='store_true', help='Exit the guest probe while it owns the CPU lock; require VM-exit-first native teardown')
    parser.add_argument('--cpu-span-eof-test', action='store_true', help='Exit after a CPU view spanning multiple native slots is mapped')
    parser.add_argument('--hwqueue-eof-test', action='store_true', help='Exit the guest probe with its first hardware queue still owned')
    parser.add_argument('--hwqueue-no-broadcast-eof-test', action='store_true', help='Clear workload control: exit immediately after its first NoBroadcastSignal queue, before graphics startup fills the aperture')
    parser.add_argument('--hwqueue-no-broadcast-wait-eof-test', action='store_true', help='Clear workload control: exit immediately after its first combined NoBroadcastSignal/NoBroadcastWait queue')
    parser.add_argument('--sync-eof-test', action='store_true', help='Exit the guest probe with its first NoGPUAccess fence still owned')
    parser.add_argument('--sync-no-max-eof-test', action='store_true', help='Exit with the first NoSignalMaxValueOnTdr fence still owned')
    parser.add_argument('--reservation-eof-test', action='store_true', help='Exit the guest probe with its first GPU address reservation still owned')
    parser.add_argument('--gpu-state-eof-test', action='store_true', help='Exit the guest after its first retired GPU Zero/NoAccess mapping')
    args = parser.parse_args()
    early_guest_eof = args.cpu_eof_test or args.hwqueue_eof_test or args.sync_eof_test or args.reservation_eof_test or args.gpu_state_eof_test or args.cpu_span_eof_test or args.sync_no_max_eof_test
    late_guest_eof = args.hwqueue_no_broadcast_eof_test or args.hwqueue_no_broadcast_wait_eof_test
    guest_eof = early_guest_eof or late_guest_eof
    if late_guest_eof and (args.runtime_workload != 'clear' or not args.driver_submit or not args.driver_retirement or args.cpu_store_test or early_guest_eof or
                          (args.hwqueue_no_broadcast_eof_test and args.hwqueue_no_broadcast_wait_eof_test)):
        parser.error('NoBroadcast queue EOF control requires the clear image with submission and retirement in a separate run')
    if args.runtime_workload != 'init' and (not args.driver_submit or not args.driver_retirement or args.cpu_store_test or early_guest_eof):
        parser.error('GPU workload requires submission and retirement in a separate run from CPU/EOF controls')
    if args.sync_no_max_eof_test and (not args.driver_syncs or args.cpu_store_test or args.cpu_eof_test or args.hwqueue_eof_test or args.sync_eof_test or args.reservation_eof_test or args.gpu_state_eof_test or args.cpu_span_eof_test):
        parser.error('NoSignalMaxValueOnTdr EOF control requires syncs and a separate diagnostic run')
    if args.minimum_uninitialized_source_allocations < 0 or (args.minimum_uninitialized_source_allocations and not args.driver_allocations):
        parser.error('Uninitialized display-source acceptance requires allocation opt-in')
    if args.minimum_no_max_tdr_fences < 0 or (args.minimum_no_max_tdr_fences and not args.driver_syncs):
        parser.error('NoSignalMaxValueOnTdr acceptance requires synchronization opt-in')
    if args.cpu_span_eof_test and (not args.driver_cpu or args.cpu_store_test or args.cpu_eof_test or args.hwqueue_eof_test or args.sync_eof_test or args.reservation_eof_test or args.gpu_state_eof_test):
        parser.error('CPU span EOF control requires CPU locks and a separate diagnostic run')
    if args.minimum_gpu_state_maps < 0 or (args.driver_gpu_state and not args.driver_reservation) or (args.minimum_gpu_state_maps and not args.driver_gpu_state):
        parser.error('GPU address-state acceptance requires reservations and the explicit state opt-in')
    if args.gpu_state_eof_test and (not args.driver_gpu_state or args.cpu_store_test or args.cpu_eof_test or args.hwqueue_eof_test or args.sync_eof_test or args.reservation_eof_test):
        parser.error('GPU state EOF control requires state mappings and a separate diagnostic run')
    if args.minimum_gpu_reservations < 0 or (args.driver_reservation and not args.driver_gpuva) or (args.minimum_gpu_reservations and not args.driver_reservation):
        parser.error('GPU reservation acceptance requires GPU-address and reservation opt-ins')
    if args.reservation_eof_test and (not args.driver_reservation or args.cpu_eof_test or args.hwqueue_eof_test or args.sync_eof_test or args.cpu_store_test):
        parser.error('Reservation EOF control requires reservation opt-in and a separate diagnostic run')
    if args.minimum_vendor_allocations < 0 or (args.minimum_vendor_allocations and not args.driver_allocations):
        parser.error('Minimum allocation acceptance requires the explicit allocation opt-in')
    if args.minimum_vendor_gpuva_maps < 0 or (args.driver_gpuva and not args.driver_allocations) or (args.minimum_vendor_gpuva_maps and not args.driver_gpuva):
        parser.error('GPU-address mapping acceptance requires explicit allocation and GPU-address opt-ins')
    if args.minimum_vendor_residency_requests < 0 or (args.driver_residency and not args.driver_allocations) or (args.minimum_vendor_residency_requests and not args.driver_residency):
        parser.error('Residency acceptance requires explicit allocation and residency opt-ins')
    if args.minimum_vendor_cpu_locks < 0 or (args.driver_cpu and not args.driver_gpuva) or (args.minimum_vendor_cpu_locks and not args.driver_cpu):
        parser.error('CPU-lock acceptance requires explicit CPU and GPU-address opt-ins')
    if args.minimum_spanning_cpu_mappings < 0 or (args.minimum_spanning_cpu_mappings and not args.driver_cpu):
        parser.error('Spanning CPU mapping acceptance requires CPU locks')
    if args.minimum_expanded_cpu_mappings < 0 or (args.driver_cpu_slots != 16 and not args.driver_cpu) or (args.minimum_expanded_cpu_mappings and args.driver_cpu_slots == 16):
        parser.error('Expanded CPU mappings require explicit CPU locks with 32, 64 or 128 aperture slots')
    if args.expected_cpu_slot_quota_rejections is not None and (args.expected_cpu_slot_quota_rejections < 0 or not args.driver_cpu):
        parser.error('CPU-slot quota acceptance requires explicit CPU locks')
    if args.cpu_store_test and not args.driver_cpu:
        parser.error('CPU store control requires explicit CPU-lock opt-in')
    if args.minimum_allocation_translations < 0 or (args.driver_translation and not args.driver_allocations) or (args.minimum_allocation_translations and not args.driver_translation):
        parser.error('Allocation translation acceptance requires allocation and translation opt-ins')
    if args.minimum_hwqueues < 0 or (args.driver_hwqueues and not args.driver_translation) or (args.minimum_hwqueues and not args.driver_hwqueues):
        parser.error('Hardware queue acceptance requires allocation translation and hardware queue opt-ins')
    if args.expected_allocation_limit < 0 or (args.expected_allocation_limit and not args.driver_allocations):
        parser.error('An allocation-limit boundary requires allocation opt-in')
    if args.minimum_retirements_with_queues < 0 or (args.driver_retirement and not args.driver_hwqueues) or (args.minimum_retirements_with_queues and not args.driver_retirement):
        parser.error('Allocation retirement requires the owned hardware queue opt-in')
    if min(args.minimum_monitored_fences, args.minimum_sync_mutexes, args.minimum_no_gpu_access_fences) < 0 or (
            (args.minimum_monitored_fences or args.minimum_sync_mutexes or args.minimum_no_gpu_access_fences) and not args.driver_syncs):
        parser.error('Synchronization acceptance requires the explicit synchronization opt-in')
    if args.cpu_eof_test and (not args.driver_cpu or args.cpu_store_test):
        parser.error('CPU EOF control requires CPU locks and a separate run from store control')
    if args.hwqueue_eof_test and (not args.driver_hwqueues or args.cpu_eof_test or args.cpu_store_test):
        parser.error('Hardware queue EOF control requires hardware queues and a separate diagnostic run')
    if args.sync_eof_test and (not args.driver_syncs or args.cpu_eof_test or args.hwqueue_eof_test or args.cpu_store_test):
        parser.error('Synchronization EOF control requires syncs and a separate diagnostic run')
    if args.minimum_submissions < 0 or (args.minimum_submissions and not args.driver_submit) or (
            args.driver_submit and (not args.driver_hwqueues or not args.driver_syncs or not args.driver_cpu or not args.driver_residency or
                                    args.cpu_store_test or early_guest_eof)):
        parser.error('Submission requires queues, syncs, CPU mappings and residency in a separate diagnostic run')
    if args.driver_native_metadata and not args.driver_shared_resources:
        parser.error('Native texture metadata requires explicit shared resource opt-in')
    if args.driver_present_shared and not args.driver_consume_shared:
        parser.error('Shared texture presentation requires explicit consumption opt-in')
    if args.driver_consume_shared_process and not args.driver_consume_shared:
        parser.error('Shared GPU worker requires explicit consumption opt-in')
    if args.driver_consume_shared and (not args.driver_shared_resources or not args.driver_async_submit or not args.driver_syncs):
        parser.error("Shared texture consumption requires shared resources, asynchronous submission and sync opt-ins")
    if args.driver_shared_resources and not args.driver_allocations:
        parser.error('Shared resource diagnostics require allocation opt-in')
    if args.driver_async_submit and (not args.driver_submit or not args.driver_retirement):
        parser.error('Asynchronous submission requires submission and retirement')
    if sys.platform != 'win32':
        parser.error('Run on the Windows NVIDIA host')
    for path in (args.qemu, args.kernel, args.initramfs, args.bridge):
        if not path.is_file():
            parser.error(f'Missing file: {path}')
    if args.driver_rootfs_readonly and (not args.driver_rootfs_readonly.is_absolute() or not args.driver_rootfs_readonly.is_file()):
        parser.error('Read-only root image must be an existing absolute regular file')
    log_path = args.report.with_suffix('.log')
    error_log_path = args.report.with_suffix('.errors.log')
    if args.report.exists() or log_path.exists() or error_log_path.exists() or args.report.with_suffix('.host.log').exists():
        parser.error('Use a fresh report path')
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    api.OpenProcess.restype = wintypes.HANDLE
    api.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    api.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    if args.driver_window_input and (not args.driver_coordinator or not args.driver_rootfs_readonly or args.driver_fence_slots != 512):
        parser.error('Window input requires the owned read-only-root coordinator and 512 fence slots')
    if args.driver_input_self_test and not args.driver_window_input:
        parser.error('Input self-test requires explicit window input')
    owner = subprocess.Popen([str(args.bridge.resolve()), '--run-qemu', str(args.qemu.resolve()),
                              str(args.firmware.resolve()), str(args.kernel.resolve()), str(args.initramfs.resolve()),
                              str(log_path.resolve()), '--driver-contexts', '--driver-queries'] +
                             (['--driver-allocations'] if args.driver_allocations else []) + (['--driver-gpuva'] if args.driver_gpuva else []) +
                             (['--driver-residency'] if args.driver_residency else []) + (['--driver-cpu'] if args.driver_cpu else []) +
                             (['--driver-cpu-slots', str(args.driver_cpu_slots)] if args.driver_cpu_slots != 16 else []) +
                             (['--driver-fence-slots', str(args.driver_fence_slots)] if args.driver_fence_slots is not None else []) +
                             (['--driver-translation'] if args.driver_translation else []) +
                             (['--driver-hwqueues'] if args.driver_hwqueues else []) +
                             (['--driver-syncs'] if args.driver_syncs else []) +
                             (['--driver-reservation'] if args.driver_reservation else []) +
                             (['--driver-gpu-state'] if args.driver_gpu_state else []) +
                             (['--driver-submit'] if args.driver_submit else []) +
                             (['--driver-retirement'] if args.driver_retirement else []) +
                             (['--driver-async-submit'] if args.driver_async_submit else []) +
                             (['--driver-shared-resources'] if args.driver_shared_resources else []) +
                             (['--driver-consume-shared'] if args.driver_consume_shared else []) +
                             (['--driver-consume-shared-process'] if args.driver_consume_shared_process else []) +
                             (['--driver-present-shared'] if args.driver_present_shared else []) +
                             (['--driver-native-metadata'] if args.driver_native_metadata else []) +
                             (['--driver-coordinator'] if args.driver_coordinator else []) +
                             (['--driver-window-input'] if args.driver_window_input else []) +
                             (['--driver-input-self-test'] if args.driver_input_self_test else []) +
                             (['--driver-sdl-relay'] if args.driver_sdl_relay else []) +
                             (['--cpu-store-test'] if args.cpu_store_test else []) + (['--cpu-eof-test'] if args.cpu_eof_test else []) +
                             (['--cpu-span-eof-test'] if args.cpu_span_eof_test else []) +
                             (['--hwqueue-eof-test'] if args.hwqueue_eof_test else []) +
                             (['--hwqueue-no-broadcast-eof-test'] if args.hwqueue_no_broadcast_eof_test else []) +
                             (['--hwqueue-no-broadcast-wait-eof-test'] if args.hwqueue_no_broadcast_wait_eof_test else []) +
                             (['--sync-eof-test'] if args.sync_eof_test else []) +
                             (['--sync-no-max-eof-test'] if args.sync_no_max_eof_test else []) +
                             (['--reservation-eof-test'] if args.reservation_eof_test else []) +
                             (['--gpu-state-eof-test'] if args.gpu_state_eof_test else []) +
                             (['--driver-rootfs-readonly',str(args.driver_rootfs_readonly.resolve())] if args.driver_rootfs_readonly else []),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    output, errors, observer_errors = [], [], []
    child_handle = None

    def read_stdout():
        nonlocal child_handle
        try:
            for line in owner.stdout:
                output.append(line)
                message = json.loads(line)
                if 'ownedQemuPid' in message:
                    handle = api.OpenProcess(0x00101001, False, message['ownedQemuPid'])
                    if not handle:
                        raise RuntimeError('Cannot retain the owned QEMU process for timeout cleanup')
                    name, size = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
                    if not api.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)) or pathlib.Path(name.value).resolve() != args.qemu.resolve():
                        api.CloseHandle(handle)
                        raise RuntimeError('Owned QEMU executable identity mismatch')
                    child_handle = handle
        except Exception as error:
            observer_errors.append(str(error))

    def read_stderr():
        try:
            with error_log_path.open('x', encoding='utf-8', newline='\n') as stream:
                for line in owner.stderr:
                    errors.append(line); stream.write(line); stream.flush()
        except Exception as error:
            observer_errors.append(str(error))

    # Inherited pipe writers must not keep a failed observer alive after its
    # bounded owner/VM termination path. Neither thread releases native pages.
    reader = threading.Thread(target=read_stdout, daemon=True)
    error_reader = threading.Thread(target=read_stderr, daemon=True)
    reader.start(); error_reader.start()
    try:
        try:
            owner.wait(timeout=300 if args.driver_coordinator and args.driver_fence_slots==512 else 210 if args.runtime_workload == 'native-stream' else 150 if args.driver_coordinator else 110)
        except subprocess.TimeoutExpired:
            # The retained handle cannot target a reused PID. Stop and reap
            # QEMU first, while the owner still retains the native pages.
            if child_handle:
                if api.WaitForSingleObject(child_handle, 0) != 0:
                    if not api.TerminateProcess(child_handle, 91) or api.WaitForSingleObject(child_handle, 10000) != 0:
                        raise RuntimeError('Cannot reap QEMU; native owner deliberately retained')
            elif any('ownedQemuPid' in line for line in output):
                raise RuntimeError('Cannot verify child exit; native owner deliberately retained')
            owner.kill(); owner.wait(timeout=10)
            raise RuntimeError('Owned runtime deadline exceeded')
        reader.join(timeout=5); error_reader.join(timeout=5)
        if reader.is_alive() or error_reader.is_alive() or observer_errors:
            raise RuntimeError('Owner output observation failed: ' + '; '.join(observer_errors))
        control = next((json.loads(line) for line in output if 'ownedQemuExited' in line), {})
        cleanup = next((json.loads(line) for line in errors if line.startswith('{') and 'driverCleanupVerified' in line), {})
        log = log_path.read_text(encoding='utf-8', errors='replace') if log_path.exists() else ''
        unsupported = sorted(set(int(n) for n in re.findall(r'LINUX_BRIDGE unsupported nr=(\d+)', log)))
        completed = [{'type': int(t), 'bytes': int(b)} for t, b in re.findall(r'LINUX_BRIDGE queryCompleted type=(\d+) bytes=(\d+)', log)]
        fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'pagingFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        result = re.search(r'^device=([0-9a-f]{8})\s*$', log, re.MULTILINE)
        copy_queue_result = re.search(r'^copyQueue=([0-9a-f]{8})\s*$', log, re.MULTILINE)
        initialized = initialization_complete(log, owner.returncode, control, cleanup)
        loaded = 'nvidiaUmdPresentDuringPrivateQuery=true' in log
        contexts = log.count('LINUX_BRIDGE nativeContextCreated=true')
        allocations = log.count('LINUX_BRIDGE nativeVendorAllocationCreated=true')
        uninitialized_sources = log.count('LINUX_BRIDGE standaloneSourceUninitializedAccepted=true primary=false privateDataPreserved=true')
        source_normalizations = [json.loads(line) for line in errors if line.startswith('{') and 'vendorAllocationSourceNormalization' in line]
        wire_summaries = [json.loads(line) for line in errors if line.startswith('{') and 'wireObjectSummary' in line]
        wire_quotas = [json.loads(line) for line in errors if line.startswith('{') and 'wireQuotaRejection' in line]
        context_priorities = [json.loads(line) for line in errors if line.startswith('{') and 'nativeContextPriorityVerified' in line]
        guest_priorities = [int(n) for n in re.findall(r'nativeContextPriorityChanged=true priority=(\d+) inProcessOnly=true', log)]
        cpu_regions = [json.loads(line) for line in errors if line.startswith('{') and 'vendorCpuRegionCandidate' in line]
        subrange_cpu = [region for region in cpu_regions if region['regionBytes'] != region['expectedBytes'] or not region['baseMatches']]
        translations = log.count('LINUX_BRIDGE allocationDriverAliasCreated=true typedWireIdentityRetained=true')
        hwqueues = log.count('LINUX_BRIDGE nativeHwQueueCreated=true')
        hwqueue_unmaps = log.count('LINUX_BRIDGE nativeHwQueueDestroyed=true')
        hwqueue_fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'hwQueueFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        sync_types = [int(t) for t in re.findall(r'LINUX_BRIDGE nativeSynchronizationCreated=true type=(\d+)', log)]
        sync_descriptors = [{'type': int(t), 'flags': int(f), 'gpuMapped': bool(int(g))} for t, f, g in
                            re.findall(r'nativeSynchronizationCreated=true type=(\d+) flags=(\d+) gpuMapped=([01])', log)]
        no_gpu_fences = [desc for desc in sync_descriptors if desc['type'] == 5 and desc['flags'] == 128]
        no_max_fences = [desc for desc in sync_descriptors if desc['type'] == 5 and desc['flags'] == 64]
        sync_destroyed = [int(t) for t in re.findall(r'LINUX_BRIDGE nativeSynchronizationDestroyed=true type=(\d+)', log)]
        monitored_fences = [{'offset': int(o), 'value': int(v)} for o, v in re.findall(r'monitoredFenceMapped=true direct=true loads=10000 offset=(\d+) value=(\d+)', log)]
        total_fence_mappings = len(fences) + len(hwqueue_fences) + len(monitored_fences)
        late_fence_unmaps = log.count('LINUX_BRIDGE guestFenceUnmapped=true')
        gpuva = [{'pages': int(p), 'status': int(s), 'fence': int(f)} for p, s, f in
                 re.findall(r'nativeVendorGpuVaMapped=true pages=(\d+) status=(\d+) fence=(\d+)', log)]
        gpu_states = [{'pages': int(p), 'protection': int(t), 'status': int(s), 'fence': int(f)} for p, t, s, f in
                      re.findall(r'nativeGpuStateMapped=true pages=(\d+) protection=(\d+) status=(\d+) fence=(\d+) allocationIsNull=true', log)]
        retirements = [{'target': int(t), 'observed': int(o), 'operation': op or 'gpuva'} for t, o, op in
                       re.findall(r'pagingFenceRetired=true direct=true loads=10000 target=(\d+) observed=(\d+)(?: operation=([\w-]+))?', log)]
        residency = [{'count': int(c), 'status': int(s), 'fence': int(f), 'bytesToTrim': int(b)} for c, s, f, b in
                     re.findall(r'nativeVendorResident=true count=(\d+) status=(\d+) fence=(\d+) bytesToTrim=(\d+)', log)]
        cpu = [{'bytes': int(b), 'offset': int(o), 'generation': int(g)} for b, o, g in
               re.findall(r'allocationCpuMapped=true direct=true loads=10000 bytes=(\d+) offset=(\d+) generation=(\d+)', log)]
        cpu_layouts = [{'slots': int(s), 'stride': int(b)} for s, b in re.findall(r'allocationCpuLayout=true slots=(\d+) stride=(\d+)', log)]
        expanded_cpu = [mapping for mapping in cpu if mapping['offset'] >= 16 * 1048576]
        spanning_cpu = [mapping for mapping in cpu if mapping['bytes'] > 1048576]
        cpu_slot_maps = sum((mapping['bytes'] + 1048575) // 1048576 for mapping in cpu)
        cpu_unlocks = log.count('nativeVendorCpuUnlocked=true')
        cpu_unmaps = log.count('allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0')
        late_cpu_unmap_sizes = [int(b) for b in re.findall(r'allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0 bytes=(\d+)', log)]
        late_cpu_slot_unmaps = sum((b + 1048575) // 1048576 for b in late_cpu_unmap_sizes)
        cpu_released_after_exit = cleanup.get('cpuLocksReleasedAfterVmExit', 0)
        allocation_limit_rejections = log.count('LINUX_BRIDGE ioctlFailed nr=6 errno=24')
        retirement_batches = [{'count': int(c), 'queues': int(q)} for c, q in re.findall(
            r'nativeVendorAllocationsDestroyed=true count=(\d+) hardwareQueuesAlive=(\d+)', log)]
        retirements_with_queues = sum(batch['count'] for batch in retirement_batches if batch['queues'])
        map_retirements = [r for r in retirements if r['operation'] == 'gpuva']
        state_retirements = [r for r in retirements if r['operation'] == 'gpu-state']
        resident_retirements = [r for r in retirements if r['operation'] == 'residency']
        command_retirements = [r for r in retirements if r['operation'] == 'command']
        submissions = [{'bytes': int(b), 'privateBytes': int(p), 'target': int(t), 'observed': int(o)} for b, p, t, o in
                       re.findall(r'nativeCommandSubmitted=true bytes=(\d+) privateBytes=(\d+) target=(\d+) observed=(\d+)', log)]
        submission_preflight = [json.loads(line) for line in errors if line.startswith('{') and 'nativeSubmissionPreflight' in line]
        queued_submissions = [{'bytes':int(b),'privateBytes':int(p),'target':int(t),'observedAtReturn':int(o)} for b,p,t,o in
                              re.findall(r'nativeCommandQueued=true bytes=(\d+) privateBytes=(\d+) target=(\d+) observedAtReturn=(\d+)',log)]
        async_accepted = [json.loads(line) for line in errors if line.startswith('{') and 'nativeAsyncSubmissionAccepted' in line]
        async_retired = [json.loads(line) for line in errors if line.startswith('{') and 'nativeAsyncSubmissionRetired' in line]
        if args.driver_async_submit:
            async_verified = not submissions and not command_retirements and async_submissions_complete(queued_submissions,async_accepted,async_retired,cleanup)
            by_index = {r['index']:r for r in async_retired}
            submissions = [{'bytes':q['bytes'],'privateBytes':q['privateBytes'],'target':q['target'],
                            'observed':by_index.get(i,{}).get('observed',0)} for i,q in enumerate(queued_submissions,1)]
        else:
            async_verified = not queued_submissions and not async_accepted and not async_retired and not cleanup.get('asynchronousSubmissionOptIn',False)
        reservations = [int(n) for n in re.findall(r'nativeGpuReserved=true bytes=(\d+)', log)]
        reservations_freed = [int(n) for n in re.findall(r'nativeGpuReservationFreed=true bytes=(\d+)', log)]
        accepted_stage_marker = ('nativeGpuStateMapped=true' if args.driver_gpu_state else
                                 'nativeGpuReserved=true' if args.driver_reservation else
                                 'nativeCommandSubmitted=true' if args.driver_submit else
                                 'nativeHwQueueCreated=true' if args.driver_hwqueues else
                                 'nativeVendorCpuLocked=true' if args.driver_cpu else
                                 'nativeVendorResident=true' if args.driver_residency else
                                 'nativeVendorGpuVaMapped=true' if args.driver_gpuva else
                                 'nativeVendorAllocationCreated=true' if args.driver_allocations else
                                 'nativeContextCreated=true')
        boundary = re.search(re.escape(accepted_stage_marker) + r'[^\n]*\n[\s\S]*?LINUX_BRIDGE unsupported nr=(\d+)', log)
        first_unsupported_after_stage = int(boundary[1]) if boundary else None
        accepted = (owner.returncode == 0 and loaded and contexts > 0 and len(fences) > 0 and
                    'LINUX_BRIDGE deviceCreated=true' in log and 'factory=00000000' in log and 'list=00000000' in log and
                    {'type': 0, 'bytes': 50616} in completed and
                    control.get('ownedQemuExited') is True and control.get('qemuExit') == 0 and
                    control.get('qemuForcedStop') is False and control.get('fenceControlFailed') is False and
                    control.get('liveFenceMappings') == 0 and control.get('fenceMappingsCreated') == total_fence_mappings and
                    control.get('fenceApertureSlots',64) == (args.driver_fence_slots or (128 if args.driver_cpu_slots >= 128 else 64)) and
                    control.get('fenceUnmapAcknowledgements') == (late_fence_unmaps if late_guest_eof else 0 if early_guest_eof else total_fence_mappings) and
                    cleanup.get('driverCleanupVerified') is True and cleanup.get('failedAdapterQueries') == 0 and
                    len(context_priorities) >= args.minimum_context_priority_changes and
                    cleanup.get('completedContextPriorityChanges') == len(context_priorities) == len(guest_priorities) and
                    cleanup.get('failedContextPriorityChanges') == 0 and
                    all(p['priority'] == g and g in (0, 1) and p['inProcessOnly'] is True for p, g in zip(context_priorities, guest_priorities)) and
                    len(wire_summaries) == 1 and wire_summaries[0]['objectLimit'] == 256 and
                    0 < wire_summaries[0]['peakLiveObjects'] <= 256 and
                    0 <= wire_summaries[0]['liveObjectsBeforeDisconnect'] <= wire_summaries[0]['peakLiveObjects'] and
                    cleanup.get('completedAdapterQueries') == len(completed) and
                    'transport=virtio-port' in log and
                    (initialized or ('BRIDGE_RUNTIME_EXIT=1' in log and
                        (guest_eof or first_unsupported_after_stage == args.expected_unimplemented_ioctl) and
                        (guest_eof or (result is not None and int(result[1], 16) & 0x80000000)))))
        if args.driver_allocations:
            accepted = (accepted and allocations >= args.minimum_vendor_allocations and
                        uninitialized_sources >= args.minimum_uninitialized_source_allocations and
                        cleanup.get('completedVendorUninitializedSourceAllocations') == uninitialized_sources == len(source_normalizations) and
                        all(n['inputSource'] == 4294967295 and n['nativeSource'] == 0 and n['primary'] is False and n['ntstatus'] == 0 for n in source_normalizations) and
                        cleanup.get('liveVendorAllocations') == 0 and cleanup.get('failedVendorAllocations') == 0 and
                        cleanup.get('completedVendorAllocations') == allocations and cleanup.get('destroyedVendorAllocations') == allocations and
                        6 not in unsupported and 19 not in unsupported)
        retirement_verified = (args.driver_retirement and retirements_with_queues >= max(1, args.minimum_retirements_with_queues) and
                               cleanup.get('allocationRetirementOptIn') is True and
                               cleanup.get('vendorDestructionsWithHwQueues') == retirements_with_queues)
        if args.driver_retirement:
            accepted = (accepted and retirement_verified)
        else:
            accepted = (accepted and cleanup.get('allocationRetirementOptIn') is False and
                        cleanup.get('vendorDestructionsWithHwQueues') == 0 and retirements_with_queues == 0)
        if args.expected_allocation_limit:
            accepted = (accepted and allocation_limit_rejections > 0 and
                        cleanup.get('peakVendorAllocationObjects') == args.expected_allocation_limit and
                        cleanup.get('vendorAllocationObjectLimit') == args.expected_allocation_limit)
        if args.driver_gpuva:
            accepted = (accepted and len(gpuva) >= args.minimum_vendor_gpuva_maps and (12 not in unsupported or (not args.driver_gpu_state and args.expected_unimplemented_ioctl == 12)) and
                        cleanup.get('completedVendorGpuVaMaps') == len(gpuva) and cleanup.get('failedVendorGpuVaMaps') == 0 and
                        cleanup.get('completedVendorGpuVaWaits') == len(gpuva) and cleanup.get('liveVendorMappedPages') == 0 and
                        cleanup.get('vendorGpuMappedByteLimit') == (67108864 if args.driver_cpu_slots >= 128 else 33554432) and
                        len(map_retirements) == len(gpuva) and
                        all(retired['target'] == mapped['fence'] and retired['observed'] >= mapped['fence'] for retired, mapped in zip(map_retirements, gpuva)))
        if args.driver_translation:
            accepted = (accepted and translations >= args.minimum_allocation_translations and translations == allocations and
                        cleanup.get('completedAllocationTranslations') == translations and
                        cleanup.get('failedAllocationTranslations') == 0 and cleanup.get('liveAllocationTranslations') == 0)
        if args.driver_hwqueues:
            expected_hw_unmaps = hwqueues - cleanup.get('hwQueuesReleasedAfterVmExit', 0) if late_guest_eof else 0 if args.hwqueue_eof_test or args.sync_eof_test or args.reservation_eof_test or args.gpu_state_eof_test or args.cpu_span_eof_test or args.sync_no_max_eof_test else hwqueues
            accepted = (accepted and hwqueues >= args.minimum_hwqueues and hwqueues == len(hwqueue_fences) and hwqueue_unmaps == expected_hw_unmaps and
                        cleanup.get('completedHwQueues') == hwqueues and cleanup.get('destroyedHwQueues') == hwqueues and
                        cleanup.get('failedHwQueues') == 0 and cleanup.get('liveHwQueues') == 0 and 24 not in unsupported and 27 not in unsupported)
        if args.driver_syncs:
            accepted = (accepted and sync_types.count(5) >= args.minimum_monitored_fences and sync_types.count(1) >= args.minimum_sync_mutexes and
                        all(t in (1, 5) for t in sync_types) and sync_types.count(5) == len(monitored_fences) and
                        cleanup.get('completedSyncObjects') == len(sync_types) and cleanup.get('destroyedSyncObjects') == len(sync_types) and
                        cleanup.get('completedMonitoredFences') == sync_types.count(5) and cleanup.get('completedSynchronizationMutexes') == sync_types.count(1) and
                        len(sync_descriptors) == len(sync_types) and len(no_gpu_fences) >= args.minimum_no_gpu_access_fences and
                        cleanup.get('completedNoGpuAccessFences') == len(no_gpu_fences) and cleanup.get('syncObjectLimit') == 96 and
                        len(no_max_fences) >= args.minimum_no_max_tdr_fences and cleanup.get('completedNoSignalMaxValueOnTdrFences') == len(no_max_fences) and
                        all(d['flags'] in (0, 64, 128) and (d['type'] == 5 or not d['flags']) and
                            d['gpuMapped'] == (d['type'] == 5 and d['flags'] != 128) for d in sync_descriptors) and
                        cleanup.get('failedSyncObjects') == 0 and cleanup.get('liveSyncObjects') == 0 and 16 not in unsupported and 29 not in unsupported and
                        (not early_guest_eof or not sync_destroyed) and
                        len(sync_destroyed) + cleanup.get('syncObjectsReleasedAfterVmExit', 0) == len(sync_types) and
                        (guest_eof or sorted(sync_destroyed) == sorted(sync_types)))
        if args.driver_submit:
            accepted = (accepted and async_verified and len(submissions) >= args.minimum_submissions and 52 not in unsupported and
                        cleanup.get('commandSubmissionOptIn') is True and cleanup.get('completedNativeSubmissions') == len(submissions) and
                        cleanup.get('nativeSubmissionAttempts') == len(submissions) and cleanup.get('failedNativeSubmissions') == 0 and
                        cleanup.get('nativeSubmissionTimeouts') == 0 and cleanup.get('submittedCommandBytes') == sum(s['bytes'] for s in submissions) and
                        len(submission_preflight) == len(submissions) and
                        all(p['initialFence'] < p['targetFence'] and p['targetFence'] == s['target'] for p, s in zip(submission_preflight, submissions)) and
                        (args.driver_async_submit or (len(command_retirements) == len(submissions) and
                        all(s['observed'] >= s['target'] and r['observed'] >= r['target'] and s['target'] == r['target']
                            for s, r in zip(submissions, command_retirements)))))
        if args.driver_reservation:
            accepted = (accepted and len(reservations) >= args.minimum_gpu_reservations and 8 not in unsupported and
                        cleanup.get('completedGpuReservations') == len(reservations) and cleanup.get('freedGpuReservations') == len(reservations) and
                        cleanup.get('failedGpuReservations') == 0 and cleanup.get('liveGpuReservations') == 0 and cleanup.get('liveGpuReservedBytes') == 0 and
                        len(reservations_freed) + cleanup.get('gpuReservationsReleasedAfterVmExit', 0) == len(reservations) and
                        (not early_guest_eof or not reservations_freed))
        else:
            accepted = (accepted and not reservations and cleanup.get('completedGpuReservations') == 0 and cleanup.get('liveGpuReservations') == 0)
        accepted = (accepted and cleanup.get('liveGpuStateRanges') == 0 and cleanup.get('liveGpuStateBytes') == 0 and cleanup.get('failedGpuStateMaps') == 0)
        if args.driver_gpu_state:
            accepted = (accepted and len(gpu_states) >= args.minimum_gpu_state_maps and cleanup.get('gpuStateMappingOptIn') is True and
                        cleanup.get('completedGpuStateMaps') == cleanup.get('completedGpuStateWaits') == len(gpu_states) and
                        len(state_retirements) == len(gpu_states) and
                        all(s['status'] in (0, 259) and (s['protection'] & 12) in (4, 8) and r['target'] == s['fence'] and r['observed'] >= s['fence']
                            for s, r in zip(gpu_states, state_retirements)))
        else:
            accepted = (accepted and not gpu_states and not state_retirements and cleanup.get('gpuStateMappingOptIn') is False and cleanup.get('completedGpuStateMaps') == 0)
        if args.driver_residency:
            accepted = (accepted and len(residency) >= args.minimum_vendor_residency_requests and 11 not in unsupported and
                        all(r['count'] > 0 and r['status'] in (0, 259) for r in residency) and
                        cleanup.get('completedVendorResidencyRequests') == len(residency) and cleanup.get('failedVendorResidencyRequests') == 0 and
                        cleanup.get('completedVendorResidencyWaits') == len(residency) and cleanup.get('liveVendorResidencyAttempts') == 0 and
                        cleanup.get('vendorAllocationsMadeResident') == sum(r['count'] for r in residency) and
                        len(resident_retirements) == len(residency) and
                        all(retired['target'] == made['fence'] and retired['observed'] >= made['fence'] for retired, made in zip(resident_retirements, residency)))
        if args.driver_cpu:
            accepted = (accepted and len(cpu) >= args.minimum_vendor_cpu_locks and 37 not in unsupported and 55 not in unsupported and
                        len(cpu_regions) == len(cpu) and len(subrange_cpu) >= args.minimum_subrange_cpu_mappings and
                        cleanup.get('completedVendorCpuSubrangeLocks') == len(subrange_cpu) and
                        all(r['status'] == 0 and r['exportedRangeContained'] is True and r['type'] == 131072 and r['state'] == 4096 and
                            r['protection'] in (4, 1028) and r['expectedBytes'] > 0 and r['expectedBytes'] <= 4194304 and
                            r['regionBaseOffsetBytes'] >= 0 and r['expectedBytes'] <= r['regionBytes'] and
                            r['regionBaseOffsetBytes'] <= r['regionBytes'] - r['expectedBytes'] for r in cpu_regions) and
                        cpu_layouts == [{'slots': args.driver_cpu_slots, 'stride': 1048576}] and
                        cleanup.get('vendorCpuSlotLimit') == control.get('allocationApertureSlots') == args.driver_cpu_slots and
                        cleanup.get('vendorCpuMappedByteLimit', 16777216) == (33554432 if args.driver_cpu_slots >= 128 else 16777216) and
                        len(expanded_cpu) >= args.minimum_expanded_cpu_mappings and
                        cpu_unlocks + cpu_released_after_exit == len(cpu) and cpu_unmaps == cpu_unlocks and
                        (not early_guest_eof or cpu_unlocks == 0) and
                        cleanup.get('completedVendorCpuLocks') == len(cpu) and cleanup.get('completedVendorCpuUnlocks') == len(cpu) and
                        cleanup.get('failedVendorCpuLocks') == 0 and cleanup.get('failedVendorCpuUnlocks') == 0 and
                        cleanup.get('liveVendorCpuBytes') == 0 and control.get('liveAllocationMappings') == 0 and
                        control.get('liveAllocationMappedBytes') == 0 and control.get('allocationMappingsCreated') == len(cpu) and
                        control.get('allocationUnmapAcknowledgements') == cpu_unlocks and
                        len(spanning_cpu) >= args.minimum_spanning_cpu_mappings and control.get('liveAllocationSlotMappings') == 0 and
                        control.get('allocationSlotMappingsCreated') == cpu_slot_maps and
                        control.get('allocationSlotUnmapAcknowledgements') == (late_cpu_slot_unmaps if late_guest_eof else 0 if early_guest_eof else cpu_slot_maps))
        if args.expected_cpu_slot_quota_rejections is not None:
            accepted = accepted and cleanup.get('vendorCpuSlotQuotaRejections') == args.expected_cpu_slot_quota_rejections
        if args.cpu_eof_test:
            accepted = (accepted and len(cpu) > 0 and 'allocationCpuEofTest=true exitingWithLockOwned=true' in log and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.cpu_span_eof_test:
            accepted = (accepted and len(spanning_cpu) == 1 and 'allocationCpuSpanEofTest=true exitingWithSpanOwned=true' in log and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and
                        cleanup.get('gpuReservationsReleasedAfterVmExit') == len(reservations) and result is None)
        if args.hwqueue_eof_test:
            accepted = (accepted and hwqueues > 0 and 'hwQueueEofTest=true exitingWithQueueOwned=true' in log and
                        cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.sync_eof_test:
            accepted = (accepted and len(no_gpu_fences) == 1 and 'syncEofTest=true exitingWithNoGpuAccessFenceOwned=true' in log and
                        cleanup.get('syncObjectsReleasedAfterVmExit') == len(sync_types) and
                        cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.sync_no_max_eof_test:
            accepted = (accepted and len(no_max_fences) == 1 and 'syncNoMaxEofTest=true exitingWithNoMaxFenceOwned=true' in log and
                        cleanup.get('syncObjectsReleasedAfterVmExit') == len(sync_types) and cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and
                        cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and cleanup.get('gpuReservationsReleasedAfterVmExit') == len(reservations) and result is None)
        if args.reservation_eof_test:
            accepted = (accepted and len(reservations) == 1 and 'reservationEofTest=true exitingWithGpuReservationOwned=true' in log and
                        cleanup.get('gpuReservationsReleasedAfterVmExit') == 1 and cleanup.get('syncObjectsReleasedAfterVmExit') == len(sync_types) and
                        cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.gpu_state_eof_test:
            accepted = (accepted and len(gpu_states) == 1 and 'gpuStateEofTest=true exitingWithGpuStateOwned=true' in log and
                        cleanup.get('gpuReservationsReleasedAfterVmExit') == len(reservations) and cleanup.get('syncObjectsReleasedAfterVmExit') == len(sync_types) and
                        cleanup.get('hwQueuesReleasedAfterVmExit') == hwqueues and cleanup.get('cpuLocksReleasedAfterVmExit') == len(cpu) and result is None)
        if args.cpu_store_test:
            accepted = (accepted and len(cpu) > 0 and cleanup.get('cpuStoreTestRequested') is True and
                        cleanup.get('completedCpuStoreTests') == len(cpu) and cleanup.get('failedCpuStoreTests') == 0 and
                        log.count('allocationCpuStoreTest=true firstLastReadback=true') == len(cpu) and
                        log.count('allocationCpuReferenceTest=true sameGuestPointer=true intermediateUnlockRetained=true') == len(cpu))
        resources = len(re.findall(r'^LINUX_BRIDGE nativeVendorResourceCreated=true allocationCount=1 shared=(?:false|true) systemMemory=false$', log, re.MULTILINE))
        shared_resources = log.count('LINUX_BRIDGE nativeVendorResourceCreated=true allocationCount=1 shared=true systemMemory=false')
        shared_probes = [json.loads(line) for line in errors if line.startswith('{') and '"nativeSharedResourceImportProbe"' in line]
        resource_native = [json.loads(line) for line in errors if line.startswith('{') and '"nativeVendorResourceCreated"' in line]
        driver_protection = [json.loads(line) for line in errors if line.startswith('{') and '"nativeVendorDriverProtectionMap"' in line]
        guest_driver_protection = [int(value) for value in re.findall(r'nativeVendorDriverProtectionMapped=true value=(\d+) resourceOwned=true', log)]
        cpu_waits = [{'target':int(target), 'observed':int(observed), 'owner':owner} for target,observed,owner in re.findall(
            r'cpuFenceWaitRetired=true direct=true target=(\d+) observed=(\d+) owner=(paging|hardware|independent)',log)]
        cpu_wait_inputs = re.findall(r'cpuFenceWaitInput count=(\d+) flags=(\d+) asyncEvent=(\d+) ownedDevice=(\d+) hasObjects=(\d+) hasValues=(\d+)',log)
        context_signals = [{'flags':int(flags),'target':int(target),'observed':int(observed)} for flags,target,observed in re.findall(
            r'nativeContextSignalSubmitted=true flags=(\d+) target=(\d+) observed=(\d+) noGpuAccess=true',log)]
        context_signal_native = [json.loads(line) for line in errors if line.startswith('{') and '"nativeContextSignalRetired"' in line]
        context_signal_status = [json.loads(line) for line in errors if line.startswith('{') and '"nativeContextSignalStatus"' in line]
        context_signal_retirements = [r for r in retirements if r['operation'] == 'context-signal']
        hw_queue_signals=[{'flags':int(f),'queues':int(q),'target':int(t),'observedAtReturn':int(o)} for f,q,t,o in re.findall(
            r'nativeHwQueueSignalQueued=true flags=(\d+) queues=(\d+) target=(\d+) observedAtReturn=(\d+) noGpuAccess=true',log)]
        hw_signal_returned=[json.loads(line) for line in errors if line.startswith('{') and 'nativeHwQueueSignalReturned' in line]
        hw_signal_accepted=[json.loads(line) for line in errors if line.startswith('{') and 'nativeHwQueueSignalAccepted' in line]
        hw_signal_retired=[json.loads(line) for line in errors if line.startswith('{') and 'nativeHwQueueSignalRetired' in line]
        accepted = accepted and hw_queue_signals_complete(hw_queue_signals,hw_signal_returned,hw_signal_accepted,hw_signal_retired,cleanup)
        if hw_queue_signals: accepted = accepted and args.driver_async_submit and 53 not in unsupported
        accepted = (accepted and cleanup.get('completedVendorResources',0) == cleanup.get('destroyedVendorResources',0) == resources == len(resource_native) and
                    cleanup.get('liveVendorResources',0) == 0 and
                    sum(r.get('shared') is True for r in resource_native) == shared_resources and
                    (args.driver_shared_resources or shared_resources == 0) and
                    all(r['allocationCount'] == 1 and type(r['shared']) is bool and r['systemMemory'] is False and r['ntstatus'] == 0 for r in resource_native) and
                    cleanup.get('completedVendorDriverProtectionMaps',0) == len(driver_protection) == len(guest_driver_protection) and
                    all(p['driverProtection'] == g == 268435457 and p['resourceOwned'] is True and p['unchanged'] is True and p['ntstatus'] in (0,259)
                        for p,g in zip(driver_protection,guest_driver_protection)) and
                    cpu_wait_inputs == [('1','0','0','1','1','1')] * len(cpu_waits) and
                    all(0 <= w['target'] <= w['observed'] < (1<<64)-1 for w in cpu_waits) and
                    cleanup.get('nativeContextSignalAttempts',0) == cleanup.get('completedNativeContextSignals',0) == len(context_signals) == len(context_signal_native) == len(context_signal_status) == len(context_signal_retirements) and
                    cleanup.get('failedNativeContextSignals',0) == cleanup.get('nativeContextSignalTimeouts',0) == 0 and
                    all(g['flags'] == s['flags'] == 4 and g['target'] == n['target'] == s['target'] == r['target'] and
                        s['ntstatus'] == 0 and 0 < g['target'] <= g['observed'] < (1<<64)-1 and
                        g['observed'] == n['observed'] and r['observed'] >= r['target'] and n['noGpuAccess'] is True and n['cpuValueWrittenByBridge'] is False
                        for g,n,s,r in zip(context_signals,context_signal_native,context_signal_status,context_signal_retirements)))
        copy_verified = copy_workload_complete(log, args.driver_async_submit and async_verified)
        clear_verified = clear_workload_complete(log, args.driver_async_submit and async_verified)
        triangle_verified = triangle_workload_complete(log, args.driver_async_submit and async_verified)
        native_requested = args.runtime_workload in ('native', 'native-stream')
        metadata_bytes = native_texture_runtime_bytes(log, "\n".join(errors), requested=native_requested)
        shared_verified = shared_resource_import_complete(log, shared_probes, cleanup, triangle_verified, runtime_bytes=metadata_bytes)
        consume_verified = shared_texture_consume_complete(log, "\n".join(errors), cleanup, shared_verified,
                                                         presentation=args.driver_present_shared)
        hwqueue_flags = [int(flag or '0') for flag in re.findall(r'nativeHwQueueCreated=true privateBytes=\d+ progressFenceDirect=true(?: flags=(\d+))?',log)]
        hwqueue_flag_checks = [json.loads(line) for line in errors if line.startswith('{') and '"nativeHardwareQueueFlagsVerified"' in line]
        no_broadcast_queues = sum(bool(flag & 2) for flag in hwqueue_flags)
        no_broadcast_wait_queues = hwqueue_flags.count(6)
        accepted = (accepted and all(flag in (0,2,6) for flag in hwqueue_flags) and
                    cleanup.get('completedNoBroadcastSignalHwQueues',0) == no_broadcast_queues and
                    cleanup.get('completedNoBroadcastWaitHwQueues',0) == no_broadcast_wait_queues and
                    ((not hwqueue_flag_checks and not no_broadcast_queues) or
                     (len(hwqueue_flag_checks) == len(hwqueue_flags) == cleanup.get('completedHwQueues') and
                      all(n['flags'] == g and n['unchanged'] is True and n['ntstatus'] == 0 for n,g in zip(hwqueue_flag_checks,hwqueue_flags)))))
        if late_guest_eof:
            accepted = accepted and no_broadcast_queue_eof_complete(log, control, cleanup, 6 if args.hwqueue_no_broadcast_wait_eof_test else 2)
        elif args.runtime_workload in ('clear', 'triangle', 'shared', 'consume', 'native'):
            accepted = accepted and no_broadcast_queues >= 1 and len(context_signals) + len(hw_queue_signals) == (4 if args.runtime_workload in ('consume', 'native') else 2) and resources >= 2
        workload_matches = ((not clear_verified and not copy_verified and 'GPU_CLEAR_TEST_BEGIN' in log and 'GPU_COPY_TEST_BEGIN' not in log) if late_guest_eof else
                            (copy_verified and 'GPU_CLEAR_TEST_BEGIN' not in log) if args.runtime_workload == 'copy' else
                            (clear_verified and 'GPU_COPY_TEST_BEGIN' not in log) if args.runtime_workload == 'clear' else
                            (triangle_verified and ((consume_verified if args.runtime_workload in ('consume', 'native') else shared_verified and 'GPU_SHARED_CONSUME_TEST_BEGIN' not in log) if args.runtime_workload in ('shared', 'consume', 'native') else 'GPU_SHARED_RESOURCE_TEST_BEGIN' not in log) and
                             'GPU_COPY_TEST_BEGIN' not in log and 'GPU_CLEAR_TEST_BEGIN' not in log) if args.runtime_workload in ('triangle', 'shared', 'consume', 'native') else
                            'GPU_COPY_TEST_BEGIN' not in log and 'GPU_CLEAR_TEST_BEGIN' not in log)
        if args.runtime_workload not in ('triangle', 'shared', 'consume', 'native'):
            workload_matches = workload_matches and 'GPU_TRIANGLE_TEST_BEGIN' not in log
        accepted = (accepted and workload_matches and bool(metadata_bytes) and
                    (not native_requested or args.driver_native_metadata) and
                    (not args.driver_present_shared or (args.runtime_workload in ('consume', 'native') and consume_verified)))
        report = {'schema': 1, 'diagnosticAccepted': bool(accepted), 'runtimeInitializationComplete': bool(initialized and accepted),
                  'runtimeInitializationResultsSucceeded': bool(initialized),
                  'runtimeWorkload': args.runtime_workload, 'guestGpuBufferCopyVerified': bool(copy_verified and accepted),
                  'guestSharedGpuTextureImportVerified': bool(shared_verified and accepted),
                  'nativeSharedResourceImportChecks': shared_probes,
                  'nativeTextureMetadataVerified': bool(native_requested and metadata_bytes and consume_verified and accepted),
                  'nativeTextureMetadataChecks': [json.loads(line) for line in errors if line.startswith('{') and 'nativeTextureMetadata' in line],
                  'nativeSharedTextureConsumeVerified': bool(consume_verified and accepted),
                  'nativeSharedTexturePresentationVerified': bool(args.driver_present_shared and consume_verified and accepted),
                  'nativeSharedTextureChecks': [json.loads(line) for line in errors if line.startswith('{') and 'nativeSharedTexture' in line],
                  'nativeVendorResourcesCreatedByLiveRuntime': resources, 'nativeVendorResourceChecks': resource_native,
                  'nativeVendorDriverProtectionMaps': driver_protection, 'directGuestCpuFenceWaits': cpu_waits,
                  'nativeContextSignals': context_signals, 'nativeContextSignalRetirementChecks': context_signal_native,
                  'guestHwQueueSignals':hw_queue_signals,'nativeHwQueueSignalReturns':hw_signal_returned,
                  'nativeHwQueueSignalAcceptances':hw_signal_accepted,'nativeHwQueueSignalRetirements':hw_signal_retired,
                  'guestGpuBufferCopyBytesPerRound': 65536 if copy_verified and accepted else 0,
                  'guestGpuBufferCopyRounds': 2 if copy_verified and accepted else 0,
                  'guestGpuRenderTargetClearVerified': bool(clear_verified and accepted),
                  'guestGpuRenderTargetClearPixelsPerRound': 9490 if clear_verified and accepted else 0,
                  'guestGpuRenderTargetClearRounds': 2 if clear_verified and accepted else 0,
                  'guestGpuShaderTriangleVerified': bool(triangle_verified and accepted),
                  'guestGpuShaderTrianglePixelsPerRound': 9490 if triangle_verified and accepted else 0,
                  'guestGpuShaderTriangleRounds': 2 if triangle_verified and accepted else 0,
                  'nativeNoBroadcastSignalHardwareQueues': no_broadcast_queues,
                  'nativeNoBroadcastWaitHardwareQueues': no_broadcast_wait_queues,
                  'guestHardwareQueueFlags': hwqueue_flags, 'nativeHardwareQueueFlagChecks': hwqueue_flag_checks,
                  'd3d12DirectQueueHresult': next(iter(re.findall(r'^gpu(?:Clear|Triangle)DirectQueue=([0-9a-f]{8})[ \t\r]*$',log,re.MULTILINE)),None),
                  'stage': 'owned VM exit with a combined NoBroadcastSignal/NoBroadcastWait hardware queue; graphics rendering unverified' if args.hwqueue_no_broadcast_wait_eof_test else
                           'owned VM exit with a NoBroadcastSignal hardware queue; graphics rendering unverified' if late_guest_eof else
                           ('guest shader frames copied into native DXGI backbuffers with matching pixels and completed presentation statistics; Omarchy display integration unverified' if args.driver_present_shared else
                            'guest shared shader texture copied on the Windows GPU with matching native readback; presentation unverified') if args.runtime_workload in ('consume', 'native') else
                           'guest shader drawing in a shared texture and separate native Windows resource import; presentation unverified' if args.runtime_workload == 'shared' else
                           'live NVIDIA D3D12 shader triangle drawing with independently verified pixels' if args.runtime_workload == 'triangle' else
                           'live NVIDIA D3D12 graphics queue render-target clears with verified pixels' if args.runtime_workload == 'clear' else
                           'live NVIDIA D3D12 GPU buffer copies with verified guest readback' if args.runtime_workload == 'copy' else
                           'owned VM exit with a NoSignalMaxValueOnTdr fence and standalone source allocation' if args.sync_no_max_eof_test else
                           'live NVIDIA runtime with standalone source normalization and native TDR fence flags' if args.minimum_no_max_tdr_fences else
                           'owned VM exit with a spanning native CPU allocation view' if args.cpu_span_eof_test else
                           'live NVIDIA runtime with spanning native CPU allocation views' if args.minimum_spanning_cpu_mappings else
                           'owned VM exit with a retired GPU address-state mapping' if args.gpu_state_eof_test else
                           'live NVIDIA runtime with Windows GPU address-state mappings' if args.driver_gpu_state else
                           'owned VM exit before GPU reservation cleanup' if args.reservation_eof_test else
                           'live NVIDIA runtime with Windows GPU address reservation' if args.driver_reservation else
                           'owned VM exit before NoGPUAccess fence cleanup' if args.sync_eof_test else
                           'owned VM exit before native hardware queue and CPU lock cleanup' if args.hwqueue_eof_test else
                           'owned VM exit before native CPU lock cleanup' if args.cpu_eof_test else
                           'live NVIDIA runtime with native Windows synchronization objects' if args.driver_syncs else
                           'live NVIDIA runtime with native Windows hardware queues' if args.driver_hwqueues else
                           'live NVIDIA runtime with Windows allocation CPU locks' if args.driver_cpu else
                           'live NVIDIA runtime with Windows allocation residency' if args.driver_residency else
                           'live NVIDIA runtime with Windows GPU-address mappings' if args.driver_gpuva else
                           'live NVIDIA runtime with Windows video-memory allocation' if args.driver_allocations else
                           'live NVIDIA runtime with dynamic Windows paging fence mappings',
                  'hypervisor': 'QEMU/WHPX', 'hostBridgeExit': owner.returncode,
                  'liveNvidiaLinuxUmdLoaded': loaded, 'nativeKmtDeviceCreatedByLiveRuntime': 'deviceCreated=true' in log,
                  'nativeKmtContextsCreatedByLiveRuntime': contexts, 'nativePagingFenceMappings': fences,
                  'vendorAllocationOptIn': args.driver_allocations, 'nativeVendorAllocationsCreatedByLiveRuntime': allocations,
                  'minimumVendorAllocationsRequired': args.minimum_vendor_allocations,
                  'nativeUninitializedSourceAllocations': uninitialized_sources, 'nativeAllocationSourceNormalizations': source_normalizations,
                  'minimumUninitializedSourceAllocationsRequired': args.minimum_uninitialized_source_allocations,
                  'expectedAllocationObjectLimit': args.expected_allocation_limit,
                  'allocationLimitRejections': allocation_limit_rejections,
                  'allocationRetirementOptIn': args.driver_retirement,
                  'nativeAllocationRetirementWithQueuesVerified': bool(retirement_verified),
                  'guestAllocationRetirementBatches': retirement_batches,
                  'guestAllocationRetirementsWithHardwareQueues': retirements_with_queues,
                  'minimumRetirementsWithHardwareQueuesRequired': args.minimum_retirements_with_queues,
                  'allocationTranslationOptIn': args.driver_translation, 'guestAllocationDriverAliasesCreated': translations,
                  'minimumAllocationTranslationsRequired': args.minimum_allocation_translations,
                  'hardwareQueueOptIn': args.driver_hwqueues, 'nativeHardwareQueuesCreatedByLiveRuntime': hwqueues,
                  'minimumHardwareQueuesRequired': args.minimum_hwqueues, 'directHardwareQueueFences': hwqueue_fences,
                  'nativeHardwareQueueDestructionsAcknowledged': hwqueue_unmaps,
                  'synchronizationOptIn': args.driver_syncs, 'nativeSynchronizationTypesCreated': sync_types,
                  'nativeSynchronizationDescriptors': sync_descriptors, 'nativeNoGpuAccessFenceCount': len(no_gpu_fences),
                  'minimumNoGpuAccessFencesRequired': args.minimum_no_gpu_access_fences,
                  'nativeNoSignalMaxValueOnTdrFenceCount': len(no_max_fences), 'minimumNoMaxTdrFencesRequired': args.minimum_no_max_tdr_fences,
                  'gpuReservationOptIn': args.driver_reservation, 'nativeGpuReservationSizes': reservations,
                  'guestGpuReservationReleaseSizes': reservations_freed, 'minimumGpuReservationsRequired': args.minimum_gpu_reservations,
                  'gpuStateMappingOptIn': args.driver_gpu_state, 'nativeGpuStateMappings': gpu_states,
                  'minimumGpuStateMapsRequired': args.minimum_gpu_state_maps,
                  'nativeSynchronizationTypesDestroyedByGuest': sync_destroyed, 'directMonitoredFences': monitored_fences,
                  'minimumMonitoredFencesRequired': args.minimum_monitored_fences, 'minimumSynchronizationMutexesRequired': args.minimum_sync_mutexes,
                  'commandSubmissionOptIn': args.driver_submit, 'nativeCommandSubmissions': submissions,
                  'asynchronousSubmissionOptIn': args.driver_async_submit, 'guestCommandEnqueues': queued_submissions,
                  'nativeAsyncSubmissionAcceptances': async_accepted, 'nativeAsyncSubmissionRetirements': async_retired,
                  'nativeCommandPreflightChecks': submission_preflight,
                  'minimumSubmissionsRequired': args.minimum_submissions, 'directCommandRetirementChecks': command_retirements,
                  'vendorGpuVaOptIn': args.driver_gpuva, 'nativeVendorGpuVaMappings': gpuva,
                  'directGuestPagingRetirementChecks': retirements,
                  'minimumVendorGpuVaMappingsRequired': args.minimum_vendor_gpuva_maps,
                  'vendorResidencyOptIn': args.driver_residency, 'nativeVendorResidencyRequests': residency,
                  'minimumVendorResidencyRequestsRequired': args.minimum_vendor_residency_requests,
                  'vendorCpuOptIn': args.driver_cpu, 'nativeVendorCpuLocks': cpu,
                  'configuredCpuApertureSlots': args.driver_cpu_slots, 'guestCpuApertureLayouts': cpu_layouts,
                  'configuredFenceApertureSlots': args.driver_fence_slots or (128 if args.driver_cpu_slots >= 128 else 64),
                  'directCpuMappingsBeyondDefaultAperture': expanded_cpu,
                  'minimumExpandedCpuMappingsRequired': args.minimum_expanded_cpu_mappings,
                  'expectedCpuSlotQuotaRejections': args.expected_cpu_slot_quota_rejections,
                  'minimumVendorCpuLocksRequired': args.minimum_vendor_cpu_locks,
                  'spanningCpuMappings': spanning_cpu, 'minimumSpanningCpuMappingsRequired': args.minimum_spanning_cpu_mappings,
                  'nativeSubrangeCpuMappings': len(subrange_cpu), 'minimumSubrangeCpuMappingsRequired': args.minimum_subrange_cpu_mappings,
                  'wireObjectSummary': wire_summaries, 'wireQuotaRejections': wire_quotas,
                  'nativeContextPriorityChanges': context_priorities, 'minimumContextPriorityChangesRequired': args.minimum_context_priority_changes,
                  'guestCpuViewsUnmapped': cpu_unmaps, 'nativeVendorCpuLocksReleased': cpu_unlocks,
                  'cpuLocksRetainedUntilOwnedVmExit': cpu_released_after_exit,
                  'directGuestAllocationLoads': len(cpu) * 10000,
                  'diagnosticCpuStoresRequested': args.cpu_store_test,
                  'diagnosticCpuEofRequested': args.cpu_eof_test,
                  'diagnosticCpuSpanEofRequested': args.cpu_span_eof_test,
                  'diagnosticHardwareQueueEofRequested': args.hwqueue_eof_test,
                  'diagnosticNoBroadcastSignalQueueEofRequested': args.hwqueue_no_broadcast_eof_test,
                  'diagnosticNoBroadcastWaitQueueEofRequested': args.hwqueue_no_broadcast_wait_eof_test,
                  'diagnosticSynchronizationEofRequested': args.sync_eof_test,
                  'diagnosticNoMaxSynchronizationEofRequested': args.sync_no_max_eof_test,
                  'diagnosticReservationEofRequested': args.reservation_eof_test,
                  'diagnosticGpuStateEofRequested': args.gpu_state_eof_test,
                  'nativeCpuRegionChecks': cpu_regions,
                  'directGuestFenceLoads': (total_fence_mappings + len(retirements)) * 10000,
                  'expectedInitializationBoundary': None if guest_eof else args.expected_unimplemented_ioctl,
                  'firstUnsupportedIoctlAfterAcceptedStage': first_unsupported_after_stage,
                  'unsupportedIoctls': unsupported, 'completedNativeQueries': completed,
                  'd3d12DeviceHresult': result[1] if result else None, 'ownedQemuControl': control, 'disconnectCleanup': cleanup,
                  'd3d12CopyQueueHresult': copy_queue_result[1] if copy_queue_result else None,
                  'realWslDxgForwarding': False, 'capturedPrivateFixturesUsed': False,
                  'privateRuntimeImageRedistributable': False, 'kernelSha256': sha256(args.kernel),
                  'initramfsSha256': sha256(args.initramfs), 'driverBridgeSha256': sha256(args.bridge), 'qemuSha256': sha256(args.qemu),
                  'guestArbitraryGpuCommandSubmissionImplemented': False,
                  'boundedNativeCommandSubmissionVerified': bool(args.driver_submit and accepted and submissions),
                  'guestDesktopAcceleratedByThisBackend': False, 'nearNativePerformanceMeasured': False}
        if args.runtime_workload == 'native-stream':
            # Separate offline stream verifier owns this new milestone.
            accepted = False; report['diagnosticAccepted'] = False
            report['guestStreamChecks'] = [json.loads(line) for line in errors if line.startswith('{') and 'guestStream' in line]
        args.report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8', newline='\n')
        args.report.with_suffix('.host.log').write_text(''.join(output + errors), encoding='utf-8', newline='\n')
        for line in log.splitlines():
            if any(marker in line for marker in ('factory=', 'list=', 'nvidia vendor=', 'device=', 'LINUX_BRIDGE', 'LINUX_RUNTIME', 'BRIDGE_RUNTIME')):
                print(line)
        print(json.dumps(report, indent=2))
        return 0 if accepted else 1
    except Exception as error:
        if not args.report.exists():
            def last_metadata(lines, marker):
                return next((json.loads(line) for line in reversed(lines) if line.startswith('{') and marker in line), {})
            failure = {'schema':1, 'diagnosticAccepted':False, 'runtimeInitializationComplete':False,
                       'guestGpuBufferCopyVerified':False, 'guestGpuRenderTargetClearVerified':False, 'guestGpuShaderTriangleVerified':False,
                       'runtimeWorkload':args.runtime_workload, 'fatalDiagnosticError':str(error),
                       'hostBridgeExit':owner.poll(), 'observerErrors':list(observer_errors),
                       'ownedQemuControl':last_metadata(output,'ownedQemuExited'),
                       'disconnectCleanup':last_metadata(errors,'driverCleanupVerified'),
                       'kernelSha256':sha256(args.kernel), 'initramfsSha256':sha256(args.initramfs),
                       'driverBridgeSha256':sha256(args.bridge), 'qemuSha256':sha256(args.qemu),
                       'guestDesktopAcceleratedByThisBackend':False, 'nearNativePerformanceMeasured':False}
            args.report.write_text(json.dumps(failure,indent=2)+'\n',encoding='utf-8',newline='\n')
        raise
    finally:
        if not args.report.with_suffix('.host.log').exists():
            args.report.with_suffix('.host.log').write_text(''.join(output+errors),encoding='utf-8',newline='\n')
        if child_handle:
            api.CloseHandle(child_handle)
        # Do not kill a native page owner here: the timeout path verifies
        # child exit first. An unreaped child deliberately retains its owner.


if __name__ == '__main__':
    raise SystemExit(main())
