"""Reject false D3D12 initialization milestone reports; no GPU is used."""
import unittest
from test_owned_runtime_qemu import initialization_complete, copy_workload_complete, clear_workload_complete, expected_clear_pixels, no_broadcast_queue_eof_complete, async_submissions_complete, hw_queue_signals_complete
from triangle_evidence import triangle_workload_complete, triangle_reference, checksum, SHADERS


class MilestoneTests(unittest.TestCase):
    def setUp(self):
        self.log = '\n'.join(('factory=00000000', 'list=00000000', 'nvidia vendor=4318 device=11352',
            'nvidiaUmdPresentDuringPrivateQuery=true', 'LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true',
            'transport=virtio-port', 'realDxgForwarding=false', 'device=00000000', 'copyQueue=00000000', 'BRIDGE_RUNTIME_EXIT=0'))
        self.control = {'ownedQemuExited':True, 'qemuExit':0, 'qemuForcedStop':False, 'fenceControlFailed':False}
        self.cleanup = {'driverCleanupVerified':True}

    def test_all_conditions_required(self):
        self.assertTrue(initialization_complete(self.log, 0, self.control, self.cleanup))
        for line in self.log.splitlines():
            with self.subTest(missing=line):
                self.assertFalse(initialization_complete(self.log.replace(line, ''), 0, self.control, self.cleanup))
        for key in self.control:
            control = dict(self.control); del control[key]
            self.assertFalse(initialization_complete(self.log, 0, control, self.cleanup))
        self.assertFalse(initialization_complete(self.log, 1, self.control, self.cleanup))
        self.assertFalse(initialization_complete(self.log, 0, self.control, {}))

    def test_partial_or_failed_device_and_queue(self):
        for old, new in (('device=00000000', 'device=8007000e'), ('copyQueue=00000000', 'copyQueue=80004001'),
                         ('BRIDGE_RUNTIME_EXIT=0', 'BRIDGE_RUNTIME_EXIT=1'), ('device=00000000', 'device=000000001')):
            self.assertFalse(initialization_complete(self.log.replace(old, new), 0, self.control, self.cleanup))
        self.assertFalse(initialization_complete('host device=00000000\ncopyQueue=00000000', 0, self.control, self.cleanup))
        for extra in ('device=80004005', 'copyQueue=80004001', 'BRIDGE_RUNTIME_EXIT=1', 'device=00000000'):
            self.assertFalse(initialization_complete(self.log + '\n' + extra, 0, self.control, self.cleanup))


class CopyEvidenceTests(unittest.TestCase):
    def setUp(self):
        lines = ['GPU_COPY_TEST_BEGIN bytes=65536 rounds=2', 'nativeCommandSubmitted=true', 'nativeCommandSubmitted=true']
        for stage, count in (('Upload',1), ('Default',1), ('Readback',1), ('Allocator',1), ('CommandList',1), ('Fence',1),
                             ('UploadMap',2), ('ReadbackMap',2), ('Close',2), ('Signal',2), ('AllocatorReset',1), ('CommandListReset',1)):
            lines.extend([f'gpuCopy{stage}=00000000'] * count)
        for round_, checksum in ((1,'28e3f2dd58641325'), (2,'3ce54625a5a41325')):
            lines.append(f'GPU_COPY_ROUND round={round_} verifiedBytes=65536 expectedHash={checksum} observedHash={checksum} fenceTarget={round_} fenceObserved={round_}')
        lines.append('GPU_COPY_TEST_COMPLETE verified=true bytes=65536 rounds=2')
        self.log = '\n'.join(lines)

    def test_async_commands_require_separately_verified_retirement(self):
        log = self.log.replace('nativeCommandSubmitted=true', 'nativeCommandQueued=true')
        self.assertFalse(copy_workload_complete(log))
        self.assertTrue(copy_workload_complete(log, True))
        for old, new in (('observedHash=28e3f2dd58641325', 'observedHash=0000000000000000'),
                         ('gpuCopySignal=00000000', 'gpuCopySignal=80004001'),
                         ('nativeCommandQueued=true', 'missingCommand=true')):
            self.assertFalse(copy_workload_complete(log.replace(old, new), True))

    def test_no_cpu_only_or_partial_success(self):
        self.assertTrue(copy_workload_complete(self.log))
        for line in self.log.splitlines():
            with self.subTest(missing=line):
                self.assertFalse(copy_workload_complete(self.log.replace(line, '', 1)))
        for old,new in (('verifiedBytes=65536','verifiedBytes=0'), ('gpuCopySignal=00000000','gpuCopySignal=80004001'),
                        ('observedHash=28e3f2dd58641325','observedHash=0000000000000000'),
                        ('expectedHash=28e3f2dd58641325','expectedHash=0000000000000000'),
                        ('fenceObserved=2','fenceObserved=1'), ('fenceObserved=2','fenceObserved=18446744073709551615')):
            self.assertFalse(copy_workload_complete(self.log.replace(old,new)))

    def test_duplicate_or_out_of_order_evidence_rejected(self):
        for line in self.log.splitlines():
            if line.startswith(('GPU_COPY_TEST','GPU_COPY_ROUND','gpuCopy')):
                self.assertFalse(copy_workload_complete(self.log + '\n' + line))
        self.assertFalse(copy_workload_complete(self.log.replace('round=1 verifiedBytes','round=2 verifiedBytes')))


class ClearEvidenceTests(unittest.TestCase):
    def setUp(self):
        lines = ['GPU_CLEAR_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2',
                 'nativeCommandSubmitted=true', 'nativeCommandSubmitted=true',
                 'GPU_CLEAR_FOOTPRINT width=130 height=73 offset=512 rowPitch=768 rowBytes=520 rows=73 totalBytes=56328']
        for stage, count in (('DirectQueue',1), ('RenderTarget',1), ('Readback',1), ('RtvHeap',1), ('Allocator',1),
                             ('CommandList',1), ('Fence',1), ('Close',2), ('Signal',2), ('ReadbackMap',2),
                             ('AllocatorReset',1), ('CommandListReset',1)):
            lines.extend([f'gpuClear{stage}=00000000'] * count)
        for round_ in (1,2):
            checksum = 14695981039346656037
            for byte in expected_clear_pixels(round_):
                checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
            lines.append(f'GPU_CLEAR_ROUND round={round_} verifiedPixels=9490 expectedHash={checksum:016x} observedHash={checksum:016x} fenceTarget={round_} fenceObserved={round_}')
        pixels = expected_clear_pixels(2)
        lines.extend(f'GPU_CLEAR_PIXEL_ROW round=2 y={y} rgba={pixels[y*520:(y+1)*520].hex()}' for y in range(73))
        lines.append('GPU_CLEAR_TEST_COMPLETE verified=true width=130 height=73 rounds=2')
        self.log = '\n'.join(lines)

    def test_async_commands_require_separately_verified_retirement(self):
        log=self.log.replace('nativeCommandSubmitted=true','nativeCommandQueued=true')
        self.assertFalse(clear_workload_complete(log))
        self.assertTrue(clear_workload_complete(log,True))
        self.assertFalse(clear_workload_complete(log.replace('rgba=0000ffff','rgba=ffffffff'),True))
        self.assertFalse(clear_workload_complete(log.replace('gpuClearSignal=00000000','gpuClearSignal=80004001'),True))
        self.assertFalse(clear_workload_complete(log.replace('nativeCommandQueued=true','missingCommand=true'),True))

    def test_complete_graphics_evidence_required(self):
        self.assertTrue(clear_workload_complete(self.log))
        self.assertFalse(copy_workload_complete(self.log))
        for line in self.log.splitlines():
            with self.subTest(missing=line[:70]):
                self.assertFalse(clear_workload_complete(self.log.replace(line, '', 1)))

    def test_incorrect_layout_pixels_fence_or_status_rejected(self):
        for old,new in (('rowPitch=768','rowPitch=520'), ('totalBytes=56328','totalBytes=4096'), ('offset=512','offset=0'),
                        ('rowBytes=520','rowBytes=512'), ('verifiedPixels=9490','verifiedPixels=0'),
                        ('gpuClearSignal=00000000','gpuClearSignal=80004001'),
                        ('fenceObserved=2','fenceObserved=1'), ('fenceObserved=2','fenceObserved=18446744073709551615'),
                        ('rgba=0000ffff','rgba=ffffffff'), ('y=72 rgba=','y=71 rgba=')):
            self.assertFalse(clear_workload_complete(self.log.replace(old,new)))
        self.assertFalse(clear_workload_complete(self.log + '\ngpuClearDeviceRemoved=887a0005'))

    def test_duplicate_out_of_order_or_stale_pixels_rejected(self):
        for line in self.log.splitlines():
            if line.startswith(('GPU_CLEAR', 'gpuClear')):
                self.assertFalse(clear_workload_complete(self.log + '\n' + line))
        self.assertFalse(clear_workload_complete(self.log.replace('round=1 verifiedPixels','round=2 verifiedPixels')))
        self.assertFalse(clear_workload_complete(self.log.replace('GPU_CLEAR_PIXEL_ROW round=2','GPU_CLEAR_PIXEL_ROW round=1')))


class TriangleEvidenceTests(unittest.TestCase):
    """Synthetic evidence tests only; passing these never certifies a GPU run."""
    def setUp(self):
        self.frames = [triangle_reference(r)[0] for r in (1,2)]
        self.log = self.make_log(self.frames)

    @staticmethod
    def make_log(frames):
        lines = ['GPU_TRIANGLE_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2 vertices=3 tolerance=2',
                 'GPU_TRIANGLE_SHADERS vertexBytes={} pixelBytes={} vertexHash={} pixelHash={}'.format(*SHADERS),
                 'GPU_TRIANGLE_FOOTPRINT width=130 height=73 offset=512 rowPitch=768 rowBytes=520 rows=73 totalBytes=56328',
                 'nativeCommandSubmitted=true', 'nativeCommandSubmitted=true']
        for stage, count in (('DirectQueue',1), ('SerializeRootSignature',1), ('RootSignature',1), ('Pipeline',1),
                             ('RenderTarget',1), ('Readback',1), ('RtvHeap',1), ('Allocator',1), ('CommandList',1),
                             ('Fence',1), ('Close',2), ('Signal',2), ('ReadbackMap',2), ('AllocatorReset',1), ('CommandListReset',1)):
            lines.extend([f'gpuTriangle{stage}=00000000'] * count)
        for r, actual in enumerate(frames, 1):
            expected, coverage = triangle_reference(r)
            maximum = max(abs(a-b) for a,b in zip(expected,actual))
            lines.append(f'GPU_TRIANGLE_DRAW round={r} vertices=3 startVertex=0 instances=1 colorRotation={r-1}')
            lines.append(f'GPU_TRIANGLE_ROUND round={r} verifiedPixels=9490 trianglePixels={sum(coverage)} backgroundPixels={9490-sum(coverage)} maxChannelError={maximum} referenceHash={checksum(expected)} observedHash={checksum(actual)} fenceTarget={r} fenceObserved={r}')
            lines.extend(f'GPU_TRIANGLE_PIXEL_ROW round={r} y={y} rgba={actual[y*520:(y+1)*520].hex()}' for y in range(73))
        lines.append('GPU_TRIANGLE_TEST_COMPLETE verified=true width=130 height=73 rounds=2')
        return '\n'.join(lines)

    def test_shader_pipeline_every_row_and_both_rounds_required(self):
        self.assertTrue(triangle_workload_complete(self.log))
        self.assertFalse(clear_workload_complete(self.log))
        self.assertFalse(copy_workload_complete(self.log))
        for line in self.log.splitlines():
            with self.subTest(missing=line[:75]):
                self.assertFalse(triangle_workload_complete(self.log.replace(line, '', 1)))

    def test_only_interpolated_rgb_has_bounded_tolerance(self):
        _, coverage = triangle_reference(1)
        inside = coverage.index(True) * 4
        for difference, accepted in ((1,True), (2,True), (3,False)):
            frames = [bytearray(f) for f in self.frames]
            frames[0][inside] += difference
            self.assertEqual(triangle_workload_complete(self.make_log(frames)), accepted)
        for offset in (0, 3, inside + 3):
            frames = [bytearray(f) for f in self.frames]
            frames[0][offset] ^= 1
            self.assertFalse(triangle_workload_complete(self.make_log(frames)))

    def test_stale_cleared_or_flipped_frames_rejected_even_with_valid_hashes(self):
        for first in (self.frames[1], expected_clear_pixels(1),
                      b''.join(self.frames[0][y*520:(y+1)*520] for y in reversed(range(73)))):
            self.assertFalse(triangle_workload_complete(self.make_log([first,self.frames[1]])))
        frames = list(self.frames)
        frames[1] = frames[0]
        self.assertFalse(triangle_workload_complete(self.make_log(frames)))

    def test_wrong_shader_status_layout_or_fence_rejected(self):
        for old,new in (('gpuTrianglePipeline=00000000','gpuTrianglePipeline=80004001'),
                        (f'vertexHash={SHADERS[2]}','vertexHash=0000000000000000'),
                        (f'pixelBytes={SHADERS[1]}',f'pixelBytes={SHADERS[0]}'), ('colorRotation=1','colorRotation=0'),
                        ('rowPitch=768','rowPitch=520'), ('offset=512','offset=0'),
                        ('totalBytes=56328','totalBytes=4096'), ('fenceObserved=2','fenceObserved=1'),
                        ('fenceObserved=2','fenceObserved=18446744073709551615'),
                        ('maxChannelError=0','maxChannelError=2'), ('tolerance=2','tolerance=255'),
                        ('verifiedPixels=9490','verifiedPixels=9489')):
            self.assertFalse(triangle_workload_complete(self.log.replace(old,new)))
        for extra in ('GPU_TRIANGLE_TIMEOUT round=2', 'gpuTriangleDeviceRemoved=887a0005',
                      'GPU_TRIANGLE_PIXEL_ROW round=2 y=1 rgba=INVALID'):
            self.assertFalse(triangle_workload_complete(self.log+'\n'+extra))

    def test_duplicates_out_of_order_and_forged_hashes_rejected(self):
        for line in self.log.splitlines():
            if line.startswith(('GPU_TRIANGLE','gpuTriangle')):
                self.assertFalse(triangle_workload_complete(self.log+'\n'+line))
        self.assertFalse(triangle_workload_complete(self.log.replace('GPU_TRIANGLE_PIXEL_ROW round=2','GPU_TRIANGLE_PIXEL_ROW round=1')))
        self.assertFalse(triangle_workload_complete(self.log.replace('y=72 rgba=', 'y=71 rgba=')))
        self.assertFalse(triangle_workload_complete(self.log.replace('observedHash='+checksum(self.frames[0]), 'observedHash=0000000000000000')))
        self.assertFalse(triangle_workload_complete(self.log.replace('referenceHash='+checksum(self.frames[0]), 'referenceHash=0000000000000000')))
        stage = 'gpuTrianglePipeline=00000000'
        self.assertFalse(triangle_workload_complete(stage+'\n'+self.log.replace(stage+'\n','')))

    def test_async_acceptance_requires_separate_retirement_proof(self):
        log = self.log.replace('nativeCommandSubmitted=true','nativeCommandQueued=true')
        self.assertFalse(triangle_workload_complete(log))
        self.assertTrue(triangle_workload_complete(log,True))
        self.assertFalse(triangle_workload_complete(log.replace('nativeCommandQueued=true','missing=true'),True))


class NoBroadcastQueueEofTests(unittest.TestCase):
    def setUp(self):
        self.log = '\n'.join(('device=00000000', 'copyQueue=00000000',
            'GPU_CLEAR_TEST_BEGIN width=130 height=73 format=R8G8B8A8_UNORM rounds=2',
            'LINUX_BRIDGE guestFenceUnmapped=true',
            'LINUX_BRIDGE allocationCpuUnmapped=true direct=true mmioReads=0 mmioWrites=0 bytes=1052672',
            'LINUX_BRIDGE nativeHwQueueCreated=true privateBytes=144 progressFenceDirect=true flags=2',
            'LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags=2', 'BRIDGE_RUNTIME_EXIT=1'))
        self.control = {'ownedQemuExited':True, 'qemuExit':0, 'qemuForcedStop':False, 'fenceControlFailed':False,
                        'fenceMappingsCreated':2, 'fenceUnmapAcknowledgements':1,
                        'allocationUnmapAcknowledgements':1, 'allocationSlotUnmapAcknowledgements':2}
        self.cleanup = {'driverCleanupVerified':True, 'completedNoBroadcastSignalHwQueues':1, 'hwQueuesReleasedAfterVmExit':1}

    def test_actual_unmap_acks_and_retained_queue_required(self):
        self.assertTrue(no_broadcast_queue_eof_complete(self.log, self.control, self.cleanup))
        for key in self.control:
            control = dict(self.control); del control[key]
            self.assertFalse(no_broadcast_queue_eof_complete(self.log, control, self.cleanup))
        for key in self.cleanup:
            cleanup = dict(self.cleanup); del cleanup[key]
            self.assertFalse(no_broadcast_queue_eof_complete(self.log, self.control, cleanup))
        for key in ('fenceUnmapAcknowledgements','allocationUnmapAcknowledgements','allocationSlotUnmapAcknowledgements'):
            for bad in (-1,0,3):
                control = dict(self.control); control[key] = bad
                self.assertFalse(no_broadcast_queue_eof_complete(self.log, control, self.cleanup))

    def test_wrong_exit_flag_or_workload_is_rejected(self):
        for old,new in (('flags=2','flags=0'), ('BRIDGE_RUNTIME_EXIT=1','BRIDGE_RUNTIME_EXIT=0'),
                        ('bytes=1052672','bytes=1052673'), ('bytes=1052672','bytes=4198400'),
                        ('copyQueue=00000000','copyQueue=80004005')):
            self.assertFalse(no_broadcast_queue_eof_complete(self.log.replace(old,new), self.control, self.cleanup))
        for extra in ('GPU_COPY_TEST_BEGIN', 'gpuClearDirectQueue=00000000', 'GPU_CLEAR_ROUND', 'GPU_CLEAR_TEST_COMPLETE',
                      'LINUX_BRIDGE hwQueueEofTest=true exitingWithQueueOwned=true flags=2'):
            self.assertFalse(no_broadcast_queue_eof_complete(self.log+'\n'+extra, self.control, self.cleanup))
        self.assertFalse(clear_workload_complete(self.log))
        self.assertFalse(copy_workload_complete(self.log))
        self.assertFalse(initialization_complete(self.log, 0, self.control, self.cleanup))

    def test_combined_wait_queue_requires_exact_native_counts(self):
        log = self.log.replace('flags=2', 'flags=6')
        prior = 'LINUX_BRIDGE nativeHwQueueCreated=true privateBytes=144 progressFenceDirect=true flags=2\n'
        log = prior * 2 + log
        cleanup = dict(self.cleanup, completedNoBroadcastSignalHwQueues=3, completedNoBroadcastWaitHwQueues=1)
        self.assertTrue(no_broadcast_queue_eof_complete(log, self.control, cleanup, 6))
        self.assertFalse(no_broadcast_queue_eof_complete(log, self.control, cleanup))
        self.assertFalse(no_broadcast_queue_eof_complete(self.log, self.control, self.cleanup, 6))
        for flag in (0, 1, 4, 5, 7, 8):
            self.assertFalse(no_broadcast_queue_eof_complete(log, self.control, cleanup, flag))
        for key in ('completedNoBroadcastSignalHwQueues','completedNoBroadcastWaitHwQueues'):
            for bad in (-1,0,2,4,None):
                changed = dict(cleanup); changed[key] = bad
                self.assertFalse(no_broadcast_queue_eof_complete(log, self.control, changed, 6))
        extra = prior.replace('flags=2', 'flags=6')
        self.assertFalse(no_broadcast_queue_eof_complete(log + '\n' + extra, self.control, cleanup, 6))
        self.assertFalse(clear_workload_complete(log))
        self.assertFalse(copy_workload_complete(log))
        self.assertFalse(initialization_complete(log, 0, self.control, cleanup))


class AsyncSubmissionTests(unittest.TestCase):
    def setUp(self):
        self.queued = [{'bytes':4096,'privateBytes':280,'target':5,'observedAtReturn':0},
                       {'bytes':8192,'privateBytes':1880,'target':2,'observedAtReturn':1}]
        self.accepted = [dict(index=i,bytes=q['bytes'],privateBytes=q['privateBytes'],target=q['target'],observed=q['observedAtReturn'])
                         for i,q in enumerate(self.queued,1)]
        self.retired = [dict(a,observed=a['target']) for a in reversed(self.accepted)]
        self.cleanup = {'asynchronousSubmissionOptIn':True,'acceptedAsyncSubmissions':2,'completedNativeSubmissions':2,
                        'pendingNativeSubmissions':0,'peakPendingNativeSubmissions':2}

    def test_independent_fences_can_complete_out_of_order(self):
        self.assertTrue(async_submissions_complete(self.queued,self.accepted,self.retired,self.cleanup))

    def test_enqueue_is_not_completion(self):
        for receipts in ([],self.retired[:1],[self.retired[0]]*2):
            self.assertFalse(async_submissions_complete(self.queued,self.accepted,receipts,self.cleanup))
        for key in self.cleanup:
            cleanup = dict(self.cleanup); del cleanup[key]
            self.assertFalse(async_submissions_complete(self.queued,self.accepted,self.retired,cleanup))
        for key,value in (('pendingNativeSubmissions',1),('completedNativeSubmissions',1),('acceptedAsyncSubmissions',3),
                          ('asynchronousSubmissionOptIn',False),('peakPendingNativeSubmissions',0)):
            cleanup = dict(self.cleanup); cleanup[key] = value
            self.assertFalse(async_submissions_complete(self.queued,self.accepted,self.retired,cleanup))

    def test_wrong_stale_or_device_lost_receipts_rejected(self):
        for key,value in (('bytes',4096),('privateBytes',280),('target',3),('observed',0),('observed',(1<<64)-1)):
            receipts = [dict(r) for r in self.retired]; receipts[0][key] = value
            self.assertFalse(async_submissions_complete(self.queued,self.accepted,receipts,self.cleanup))
        self.assertFalse(async_submissions_complete(self.queued,list(reversed(self.accepted)),self.retired,self.cleanup))
        receipts = [dict(a) for a in self.accepted]; receipts[0]['observed'] = 1
        self.assertFalse(async_submissions_complete(self.queued,receipts,self.retired,self.cleanup))
        self.assertFalse(async_submissions_complete([],[],[],{}))


class HwQueueSignalTests(unittest.TestCase):
    def setUp(self):
        self.queued=[{'flags':4,'queues':3,'target':n,'observedAtReturn':n-1} for n in (1,2)]
        self.returned=[dict(flags=q['flags'],queues=q['queues'],target=q['target'],observed=q['observedAtReturn'],ntstatus=0) for q in self.queued]
        self.accepted=[dict(index=i,flags=q['flags'],queues=q['queues'],target=q['target'],observed=q['observedAtReturn'],
                            noGpuAccess=True,cpuValueWrittenByBridge=False) for i,q in enumerate(self.queued,1)]
        self.retired=[dict(a,observed=a['target']) for a in reversed(self.accepted)]
        self.cleanup={'nativeHwQueueSignalAttempts':2,'acceptedHwQueueSignals':2,'completedHwQueueSignals':2,
                      'failedHwQueueSignals':0,'pendingHwQueueSignals':0,'peakPendingHwQueueSignals':1}

    def test_requires_actual_retirement_without_cpu_fence_writes(self):
        self.assertTrue(hw_queue_signals_complete(self.queued,self.returned,self.accepted,self.retired,self.cleanup))
        for receipts in ([],self.retired[:1],[self.retired[0]]*2):
            self.assertFalse(hw_queue_signals_complete(self.queued,self.returned,self.accepted,receipts,self.cleanup))
        for key,value in (('observed',1),('observed',(1<<64)-1),('target',3),('flags',0),('queues',2),
                          ('noGpuAccess',False),('cpuValueWrittenByBridge',True)):
            receipts=[dict(r) for r in self.retired]; receipts[0][key]=value
            self.assertFalse(hw_queue_signals_complete(self.queued,self.returned,self.accepted,receipts,self.cleanup))
        for key in self.cleanup:
            cleanup=dict(self.cleanup); del cleanup[key]
            self.assertFalse(hw_queue_signals_complete(self.queued,self.returned,self.accepted,self.retired,cleanup))
        for status in (-123,259):
            returned=[dict(r) for r in self.returned]; returned[0]['ntstatus']=status
            self.assertFalse(hw_queue_signals_complete(self.queued,returned,self.accepted,self.retired,self.cleanup))

    def test_empty_and_partial_signal_evidence(self):
        self.assertTrue(hw_queue_signals_complete([],[],[],[],{}))
        self.assertFalse(hw_queue_signals_complete([],self.returned,[],[],{}))
        self.assertFalse(hw_queue_signals_complete([],[],[],[],{'pendingHwQueueSignals':1}))
        self.assertFalse(hw_queue_signals_complete(self.queued,[],self.accepted,self.retired,self.cleanup))
        self.assertFalse(hw_queue_signals_complete(self.queued,self.returned,list(reversed(self.accepted)),self.retired,self.cleanup))


if __name__ == '__main__':
    unittest.main()
