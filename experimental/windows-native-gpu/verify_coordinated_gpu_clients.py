"""Audit real, independent GPU-client initialization; never accept desktop pixels."""
import re
import hashlib
import json
from functools import lru_cache
from verify_hyprland_compositor_capture import fnv
from verify_hyprland_lifecycle_capture import require

ROOT='3954123192111ce2d28cafb9dd108e1ca14c288bfc2429ed899b0f36aeccf942'
QEMU='dcd422e6a7882af4ede4d6a79915dcfa6b344253b0b008cd6cebb55e62889111'
DESKTOP_FENCE_QEMU='9ffb191868cdd61fee0152d379f284ef6e81705dc552c793a1dd4582bca0f6b4'
DESKTOP_SLOT_QEMU='4691955c77cb33ba656a7ad2b3cc97c7c514f8705dc46c5d328d641f38c8ba46'
RENDERER='OpenGL VENDOR: Microsoft Corporation RENDERER: D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU) VERSION: 4.6 (Compatibility Profile) Mesa 26.0.8'


def validate(report,guest,records,source,profile=10):
    require(profile in (10,11,12,13),'Unsupported coordinated audit profile')
    fence_slots=512 if profile>=11 else 256
    cpu_slots=1024 if profile>=12 else 512
    manifest=source['manifest']; clean=report['disconnectCleanup']; vm=report['ownedQemuControl']
    require(source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT and source['sourceImageUnchanged'] is True,'Source image changed')
    require(report['qemuSha256']==(DESKTOP_SLOT_QEMU if profile>=12 else DESKTOP_FENCE_QEMU if profile==11 else QEMU) and report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256'] and
            report['initramfsSha256']==manifest['initramfsSha256'],'Mixed native/guest provenance')
    require(manifest['coordinatedNativeActors'] is True and manifest['ordinaryUid']==1000 and
            manifest['desktopGeometryProbe']==[3840,2160] and manifest['kernelCpuCapacityProfile']==profile and
            manifest['mesaWaylandGpuProbe'] is True and manifest['qtQuickSoftwareProbe'] is False,'Wrong client/GPU/geometry inputs')
    require(report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
            report['capturedPrivateFixturesUsed'] is False and 'qtQuickSoftware=true' not in guest,'Software/foreign runtime')
    require('BROKER_RENDER_CLIENT uid=1000 gid=1000 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied' in guest and
            'BROKER_COORDINATOR_KERNEL_CAPACITY verified=true leaseLimit=768 cpuBytes=201326592 flags=3 actualIoctl=true' in guest,'Missing actual credential/kernel discovery')
    peers={int(pid):(primary,bytes.fromhex(path).decode()) for pid,primary,path in re.findall(
        r'^BROKER_COORDINATOR_PEER pid=(\d+) primary=(true|false) exePathHex=([0-9a-f]+) pidfdAuthenticated=true$',guest,re.M)}
    actors={int(pid):(int(actor),primary) for actor,pid,primary in re.findall(
        r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=(\d+) primary=(true|false) pidfdAuthenticated=true nativeDriverCount=1$',guest,re.M)}
    composers=[pid for pid,row in peers.items() if row==('true','/usr/bin/Hyprland') and actors.get(pid,(0,''))[1]=='true']
    shells=[pid for pid,row in peers.items() if row==('false','/usr/bin/quickshell') and actors.get(pid,(0,''))[1]=='false' and
            any(f'omarchy-shell[{pid}]: ' in line and RENDERER in line for line in guest.splitlines()) and
            any(f'omarchy-shell[{pid}]: ' in line and 'Created QRhi ' in line for line in guest.splitlines())]
    require(len(composers)==1 and shells and actors[composers[0]][0] not in [actors[p][0] for p in shells],'Missing PID-correlated independent NVIDIA Qt renderer')
    def one(key):
        rows=[row for row in records if key in row]
        require(len(rows)==1 and rows[0][key] is True,'Missing/duplicate/failed '+key)
        return rows[0]
    group=one('nativeCoordinatorSummary'); cap=one('nativeCoordinatedObjectCapacity')
    require(group['nativeDriverCount']==1 and group['memoryBudgetsMultiplied'] is False and 2<=group['peakActors']<=4,'Wrong group ownership/budget')
    require((cap['version'],cap['hwQueues'],cap['syncObjects'],cap['reservedAddressBytes'],cap['reservationCount'],cap['kernelLeaseLimit'],cap['fenceSlots'],cap['cpuBytes'],cap['gpuVaBytes'],cap['reportedGpuBytes'],cap['memoryBudgetsMultiplied'])==
            (5 if profile==13 else 4 if profile>=11 else 3,32,384,68719476736,256,768,fence_slots,201326592,1073741824,1073741824,False),'Unexpected aggregate admission')
    if profile>=11:
        require(cap.get('vendorAllocationObjects')==1024 and vm['fenceApertureSlots']==512 and vm['allocationApertureSlots']==cpu_slots and
                'DESKTOP_FENCE_CONTRACT enabled=1 slots=512 bytes=2097152 actualIoctl=true' in guest and
                f'BROKER_COORDINATOR_FENCE_CAPACITY profile={profile} slots=512 bytes=2097152 actualIoctl=true' in guest,'Missing actual desktop aperture/object agreement')
    if profile>=12:
        require('DESKTOP_SLOT_CONTRACT enabled=1 slots=1024 bytes=1073741824 actualIoctl=true' in guest and
                f'BROKER_COORDINATOR_SLOT_CAPACITY profile={profile} slots=1024 bytes=1073741824 cpuByteLimit=201326592 actualIoctl=true' in guest,
                'Missing actual CPU-slot capacity agreement')
    if profile==13:
        require(cap.get('sharedResourceReferences')==cap.get('sharedSyncReferences')==64 and
                'BROKER_COORDINATOR_REFERENCE_CAPACITY profile=13 objectsVersion=5 resources=64 syncs=64 perActor=16 byteBudgetsMultiplied=false' in guest,
                'Missing exact aggregate reference agreement')
    waits=[r for r in records if 'nativeBorrowedQueueGpuWaitAccepted' in r]
    require(waits and all(r['nativeBorrowedQueueGpuWaitAccepted'] is True and r['cpuValueWrittenByBridge'] is False and
            r['paging']+r['hardware']>0 for r in waits),'Missing real borrowed-fence waits')
    satisfied={r['index'] for r in records if r.get('nativeGpuQueueWaitDependenciesSatisfied') is True}
    require(len({r['index'] for r in waits})==len(waits) and all(r['index'] in satisfied for r in waits),'Unretired/replayed waits')
    require(clean['completedNativeSubmissions']>0 and clean['acceptedAsyncSubmissions']==clean['completedNativeSubmissions'] and
            clean['acceptedGpuQueueWaits']==clean['satisfiedGpuQueueWaits']>0,'Incomplete native work')
    window=one('nativeSharedTexturePresenterOpened')
    require((window['width'],window['height'],window['sameAdapter'],window['cpuUpload'])==(3840,2160,True,False),'Wrong native GPU output')
    require(report['hostBridgeExit']==0 and clean['driverCleanupVerified'] is True and vm['ownedQemuExited'] is True and
            vm['qemuExit']==0 and vm['qemuForcedStop'] is False and vm['fenceControlFailed'] is False,'Unclean owned VM exit')
    require(all(v==0 for k,v in clean.items() if k.startswith(('failed','live','pending','forced')) or k.endswith('Timeouts')) and
            all(v==0 for k,v in vm.items() if k.startswith('live')),'Unreleased native state')
    teardown=one('nativeCoordinatorTeardown')
    require(teardown['vmReaped'] is True and teardown['globalPhases']==4 and teardown['allWireObjectsReleased'] is True,'Wrong teardown ordering')
    return dict(schema=1,independentNvidiaClientInitializationVerified=True,compositorPid=composers[0],quickshellPids=shells,
        renderer=RENDERER,borrowedGpuWaitsRetired=len(waits),completedNativeSubmissions=clean['completedNativeSubmissions'],
        deferredFenceMappings=vm['fenceMappingsReleasedAfterVmExit'],deferredAllocationMappings=vm['allocationMappingsReleasedAfterVmExit'],
        isolatedShellGpuClients=manifest['isolatedShellGpuClients'],gpuDesktopFrameVerified=False,desktopAcceptanceVerified=False,
        normalOmarchySessionVerified=False,allFramesDisplayedVerified=False,nearNativePerformanceVerified=False)


@lru_cache(maxsize=4)
def pixel_digest(pixels):
    return fnv(pixels),hashlib.sha256(pixels).hexdigest()


def validate_shell_frame(report,guest,records,frames,source):
    """Accept the actual GPU shell bar, keeping wallpaper/apps/product unaccepted."""
    result=validate(report,guest,records,source)
    require('NATIVE_PRIME_CONTRACT_PASS checks=980 repeatedImportSameHandle=true globalLimit=256 retainedAfterFdClose=true fileExitReleases=true ordinaryUid=1000 foreignRejected=true cpuMap=false gpu=false' in guest,
            'Missing real guest PRIME/lifetime/ordinary-user controls')
    require('OMARCHY_DESKTOP_COORDINATOR_WAIT shellLayerObserved=true ' in guest,'Invisible shell')
    receipts=re.findall(r'^OMARCHY_DESKTOP_LAYERS_HEX ([0-9a-f]+)$',guest,re.M)
    require(len(receipts)==1,'Missing/duplicate layer identity')
    layers=json.loads(bytes.fromhex(receipts[0]))['WDDM-1']['levels']['2']
    bars=[r for r in layers if r.get('namespace')=='omarchy-bar' and r.get('pid') in result['quickshellPids'] and
          (r.get('x'),r.get('y'),r.get('w'),r.get('h'),r.get('alpha'))==(0,0,3840,26,1)]
    require(len(bars)==1,'Wrong/invisible/non-NVIDIA shell layer')
    actors={int(pid):int(actor) for actor,pid in re.findall(r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=(\d+) primary=(?:true|false) pidfdAuthenticated=true nativeDriverCount=1$',guest,re.M)}
    aliases=re.findall(r'^BROKER_COORDINATOR_ALIAS donor=(\d+) recipient=(\d+) kind=2 sourceTicket=(\d+) recipientTicket=(\d+) cookie=(\d+) realFd=true$',guest,re.M)
    require(aliases and all(int(a)==actors[bars[0]['pid']] and int(b)==actors[result['compositorPid']] and all(int(v)>0 for v in (c,d,e)) for a,b,c,d,e in aliases),'Missing/foreign shell FD handoff')
    native=[r for r in records if r.get('nativeSharedReferenceAliased') is True]
    require(len(native)==len(aliases) and all(r.get('kind')=='resource' and r.get('independentNtReference') is True and
            r.get('inheritable') is False and r.get('rawHandleExposed') is False for r in native),'Wrong native reference ownership')
    inputs=[r for r in records if r.get('nativeCompositorRingInput') is True]
    require(len(inputs)==12 and [r['frame'] for r in inputs]==list(range(1,13)) and all(r.get('cpuUpload') is False and r.get('cpuReadback') is False and
            r.get('sourceCopyRetired') is True and r.get('sourceReturnedToCommon') is True and 0<r['producerFenceTarget']<=r['producerFenceObserved']<(1<<64)-1 for r in inputs),'Unretired/non-GPU frame input')
    audits=[r for r in records if r.get('nativeCompositorRingAudited') is True]
    captures=[r for r in records if r.get('nativeCompositorBinaryCapture') is True]
    require(len(audits)==len(captures)==2 and set(frames)=={'compositor-copy-1.raw','compositor-copy-2.raw'},'Wrong audit extent')
    proof=[]
    for round_,(audit,capture,frame) in enumerate(zip(audits,captures,(1,12)),1):
        name=f'compositor-copy-{round_}.raw'; pixels=frames[name]
        require(audit['frame']==frame and audit['auditRound']==capture['frame']==round_ and capture['file']==name and
                all((r['width'],r['height'],r['fourcc'])==(3840,2160,875713112) for r in (audit,capture)) and
                type(pixels) is bytes and len(pixels)==capture['bytes']==33177600 and audit['copyFenceObserved']>=frame,'Wrong/unretired 4K capture')
        checksum,digest=pixel_digest(pixels)
        require(checksum==audit['fnv1a']==capture['fnv1a'],'Framebuffer digest mismatch')
        # Independently inspect the real workspace, clock and status-icon areas.
        bright=[]
        for left,right in ((0,256),(1760,2080),(3584,3840)):
            count=sum(max(pixels[(y*3840+x)*4:(y*3840+x)*4+3])>64 for y in range(26) for x in range(left,right))
            if frame==12:require(count>50,'Missing visible shell glyphs in a required final-bar region')
            bright.append(count)
        proof.append(dict(frame=frame,rawSha256=digest,rawFnv1a=f'{checksum:016x}',width=3840,height=2160,barBrightPixels=bright))
    result.update(gpuShellFrameVerified=True,fullOmarchyDesktopFrameVerified=False,sharedResourceAliases=len(aliases),frames=proof)
    return result
