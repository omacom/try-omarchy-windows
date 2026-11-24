"""Verify the separate 4K lab build without changing the 130x73 acceptance guard.

Requires real native RGBA captures, the independently compiled integer oracle,
guest hashes, native copy/fence receipts, correlated submissions, metadata and
normal owned-VM cleanup. Optional ETW evidence never rewrites DXGI results.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
from presentmon_evidence import correlate_presentmon
from test_owned_runtime_qemu import async_submissions_complete, hw_queue_signals_complete


def require(ok, message):
    if not ok:
        raise ValueError(message)


def exact(record, **values):
    return all(type(record.get(k)) is type(v) and record[k] == v for k,v in values.items())


LIVE_COUNTERS = ('Adapters','Devices','Contexts','TextureConsumers','HwQueues','SyncObjects',
                 'GpuReservations','GpuReservedBytes','GpuStateRanges','GpuStateBytes','PagingQueues',
                 'Allocations','GuestMappings','GuestSections','GpuCopyObjects','VendorAllocations',
                 'VendorResources','VendorMappedPages','VendorResidencyAttempts','VendorCpuBytes','AllocationTranslations')


def validate_cleanup(d, records):
    cleanup, vm = d['disconnectCleanup'], d['ownedQemuControl']
    require(exact(d, hostBridgeExit=0) and exact(cleanup, driverCleanupVerified=True, cleanupFailures=0,
            pendingNativeSubmissions=0, pendingHwQueueSignals=0,
            **{'live'+k:0 for k in LIVE_COUNTERS}), 'Native cleanup not verified')
    require(all(type(v) is int and v == 0 for k,v in cleanup.items() if k.startswith('live')), 'Invalid live counter')
    require(exact(vm, ownedQemuExited=True, qemuExit=0, qemuForcedStop=False, fenceControlFailed=False,
                  liveFenceMappings=0, liveAllocationMappings=0, liveAllocationMappedBytes=0,
                  liveAllocationSlotMappings=0, fenceApertureSlots=128, allocationApertureSlots=128),
            'Owned VM cleanup not verified')
    for created,released in (('fenceMappingsCreated','fenceUnmapAcknowledgements'),
                             ('allocationMappingsCreated','allocationUnmapAcknowledgements'),
                             ('allocationSlotMappingsCreated','allocationSlotUnmapAcknowledgements')):
        require(type(vm.get(created)) is int and type(vm.get(released)) is int and 0 < vm[created] == vm[released],
                'Missing or mismatched mapping retirement')
    # Compare canonical JSON as well as values so booleans cannot masquerade as integers.
    for key,marker in (('disconnectCleanup','driverCleanupVerified'),('ownedQemuControl','ownedQemuExited')):
        selected=[r for r in records if marker in r]
        require(len(selected)==1 and json.dumps(d[key],sort_keys=True)==json.dumps(selected[0],sort_keys=True),
                'Report/cleanup receipt mismatch')


def validate_oracle_frames(frames):
    require(type(frames) is list and len(frames)==2, 'Incomplete independent pixel verification')
    for i,frame in enumerate(frames,1):
        require(type(frame) is dict and exact(frame,round=i,width=3840,height=2160,verifiedPixels=8294400,
                                              trianglePixels=2332800), 'Wrong oracle frame/profile')
        require(type(frame.get('maximumChannelError')) is int and 0 <= frame['maximumChannelError'] <= 2,
                'Oracle error exceeds the shader tolerance')
        for key in ('fnv1a','referenceFnv1a'):
            require(type(frame.get(key)) is int and 0 <= frame[key] < 1 << 64, 'Invalid oracle hash')
    require(frames[0]['fnv1a'] != frames[1]['fnv1a'], 'Repeated frame instead of two shader rounds')


def verify(report_path, captures, oracle, presentmon=None):
    report_path, captures, oracle = map(Path, (report_path, captures, oracle))
    d = json.loads(report_path.read_text())
    guest = report_path.with_suffix('.log').read_text()
    host = report_path.with_suffix('.host.log').read_text()
    records = [json.loads(line) for line in host.splitlines() if line.startswith('{')]
    cleanup, vm = d['disconnectCleanup'], d['ownedQemuControl']
    validate_cleanup(d, records)
    for key,markers in (
            ('nativeTextureMetadataChecks',('nativeTextureMetadataGenerated','nativeTextureMetadataApplied')),
            ('nativeSharedResourceImportChecks',('nativeSharedResourceImportProbe',)),
            ('nativeAsyncSubmissionAcceptances',('nativeAsyncSubmissionAccepted',)),
            ('nativeAsyncSubmissionRetirements',('nativeAsyncSubmissionRetired',)),
            ('nativeHwQueueSignalReturns',('nativeHwQueueSignalReturned',)),
            ('nativeHwQueueSignalAcceptances',('nativeHwQueueSignalAccepted',)),
            ('nativeHwQueueSignalRetirements',('nativeHwQueueSignalRetired',))):
        actual=[r for r in records if any(k in r for k in markers)]
        require(json.dumps(d[key],sort_keys=True)==json.dumps(actual,sort_keys=True), 'Report/native receipt mismatch: '+key)
    require(async_submissions_complete(d['guestCommandEnqueues'],d['nativeAsyncSubmissionAcceptances'],
            d['nativeAsyncSubmissionRetirements'],cleanup), 'Unretired or unmatched submission')
    require(hw_queue_signals_complete(d['guestHwQueueSignals'],d['nativeHwQueueSignalReturns'],
            d['nativeHwQueueSignalAcceptances'],d['nativeHwQueueSignalRetirements'],cleanup), 'Unretired queue signal')
    require('LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true' in guest and 'realDxgForwarding=false' in guest,
            'Missing genuine runtime/transport provenance')
    for stage in ('factory','list','device','copyQueue'):
        require(re.findall(r'^'+stage+r'=([0-9a-f]{8})[ \t\r]*$',guest,re.M) == ['00000000'], 'Initialization failure')
    require('GPU_TRIANGLE_TIMEOUT' not in guest and 'GPU_TRIANGLE_MISMATCH' not in guest and
            'gpuTriangleDeviceRemoved=' not in guest, 'Shader failure')
    for stage,count in (('DirectQueue',1),('SerializeRootSignature',1),('RootSignature',1),('Pipeline',1),
                        ('RenderTarget',1),('Readback',1),('RtvHeap',1),('Allocator',1),('CommandList',1),
                        ('Fence',1),('Close',2),('Signal',2),('ReadbackMap',2),('AllocatorReset',1),('CommandListReset',1)):
        require(re.findall(r'^gpuTriangle'+stage+r'=([0-9a-f]{8})[ \t\r]*$',guest,re.M) == ['00000000']*count,
                'Shader HRESULT count/result mismatch: '+stage)
    require(guest.splitlines().count('GPU_TRIANGLE_TEST_BEGIN width=3840 height=2160 format=R8G8B8A8_UNORM rounds=2 vertices=3 tolerance=2') == 1,
            'Wrong shader profile')
    require(re.findall(r'^GPU_TRIANGLE_SHADERS vertexBytes=(\d+) pixelBytes=(\d+) vertexHash=([0-9a-f]+) pixelHash=([0-9a-f]+)[ \t\r]*$',guest,re.M) ==
            [('2288','1720','3e5ff943b07f0f53','86df59ba24d3b394')], 'Wrong shaders')
    require(re.findall(r'^GPU_TRIANGLE_FOOTPRINT width=(\d+) height=(\d+) offset=(\d+) rowPitch=(\d+) rowBytes=(\d+) rows=(\d+) totalBytes=(\d+)[ \t\r]*$',guest,re.M) ==
            [('3840','2160','512','15360','15360','2160','33178112')], 'Wrong readback footprint')
    metadata = d['nativeTextureMetadataChecks']; imports = d['nativeSharedResourceImportChecks']
    require(len(metadata) == 2 and len(imports) == 1, 'Missing metadata/import receipts')
    generated, applied = metadata
    require(exact(generated, nativeTextureMetadataGenerated=True,width=3840,height=2160,format=28,flags=1,
                  allocationAlignment=65536,adapterNtstatus=0,deviceNtstatus=0,queryNtstatus=0,openNtstatus=0,
                  resourceDestroyedNtstatus=0,deviceDestroyedNtstatus=0,adapterClosedNtstatus=0,
                  sharedHandleClosed=True,prototypeReleased=True,privateBytesExported=False) and
            type(generated.get('allocationBytes')) is int and 33177600 <= generated['allocationBytes'] <= 67108864 and
            type(generated.get('runtimeBytes')) is int and 0 < generated['runtimeBytes'] <= 1024,
            'Native template metadata not verified')
    require(exact(applied,nativeTextureMetadataApplied=True,typedTexture=True,guestRuntimePreserved=True,
                  driverPrivateDataReused=True,ntstatus=0,nativeRuntimeBytes=generated['runtimeBytes']) and
            type(applied.get('guestRuntimeBytes')) is int and 0 < applied['guestRuntimeBytes'] <= 1024,
            'Typed native metadata not applied')
    require(exact(imports[0],nativeSharedResourceImportProbe=True,verified=True,ntstatus=0,deviceHresult=0,
                  importHresult=0,width=3840,height=2160,format=28,dimension=3,flags=1,
                  runtimeBytes=generated['runtimeBytes'],sharedNtHandleClosed=True,importedResourceReleased=True),
            'Native 4K import not verified')
    copies = [r for r in records if r.get('nativeSharedTextureCopied') is True]
    presents = [r for r in records if 'nativeSharedTexturePresented' in r]
    binaries = [r for r in records if r.get('nativeBinaryCapture') is True]
    require(d['nativeSharedTextureChecks'] == [r for r in records if any(k.startswith('nativeSharedTexture') for k in r)],
            'Report/host receipt mismatch')
    require(len(copies) == 2 and len(presents) in (0,2), 'Incomplete native frame receipts')
    require(len(binaries) == (4 if presents else 2), 'Incomplete/duplicate native captures')
    paths = [captures/f'native-copy-{i}.rgba' for i in (1,2)]
    checked = subprocess.run([str(oracle.resolve()),*[str(p.resolve()) for p in paths]],check=True,
                             stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=60)
    oracle_frames = [json.loads(line) for line in checked.stdout.splitlines()]
    validate_oracle_frames(oracle_frames)
    rounds = re.findall(r'^GPU_TRIANGLE_ROUND round=(\d+) verifiedPixels=(\d+) trianglePixels=(\d+) backgroundPixels=(\d+) maxChannelError=(\d+) referenceHash=([0-9a-f]+) observedHash=([0-9a-f]+) fenceTarget=(\d+) fenceObserved=(\d+)[ \t\r]*$',guest,re.M)
    require(len(rounds) == 2, 'Incomplete guest pixel verification')
    frames=[]
    for i,(frame,r,copy,path) in enumerate(zip(oracle_frames,rounds,copies,paths),1):
        require(tuple(map(int,r[:5])) == (i,8294400,2332800,5961600,frame['maximumChannelError']) and
                int(r[5],16) == frame['referenceFnv1a'] and int(r[6],16) == frame['fnv1a'] and
                int(r[7]) == i <= int(r[8]) < (1 << 64)-1, 'Guest hash/oracle mismatch')
        require(exact(copy,round=i,width=3840,height=2160,gpuFenceTarget=i,fnv1a=frame['fnv1a'],
                      sourceReturnedToCommon=True,cpuUpload=False,presented=False) and
                type(copy.get('gpuFenceObserved')) is int and i <= copy['gpuFenceObserved'] < (1 << 64)-1,
                'Native copy/fence mismatch')
        payload=path.read_bytes(); require(len(payload) == 33177600, 'Truncated RGBA')
        names=[path.name]
        if presents:
            back=captures/f'native-backbuffer-{i}.rgba'
            require(back.read_bytes() == payload, 'Backbuffer pixel mismatch')
            names.append(back.name)
        for name in names:
            require(sum(exact(b,file=name,round=i,bytes=33177600,fnv1a=frame['fnv1a']) for b in binaries) == 1,
                    'Missing/duplicate binary capture hash')
        frames.append(dict(frame,rgbaSha256=hashlib.sha256(payload).hexdigest(),backbufferBytesMatch=bool(presents)))
    handoffs=[r for r in records if r.get('nativeSharedTextureHandoff') is True]
    completed = (exact(cleanup,completedSharedTextureConsumes=2,failedSharedTextureConsumes=0,
                       openedTextureConsumers=1,releasedTextureConsumers=1,liveTextureConsumers=0) and len(handoffs)==2 and
                 guest.splitlines().count('GPU_NATIVE_METADATA_TEST_COMPLETE verified=true rounds=2')==1 and
                 re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$',guest,re.M)==['0'])
    for i,f in enumerate(frames,1):
        completed = completed and guest.splitlines().count(f"GPU_SHARED_CONSUME_HOST round={i} hash={f['fnv1a']:016x} returnedState=COMMON")==1
        if len(handoffs) == 2:
            h=handoffs[i-1]
            completed = completed and exact(h,target=i,nativeSignalAccepted=True,sourceState='COMMON',
                                            returnedState='COMMON',pendingGuestCommands=0) and type(h.get('observed')) is int and i <= h['observed'] < (1 << 64)-1
    consumers=[r for r in records if r.get('nativeSharedTextureConsumerReleased') is True]
    require(len(consumers)==1 and exact(consumers[0],gpuWorkRetired=True), 'Consumer release not verified')
    if presents:
        released=[r for r in records if r.get('nativeSharedTexturePresenterReleased') is True]
        require(len(released)==1 and exact(released[0],gpuWorkRetired=True,windowDestroyed=True,classUnregistered=True),
                'Presenter release not verified')
    dxgi = len(presents)==2
    previous_index, previous_qpc = None,0
    for i,(p,f) in enumerate(zip(presents,frames),1):
        dxgi = (dxgi and exact(p,nativeSharedTexturePresented=True,round=i,width=3840,height=2160,
                 backBufferFnv1a=f['fnv1a'],presentHresult=0,lastPresentCount=i,statisticsHresult=0,
                 statisticsPresentCount=i,windowVisible=True,presentQueueFenceTarget=i,
                 backBufferState='PRESENT',cpuUpload=False) and type(p.get('syncQpc')) is int and
                 previous_qpc < p['syncQpc'] < (1 << 63) and p.get('backBufferIndex') in (0,1) and
                 p['backBufferIndex'] != previous_index and type(p.get('presentQueueFenceObserved')) is int and
                 i <= p['presentQueueFenceObserved'] < (1 << 64)-1)
        previous_index,previous_qpc=p.get('backBufferIndex'),p.get('syncQpc',0)
    etw = correlate_presentmon(Path(presentmon).read_text(),presents) if presentmon else None
    return dict(schema=1,lab4kRenderingVerified=True,sharedHandoffVerified=bool(completed),
                dxgiPresentationVerified=bool(dxgi),etwPresentation=etw,
                full4kDiagnosticVerified=bool(completed and (not presents or dxgi)),frames=frames,
                nativeCleanupVerified=True,ownedVmCleanupVerified=True,
                originalSmallProfileAccepted=d['diagnosticAccepted'],
                originalReportSha256=hashlib.sha256(report_path.read_bytes()).hexdigest(),
                oracleSha256=hashlib.sha256(oracle.read_bytes()).hexdigest(),
                guestDesktopAcceleratedByThisBackend=False,nativePerformanceMeasured=False,
                graphicsBackendProductReady=False)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('report','capture-directory','oracle','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--presentmon',type=Path)
    a=p.parse_args()
    result=verify(a.report,a.capture_directory,a.oracle,a.presentmon)
    with a.output.open('x') as stream:
        json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))
    return 0 if result['full4kDiagnosticVerified'] else 1


if __name__=='__main__':
    raise SystemExit(main())
