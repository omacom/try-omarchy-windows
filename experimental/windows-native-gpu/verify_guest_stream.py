"""Verify bounded guest streaming without promoting Present acceptance to scanout proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from verify_4k_capture import exact, require, validate_cleanup, validate_oracle_frames
from test_owned_runtime_qemu import async_submissions_complete, hw_queue_signals_complete


def records_for(records, marker):
    return [r for r in records if marker in r]


def retired(value, target):
    return type(value) is int and target <= value < (1 << 64) - 1


def validate_receipts(d, guest, records, frames):
    validate_cleanup(d, records)
    validate_oracle_frames(frames)
    require(exact(d, runtimeWorkload='native-stream', diagnosticAccepted=False), 'Wrong stream report')
    cleanup = d['disconnectCleanup']
    for key in ('nativeSubmissionAttempts','completedNativeSubmissions','acceptedAsyncSubmissions',
                'peakPendingNativeSubmissions','nativeHwQueueSignalAttempts','acceptedHwQueueSignals',
                'completedHwQueueSignals','peakPendingHwQueueSignals'):
        require(type(cleanup.get(key)) is int, 'Invalid native counter: ' + key)
    require(cleanup['nativeSubmissionAttempts'] == len(d['guestCommandEnqueues']), 'Missing submission attempt')
    for peak,limit,value in (('peakVendorCpuBytes','vendorCpuMappedByteLimit',96*1024*1024),
                             ('peakVendorMappedPages','vendorGpuMappedByteLimit',256*1024*1024),
                             ('peakVendorAllocationObjects','vendorAllocationObjectLimit',128)):
        divisor = 4096 if peak == 'peakVendorMappedPages' else 1
        require(type(cleanup.get(peak)) is int and exact(cleanup,**{limit:value}) and
                0 < cleanup[peak] <= value // divisor, 'Exceeded mapped-memory/object bounds')
    for r in d['guestCommandEnqueues']:
        require(all(type(r.get(k)) is int for k in ('bytes','privateBytes','target','observedAtReturn')), 'Invalid guest enqueue')
    for r in d['guestHwQueueSignals']:
        require(all(type(r.get(k)) is int for k in ('flags','queues','target','observedAtReturn')), 'Invalid guest signal')
    profile = records_for(records, 'guestStreamNegotiated')
    require(len(profile) == 1 and exact(profile[0], guestStreamNegotiated=True, version=1, frames=64,
            pendingCommands=16, pendingSignals=16, totalCommands=1024, totalSignals=256, requestLimit=50000),
            'Missing explicit bounded negotiation')
    for key, marker in (
        ('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired'),
        ('nativeHwQueueSignalReturns', 'nativeHwQueueSignalReturned'),
        ('nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalAccepted'),
        ('nativeHwQueueSignalRetirements', 'nativeHwQueueSignalRetired')):
        require(json.dumps(d[key], sort_keys=True) == json.dumps(records_for(records, marker), sort_keys=True),
                'Report/native receipt mismatch: ' + key)
        for r in d[key]:
            require(r.get(marker) is True and all(type(v) is int for k,v in r.items()
                    if k not in (marker,'noGpuAccess','cpuValueWrittenByBridge')), 'Invalid native receipt field')
    require(async_submissions_complete(d['guestCommandEnqueues'], d['nativeAsyncSubmissionAcceptances'],
            d['nativeAsyncSubmissionRetirements'], cleanup, maximum=1024, pending_limit=16), 'Unretired guest command')
    require(hw_queue_signals_complete(d['guestHwQueueSignals'], d['nativeHwQueueSignalReturns'],
            d['nativeHwQueueSignalAcceptances'], d['nativeHwQueueSignalRetirements'], cleanup,
            maximum=256, pending_limit=16), 'Unretired guest signal')
    require(exact(cleanup, completedSharedTextureConsumes=64, failedSharedTextureConsumes=0,
            openedTextureConsumers=1, releasedTextureConsumers=1, failedNativeSubmissions=0,
            nativeSubmissionTimeouts=0, failedHwQueueSignals=0, failedNativeContextSignals=0), 'Incomplete stream cleanup')
    require(64 <= len(d['guestCommandEnqueues']) <= 1024 and len(d['guestHwQueueSignals']) == 64,
            'Wrong command/signal count')
    require('LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true' in guest and 'realDxgForwarding=false' in guest,
            'Missing genuine runtime provenance')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', guest, re.M) == ['0'], 'Guest workload failed')
    for line in ('GPU_STREAM_TEST_BEGIN frames=64 width=3840 height=2160 audits=2 pendingCommands=16 pendingSignals=16',
                 'GPU_STREAM_TEST_COMPLETE verified=true frames=64 width=3840 height=2160 audits=2 allFramesDisplayedVerified=false'):
        require(guest.splitlines().count(line) == 1, 'Incomplete guest stream')
    require('GPU_TRIANGLE_TIMEOUT' not in guest and 'GPU_TRIANGLE_MISMATCH' not in guest and
            'gpuTriangleDeviceRemoved=' not in guest, 'Guest shader failure')
    for stage in ('factory', 'list', 'device', 'copyQueue'):
        require(re.findall(r'^' + stage + r'=([0-9a-f]{8})[ \t\r]*$', guest, re.M) == ['00000000'], 'Initialization failure')
    for stage, count in (('DirectQueue',1), ('SerializeRootSignature',1), ('RootSignature',1), ('Pipeline',1),
                         ('RenderTarget',1), ('Readback',1), ('RtvHeap',1), ('Allocator',1), ('CommandList',1),
                         ('Fence',1), ('Close',64), ('Signal',64), ('ReadbackMap',2), ('AllocatorReset',63), ('CommandListReset',63)):
        require(re.findall(r'^gpuTriangle' + stage + r'=([0-9a-f]{8})[ \t\r]*$', guest, re.M) == ['00000000'] * count,
                'Shader HRESULT mismatch: ' + stage)
    draws = re.findall(r'^GPU_TRIANGLE_DRAW round=(\d+) vertices=3 startVertex=0 instances=1 colorRotation=(\d+)[ \t\r]*$', guest, re.M)
    require(draws == [(str(i),str((i-1)%2)) for i in range(1,65)], 'Missing/reordered shader draw')
    require(re.findall(r'^GPU_TRIANGLE_SHADERS vertexBytes=(\d+) pixelBytes=(\d+) vertexHash=([0-9a-f]+) pixelHash=([0-9a-f]+)[ \t\r]*$',guest,re.M) ==
            [('2288','1720','3e5ff943b07f0f53','86df59ba24d3b394')], 'Wrong shader bytecode')
    metadata = records_for(records,'nativeTextureMetadataGenerated')
    applied = records_for(records,'nativeTextureMetadataApplied')
    imports = records_for(records,'nativeSharedResourceImportProbe')
    require(len(metadata) == len(applied) == len(imports) == 1, 'Missing typed metadata/import')
    m,a,im = metadata[0],applied[0],imports[0]
    require(exact(m,nativeTextureMetadataGenerated=True,width=3840,height=2160,format=28,flags=1,
            allocationAlignment=65536,adapterNtstatus=0,deviceNtstatus=0,queryNtstatus=0,openNtstatus=0,
            resourceDestroyedNtstatus=0,deviceDestroyedNtstatus=0,adapterClosedNtstatus=0,
            sharedHandleClosed=True,prototypeReleased=True,privateBytesExported=False) and
            type(m.get('allocationBytes')) is int and 33177600 <= m['allocationBytes'] <= 67108864 and
            type(m.get('runtimeBytes')) is int and 0 < m['runtimeBytes'] <= 1024, 'Invalid native texture metadata')
    require(exact(a,nativeTextureMetadataApplied=True,typedTexture=True,guestRuntimePreserved=True,
            driverPrivateDataReused=True,ntstatus=0,nativeRuntimeBytes=m['runtimeBytes']) and
            type(a.get('guestRuntimeBytes')) is int and 0 < a['guestRuntimeBytes'] <= 1024, 'Metadata not applied')
    require(exact(im,nativeSharedResourceImportProbe=True,verified=True,ntstatus=0,deviceHresult=0,
            importHresult=0,width=3840,height=2160,format=28,dimension=3,flags=1,runtimeBytes=m['runtimeBytes'],
            sharedNtHandleClosed=True,importedResourceReleased=True), 'Native shared texture import failed')
    copies = records_for(records, 'guestStreamCopy')
    handoffs = records_for(records, 'nativeSharedTextureHandoff')
    guest_frames = re.findall(r'^GPU_STREAM_FRAME frame=(\d+) auditRound=(\d+) renderRound=(\d+) guestFenceObserved=(\d+) hostHash=([0-9a-f]{16}) cpuReadback=(true|false) returnedState=COMMON[ \t\r]*$', guest, re.M)
    require(len(copies) == len(handoffs) == len(guest_frames) == 64, 'Missing/duplicate stream frame')
    for i, (copy, handoff, g) in enumerate(zip(copies, handoffs, guest_frames),1):
        audit = 1 if i == 1 else 2 if i == 64 else 0
        value = frames[audit-1]['fnv1a'] if audit else 0
        require(exact(copy, guestStreamCopy=True, frame=i, auditRound=audit, fnv1a=value, cpuReadback=bool(audit),
                sourceReturnedToCommon=True, cpuUpload=False) and retired(copy.get('gpuFenceObserved'),i), 'Copy/audit/fence mismatch')
        require(exact(handoff, nativeSharedTextureHandoff=True, target=i, nativeSignalAccepted=True,
                sourceState='COMMON', returnedState='COMMON', pendingGuestCommands=0) and retired(handoff.get('observed'),i),
                'Unsafe guest texture handoff')
        require((int(g[0]),int(g[1]),int(g[2])) == (i,audit,1+(i-1)%2) and retired(int(g[3]),i) and
                int(g[4],16) == value and g[5] == ('true' if audit else 'false'), 'Guest/native stream mismatch')
    audits = re.findall(r'^GPU_TRIANGLE_ROUND round=(\d+) verifiedPixels=(\d+) trianglePixels=(\d+) backgroundPixels=(\d+) maxChannelError=(\d+) referenceHash=([0-9a-f]+) observedHash=([0-9a-f]+) fenceTarget=(\d+) fenceObserved=(\d+)[ \t\r]*$', guest, re.M)
    require(len(audits) == 2, 'Incomplete guest pixel audits')
    for a, frame, target in zip(audits, frames, (1,64)):
        require(tuple(map(int,a[:5])) == (frame['round'],8294400,2332800,5961600,frame['maximumChannelError']) and
                int(a[5],16) == frame['referenceFnv1a'] and int(a[6],16) == frame['fnv1a'] and
                int(a[7]) == target and retired(int(a[8]),target), 'Guest pixel audit differs from independent oracle')
    consumers = records_for(records, 'nativeSharedTextureConsumerReleased')
    require(len(consumers) == 1 and exact(consumers[0], gpuWorkRetired=True), 'Consumer release missing')
    presents = records_for(records, 'guestStreamPresent')
    if presents:
        require(len(presents) == 64, 'Incomplete Present acceptance')
        previous = 0; identity = None
        for i,p in enumerate(presents,1):
            require(exact(p, guestStreamPresent=True, frame=i, presentHresult=0, presentCountHresult=0,
                          lastPresentCount=i, allFramesDisplayedVerified=False, cpuUpload=False) and
                    retired(p.get('presentQueueFenceObserved'),i), 'Unaccepted/unretired Present')
            for key in ('presentCallQpcBefore','presentCallQpcAfter','qpcFrequency','presentProcessId','presentSwapChain'):
                require(type(p.get(key)) is int and 0 < p[key] < 1 << 63, 'Invalid Present identity/timing')
            current = (p['presentProcessId'],p['presentSwapChain'],p['qpcFrequency'])
            require(previous < p['presentCallQpcBefore'] <= p['presentCallQpcAfter'] and
                    identity in (None,current), 'Present timing/identity changed')
            previous = p['presentCallQpcAfter']; identity = current
        released = records_for(records, 'nativeSharedTexturePresenterReleased')
        require(len(released) == 1 and exact(released[0], gpuWorkRetired=True, windowDestroyed=True, classUnregistered=True),
                'Presenter cleanup missing')
    return bool(presents)


def verify(report, captures, oracle):
    report, captures, oracle = map(Path,(report,captures,oracle))
    d = json.loads(report.read_text()); guest = report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    paths = [captures / f'native-copy-{i}.rgba' for i in (1,2)]
    checked = subprocess.run([str(oracle.resolve()),*[str(p.resolve()) for p in paths]], check=True,
                             capture_output=True,text=True,timeout=60)
    frames = [json.loads(line) for line in checked.stdout.splitlines()]
    presented = validate_receipts(d,guest,records,frames)
    binaries = records_for(records,'nativeBinaryCapture')
    require(len(binaries) == (4 if presented else 2), 'Unexpected/duplicate binary capture')
    for i,(path,frame) in enumerate(zip(paths,frames),1):
        payload = path.read_bytes(); require(len(payload) == 33177600, 'Truncated capture')
        names = [path.name]
        if presented:
            back = captures / f'native-backbuffer-{i}.rgba'
            require(back.read_bytes() == payload, 'Backbuffer audit differs from guest texture'); names.append(back.name)
        for name in names:
            require(sum(exact(b,file=name,round=i,bytes=33177600,fnv1a=frame['fnv1a']) for b in binaries) == 1,
                    'Binary capture receipt mismatch')
        frame['rgbaSha256'] = hashlib.sha256(payload).hexdigest()
        frame['backbufferBytesMatch'] = presented
    return dict(schema=1,boundedGuestStreamVerified=True,frames=64,pixelAuditFrames=[1,64],pixelAudits=frames,
                intermediateFramesWithCpuReadback=0,presentCallsAcceptedAndRetired=presented,
                allFramesDisplayedVerified=False,guestDesktopAcceleratedByThisBackend=False,
                nearNativePerformanceMeasured=False,graphicsBackendProductReady=False,
                nativeCleanupVerified=True,ownedVmCleanupVerified=True,
                nativeSubmissions=len(d['guestCommandEnqueues']),nativeSignals=len(d['guestHwQueueSignals']),
                peakPendingCommands=d['disconnectCleanup']['peakPendingNativeSubmissions'],
                peakPendingSignals=d['disconnectCleanup']['peakPendingHwQueueSignals'],
                peakReportedProcessGpuUsageBytes=d['disconnectCleanup']['peakReportedGpuUsageBytes'],
                hardGpuMemoryQuotaVerified=False,
                reportSha256=hashlib.sha256(report.read_bytes()).hexdigest(),oracleSha256=hashlib.sha256(oracle.read_bytes()).hexdigest())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('report','capture-directory','oracle','output'): p.add_argument('--'+name,type=Path,required=True)
    a = p.parse_args(); result = verify(a.report,a.capture_directory,a.oracle)
    with a.output.open('x') as stream: json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__': main()
