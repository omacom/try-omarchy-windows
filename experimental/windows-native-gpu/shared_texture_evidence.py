"""Require independently exported Windows GPU copies to match guest shader bytes."""
import json
import re
from triangle_evidence import triangle_readback, checksum


def shared_texture_consume_complete(guest, host, cleanup, shared_verified, *, presentation=False):
    if type(presentation) is not bool:
        return False
    if shared_verified is not True:
        return False
    lines = guest.splitlines()
    begin = 'GPU_SHARED_CONSUME_TEST_BEGIN rounds=2 state=COMMON'
    end = 'GPU_SHARED_CONSUME_TEST_COMPLETE verified=true rounds=2'
    if (lines.count(begin) != 1 or lines.count(end) != 1 or lines.index(begin) >= lines.index(end) or
            sum(line.startswith('GPU_SHARED_CONSUME_') for line in lines) != 6):
        return False
    frames = triangle_readback(guest)
    if frames is None:
        return False
    rows = re.findall(r'^NATIVE_SHARED_TEXTURE_PIXEL_ROW round=(\d+) y=(\d+) rgba=([0-9a-f]+)[ \t\r]*$', host, re.MULTILINE)
    if len(rows) != 146 or sum(line.startswith('NATIVE_SHARED_TEXTURE_PIXEL_ROW') for line in host.splitlines()) != 146:
        return False
    for round_, frame in enumerate(frames, 1):
        actual = bytearray()
        for y, (r, row, rgba) in enumerate(rows[(round_ - 1) * 73:round_ * 73]):
            if (r, row) != (str(round_), str(y)) or len(rgba) != 1040:
                return False
            actual.extend(bytes.fromhex(rgba))
        if actual != frame:
            return False
        handoffs = re.findall(r'^GPU_SHARED_CONSUME_HANDOFF round=' + str(round_) + r' state=COMMON fenceTarget=(\d+) fenceObserved=(\d+)[ \t\r]*$', guest, re.MULTILINE)
        ack = f'GPU_SHARED_CONSUME_HOST round={round_} hash={checksum(frame)} returnedState=COMMON'
        if len(handoffs) != 1 or int(handoffs[0][0]) != round_ or not round_ <= int(handoffs[0][1]) < (1 << 64) - 1 or lines.count(ack) != 1:
            return False
        handoff_line = f'GPU_SHARED_CONSUME_HANDOFF round={round_} state=COMMON fenceTarget={handoffs[0][0]} fenceObserved={handoffs[0][1]}'
        last_row = next(line for line in lines if line.startswith(f'GPU_TRIANGLE_PIXEL_ROW round={round_} y=72 '))
        if not lines.index(last_row) < lines.index(handoff_line) < lines.index(ack) < lines.index(end):
            return False
        if round_ == 1 and lines.index(ack) >= lines.index('GPU_TRIANGLE_DRAW round=2 vertices=3 startVertex=0 instances=1 colorRotation=1'):
            return False
    try:
        records = [json.loads(line) for line in host.splitlines() if line.startswith('{') and 'nativeSharedTexture' in line]
    except ValueError:
        return False
    present_records = [r for r in records if any(k in r for k in (
        'nativeSharedTexturePresenterOpened', 'nativeSharedTexturePresented', 'nativeSharedTexturePresenterReleased'))]
    if presentation:
        if not _presentation_complete(frames, host, records, present_records):
            return False
        records = [r for r in records if r not in present_records]
    elif present_records or 'NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW' in host:
        return False
    opened = [r for r in records if r.get('nativeSharedTextureConsumerOpened') is True]
    released = [r for r in records if r.get('nativeSharedTextureConsumerReleased') is True]
    copies = [r for r in records if r.get('nativeSharedTextureCopied') is True]
    handoffs = [r for r in records if r.get('nativeSharedTextureHandoff') is True]
    if len(records) != 6 or len(opened) != 1 or len(released) != 1 or len(copies) != 2 or len(handoffs) != 2:
        return False

    def exact(record, values):
        return all(type(record.get(k)) is type(v) and record[k] == v for k, v in values.items())

    if not exact(opened[0], dict(ntstatus=0, hresult=0, sharedNtHandleClosed=True)) or not exact(released[0], dict(gpuWorkRetired=True)):
        return False
    if not (records[0] is opened[0] and records[-1] is released[0]):
        return False
    for i, (copy, handoff, frame) in enumerate(zip(copies, handoffs, frames), 1):
        if not exact(copy, dict(round=i, width=130, height=73, gpuFenceTarget=i,
                               fnv1a=int(checksum(frame), 16), sourceReturnedToCommon=True, cpuUpload=False, presented=False)):
            return False
        if type(copy.get('gpuFenceObserved')) is not int or not i <= copy['gpuFenceObserved'] < (1 << 64) - 1:
            return False
        if not exact(handoff, dict(target=i, nativeSignalAccepted=True, sourceState='COMMON', returnedState='COMMON', pendingGuestCommands=0)):
            return False
        if type(handoff.get('observed')) is not int or not i <= handoff['observed'] < (1 << 64) - 1:
            return False
        if records[i * 2 - 1] is not copy or records[i * 2] is not handoff:
            return False
    # UMD command batching can change the count. Require every actual native
    # submission to retire; the caller independently correlates all receipts.
    accepted = cleanup.get('acceptedAsyncSubmissions')
    completed = cleanup.get('completedNativeSubmissions')
    return (type(accepted) is int and type(completed) is int and 2 <= accepted == completed and
            exact(cleanup, dict(driverCleanupVerified=True, openedTextureConsumers=1, releasedTextureConsumers=1,
                               liveTextureConsumers=0, completedSharedTextureConsumes=2, failedSharedTextureConsumes=0,
                               pendingNativeSubmissions=0, acceptedHwQueueSignals=4, completedHwQueueSignals=4,
                               pendingHwQueueSignals=0, cleanupFailures=0)))


def _presentation_complete(frames, host, all_records, records):
    """Backbuffer bytes plus flip statistics; this is not a screen capture."""
    if len(records) != 4 or len(all_records) != 10:
        return False

    def exact(record, values):
        return all(type(record.get(k)) is type(v) and record[k] == v for k, v in values.items())

    if not exact(records[0], dict(nativeSharedTexturePresenterOpened=True, width=130, height=73, bufferCount=2,
                                 flipModel=True, clientWidth=780, clientHeight=438, windowVisible=True,
                                 sameAdapter=True, cpuUpload=False)):
        return False
    if not exact(records[-1], dict(nativeSharedTexturePresenterReleased=True, gpuWorkRetired=True,
                                  windowDestroyed=True, classUnregistered=True)):
        return False
    if [all_records.index(r) for r in records] != [1, 3, 6, 8]:
        return False
    rows = re.findall(r'^NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW round=(\d+) y=(\d+) rgba=([0-9a-f]+)[ \t\r]*$', host, re.MULTILINE)
    if len(rows) != 146 or sum(line.startswith('NATIVE_SHARED_TEXTURE_BACKBUFFER_ROW') for line in host.splitlines()) != 146:
        return False
    previous_index = None
    previous_qpc = 0
    for i, (record, frame) in enumerate(zip(records[1:3], frames), 1):
        actual = bytearray()
        for y, (round_, row, rgba) in enumerate(rows[(i - 1) * 73:i * 73]):
            if (round_, row) != (str(i), str(y)) or len(rgba) != 1040:
                return False
            actual.extend(bytes.fromhex(rgba))
        if actual != frame or not exact(record, dict(nativeSharedTexturePresented=True, round=i, width=130, height=73,
                backBufferFnv1a=int(checksum(frame), 16), presentHresult=0, lastPresentCount=i, statisticsHresult=0,
                statisticsPresentCount=i, dwmFlushHresult=0, windowVisible=True, presentQueueFenceTarget=i,
                backBufferState='PRESENT', cpuUpload=False)):
            return False
        index, qpc, fence = (record.get(k) for k in ('backBufferIndex', 'syncQpc', 'presentQueueFenceObserved'))
        if (type(index) is not int or index not in (0, 1) or index == previous_index or
                type(qpc) is not int or not previous_qpc < qpc < (1 << 63) or
                type(fence) is not int or not i <= fence < (1 << 64) - 1):
            return False
        previous_index, previous_qpc = index, qpc
    return True
