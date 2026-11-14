"""Synthetic rejection controls; these are not hardware acceptance."""
import copy
import json
import unittest
from triangle_evidence import triangle_reference, checksum
from shared_texture_evidence import shared_texture_consume_complete


class TextureConsumeTests(unittest.TestCase):
    def setUp(self):
        self.frames = [triangle_reference(r)[0] for r in (1, 2)]
        guest = ['GPU_SHARED_CONSUME_TEST_BEGIN rounds=2 state=COMMON']
        rows = []
        self.records = [dict(nativeSharedTextureConsumerOpened=True, ntstatus=0, hresult=0, sharedNtHandleClosed=True)]
        for r, frame in enumerate(self.frames, 1):
            if r == 2:
                guest.append('GPU_TRIANGLE_DRAW round=2 vertices=3 startVertex=0 instances=1 colorRotation=1')
            for y in range(73):
                rgba = frame[y * 520:(y + 1) * 520].hex()
                guest.append(f'GPU_TRIANGLE_PIXEL_ROW round={r} y={y} rgba={rgba}')
                rows.append(f'NATIVE_SHARED_TEXTURE_PIXEL_ROW round={r} y={y} rgba={rgba}')
            guest.extend((f'GPU_SHARED_CONSUME_HANDOFF round={r} state=COMMON fenceTarget={r} fenceObserved={r}',
                          f'GPU_SHARED_CONSUME_HOST round={r} hash={checksum(frame)} returnedState=COMMON'))
            self.records.extend((dict(nativeSharedTextureCopied=True, round=r, width=130, height=73,
                gpuFenceTarget=r, gpuFenceObserved=r, fnv1a=int(checksum(frame), 16), sourceReturnedToCommon=True, cpuUpload=False, presented=False),
                dict(nativeSharedTextureHandoff=True, target=r, observed=r, nativeSignalAccepted=True,
                     sourceState='COMMON', returnedState='COMMON', pendingGuestCommands=0)))
        guest.append('GPU_SHARED_CONSUME_TEST_COMPLETE verified=true rounds=2')
        self.records.append(dict(nativeSharedTextureConsumerReleased=True, gpuWorkRetired=True))
        self.guest = '\n'.join(guest); self.rows = '\n'.join(rows)
        self.cleanup = dict(driverCleanupVerified=True, openedTextureConsumers=1, releasedTextureConsumers=1,
            liveTextureConsumers=0, completedSharedTextureConsumes=2, failedSharedTextureConsumes=0,
            acceptedAsyncSubmissions=14, completedNativeSubmissions=14, pendingNativeSubmissions=0,
            acceptedHwQueueSignals=4, completedHwQueueSignals=4, pendingHwQueueSignals=0, cleanupFailures=0)

    def check(self, guest=None, rows=None, records=None, cleanup=None, shared=True):
        host = (self.rows if rows is None else rows) + '\n' + '\n'.join(json.dumps(r) for r in (self.records if records is None else records))
        return shared_texture_consume_complete(self.guest if guest is None else guest, host,
                                               self.cleanup if cleanup is None else cleanup, shared)

    def test_both_frames_and_shared_prerequisite(self):
        self.assertTrue(self.check())
        self.assertFalse(self.check(shared=False))
        self.assertFalse(self.check(shared=1))
        self.assertFalse(self.check(rows=''))

    def test_native_pixels_not_guest_markers_alone(self):
        self.assertFalse(self.check(rows=self.rows.replace('rgba=', 'rgba=00', 1)))
        self.assertFalse(self.check(rows=self.rows.replace('rgba=0000ff', 'rgba=ff00ff', 1)))
        self.assertFalse(self.check(rows=self.rows + '\n' + self.rows.splitlines()[0]))
        lines = self.rows.splitlines(); lines[0], lines[1] = lines[1], lines[0]
        self.assertFalse(self.check(rows='\n'.join(lines)))

    def test_retirement_and_metadata_types(self):
        for index, record in enumerate(self.records):
            for key, value in record.items():
                if key.startswith('nativeSharedTexture'):
                    continue
                for bad in (None, not value if type(value) is bool else value + 1 if type(value) is int else 'wrong',
                            int(value) if type(value) is bool else str(value)):
                    if type(bad) is type(value) and bad == value:
                        continue
                    if key in ('observed', 'gpuFenceObserved') and type(bad) is int and bad > value:
                        continue
                    changed = copy.deepcopy(self.records); changed[index][key] = bad
                    self.assertFalse(self.check(records=changed), (index, key, bad))

    def test_lifecycle_and_submission_completion(self):
        self.assertFalse(self.check(records=self.records[:-1]))
        self.assertFalse(self.check(records=self.records + self.records[:1]))
        for key, value in self.cleanup.items():
            changed = dict(self.cleanup); changed[key] = None
            self.assertFalse(self.check(cleanup=changed), key)
        changed = dict(self.cleanup); changed['completedNativeSubmissions'] = 13
        self.assertFalse(self.check(cleanup=changed))
        changed = dict(self.cleanup); changed['acceptedAsyncSubmissions'] = changed['completedNativeSubmissions'] = 15
        self.assertTrue(self.check(cleanup=changed))

    def test_handoff_order_state_and_acknowledgment(self):
        for text in ('GPU_SHARED_CONSUME_HANDOFF', 'GPU_SHARED_CONSUME_HOST', 'state=COMMON', 'returnedState=COMMON'):
            self.assertFalse(self.check(guest=self.guest.replace(text, 'wrong', 1)), text)
        self.assertFalse(self.check(guest=self.guest + '\nGPU_SHARED_CONSUME_UNKNOWN'))
        changed = self.guest.splitlines()
        a = next(i for i, line in enumerate(changed) if line.startswith('GPU_SHARED_CONSUME_HANDOFF'))
        changed[a], changed[a + 1] = changed[a + 1], changed[a]
        self.assertFalse(self.check(guest='\n'.join(changed)))

    def test_native_failures_and_record_reordering(self):
        self.assertFalse(self.check(records=self.records + [dict(nativeSharedTextureConsumeFailed=True)]))
        changed = copy.deepcopy(self.records); changed[1], changed[2] = changed[2], changed[1]
        self.assertFalse(self.check(records=changed))
        changed = copy.deepcopy(self.records); changed[1]['gpuFenceObserved'] = (1 << 64) - 1
        self.assertFalse(self.check(records=changed))


class TexturePresentationTests(unittest.TestCase):
    def setUp(self):
        self.consume = TextureConsumeTests(); self.consume.setUp()
        opened = dict(nativeSharedTexturePresenterOpened=True, width=130, height=73, bufferCount=2,
                      flipModel=True, clientWidth=780, clientHeight=438, windowVisible=True, sameAdapter=True, cpuUpload=False)
        released = dict(nativeSharedTexturePresenterReleased=True, gpuWorkRetired=True, windowDestroyed=True, classUnregistered=True)
        self.records = copy.deepcopy(self.consume.records)
        self.records.insert(1, opened)
        for i in (1, 2):
            self.records.insert(i * 3, dict(nativeSharedTexturePresented=True, round=i, width=130, height=73,
                backBufferIndex=i - 1, backBufferFnv1a=int(checksum(self.consume.frames[i - 1]), 16),
                presentHresult=0, lastPresentCount=i, statisticsHresult=0, statisticsPresentCount=i,
                syncQpc=i * 100, dwmFlushHresult=0, windowVisible=True, presentQueueFenceTarget=i,
                presentQueueFenceObserved=i, backBufferState='PRESENT', cpuUpload=False))
        self.records.insert(-1, released)
        self.rows = self.consume.rows.replace('NATIVE_SHARED_TEXTURE_PIXEL_ROW', 'NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW')

    def check(self, records=None, rows=None, presentation=True, cleanup=None):
        host = self.consume.rows + '\n' + (self.rows if rows is None else rows) + '\n' + '\n'.join(
            json.dumps(r) for r in (self.records if records is None else records))
        return shared_texture_consume_complete(self.consume.guest, host,
            self.consume.cleanup if cleanup is None else cleanup, True, presentation=presentation)

    def test_presentation_and_consumption_are_separate(self):
        self.assertTrue(self.check())
        self.assertFalse(self.check(presentation=False))
        self.assertFalse(self.check(presentation=1))
        self.assertFalse(self.check(records=self.consume.records, rows=''))

    def test_backbuffer_pixels_are_required(self):
        for rows in ('', self.rows.replace('rgba=', 'rgba=ff', 1), self.rows + '\n' + self.rows.splitlines()[0],
                     self.rows.replace('round=2', 'round=1')):
            self.assertFalse(self.check(rows=rows))

    def test_queued_or_occluded_is_not_complete(self):
        for index in (3, 6):
            for key, bad in (('presentHresult', 1), ('presentHresult', 142213121), ('statisticsHresult', -1),
                             ('statisticsPresentCount', 0), ('lastPresentCount', 0), ('windowVisible', False),
                             ('syncQpc', 0), ('syncQpc', True), ('nativeSharedTexturePresented', False),
                             ('presentQueueFenceObserved', 0), ('backBufferState', 'COPY_DEST'), ('cpuUpload', True)):
                records = copy.deepcopy(self.records); records[index][key] = bad
                self.assertFalse(self.check(records=records), (index, key, bad))

    def test_all_presentation_metadata_types(self):
        for index in (1, 3, 6, 8):
            for key, value in self.records[index].items():
                for bad in (None, str(value), int(value) if type(value) is bool else True):
                    if type(bad) is type(value) and bad == value:
                        continue
                    records = copy.deepcopy(self.records); records[index][key] = bad
                    self.assertFalse(self.check(records=records), (index, key, bad))

    def test_buffer_rotation_order_and_release(self):
        for first, second in ((1, 2), (3, 4), (6, 7), (8, 9)):
            records = copy.deepcopy(self.records); records[first], records[second] = records[second], records[first]
            self.assertFalse(self.check(records=records))
        records = copy.deepcopy(self.records); records[6]['backBufferIndex'] = records[3]['backBufferIndex']
        self.assertFalse(self.check(records=records))
        records = copy.deepcopy(self.records); records[6]['syncQpc'] = records[3]['syncQpc']
        self.assertFalse(self.check(records=records))
        self.assertFalse(self.check(records=self.records[:-2] + self.records[-1:]))

    def test_guest_retirement_still_required(self):
        cleanup = dict(self.consume.cleanup); cleanup['pendingHwQueueSignals'] = 1
        self.assertFalse(self.check(cleanup=cleanup))
        cleanup = dict(self.consume.cleanup); cleanup['driverCleanupVerified'] = False
        self.assertFalse(self.check(cleanup=cleanup))


if __name__ == '__main__':
    unittest.main()
