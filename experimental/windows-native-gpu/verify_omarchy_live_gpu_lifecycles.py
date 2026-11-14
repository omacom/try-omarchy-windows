"""Audit five actual NVIDIA application lifecycles and their final 4K desktop."""
import hashlib
import json
import re
from verify_coordinated_gpu_clients import pixel_digest, validate
from verify_hyprland_lifecycle_capture import require


WALLPAPER='37746404ae9a4357442e520c34ea105780232ac3d08a9f289f00156e49f37c4e'
APPLICATION='Running on D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)'


def receipt(guest,name):
    rows=re.findall(r'^'+name+r' ([0-9a-f]+)$',guest,re.M)
    require(len(rows)==1,'Missing/duplicate '+name)
    return json.loads(bytes.fromhex(rows[0]))


def validate_lifecycles(report,guest,records,source,actors,peers,app,result):
    require(source['manifest'].get('liveRetirementProbe') is True,'Missing live-lifecycle fixture')
    enabled=[r for r in records if r.get('nativeLiveRetirementEnabled') is True]
    require(len(enabled)==1 and (enabled[0]['version'],enabled[0]['batch'],enabled[0]['phases'],enabled[0]['ordinaryClientOpcode'])==(1,16,4,False),
            'Missing trusted-only bounded retirement agreement')
    require('BROKER_COORDINATOR_LIVE_RETIREMENT version=1 batch=16 phases=4 trustedControllerOnly=true' in guest,
            'Missing broker retirement agreement')
    warmup=[(int(n),int(pid)) for n,pid in re.findall(r'^OMARCHY_GPU_LIVE_WARMUP_READY cycle=(\d+) pid=(\d+) ptyMarkerObserved=true gpuProof=false$',guest,re.M)]
    require([n for n,_ in warmup]==[1,2,3,4],'Missing ordered warm-up application cycles')
    pids=[pid for _,pid in warmup]+[app]
    require(len(set(pids))==5 and len(result['quickshellPids'])==1 and
            set(actors)=={*pids,*result['quickshellPids'],result['compositorPid']},'Foreign/restarted/missing GPU clients')
    require([actors[pid] for pid in pids]==sorted(actors[pid] for pid in pids) and max(actors[pid] for pid in pids)>4,
            'Actor reuse beyond the four-live-actor bound not exercised')
    for pid in pids:
        require(peers.get(pid)=='/opt/wddm-root/bin/alacritty' and
                any(f'omarchy-gpu-app[{pid}]: ' in line and APPLICATION in line for line in guest.splitlines()) and
                any(f'omarchy-gpu-app[{pid}]: ' in line and 'Using OpenGL 3.3 renderer' in line for line in guest.splitlines()),
                'Missing PID-correlated NVIDIA lifecycle application')
    snapshots=re.findall(r'^OMARCHY_GPU_LIVE_WARMUP_CLIENTS_HEX ([0-9a-f]+)$',guest,re.M)
    require(len(snapshots)==4,'Missing warm-up window snapshots')
    for snapshot,(_,pid) in zip(snapshots,warmup):
        clients=json.loads(bytes.fromhex(snapshot))
        windows=[c for c in clients if c.get('pid')==pid and c.get('class')=='omarchy-gpu-terminal' and c.get('title')=='Omarchy terminal' and
                 c.get('mapped') is True and c.get('hidden') is False and c.get('visible') is True and c.get('xwayland') is False and
                 c.get('at')==[1320,693] and c.get('size')==[1200,800] and c.get('floating') is True]
        require(len(windows)==1,'Missing actual visible warm-up window')
    closed=[tuple(map(int,row)) for row in re.findall(r'^BROKER_COORDINATOR_LIVE_RETIRED actor=(\d+) pid=(\d+) destroyedWireObjects=(\d+) steps=(\d+) processExited=true cpuViewsRevoked=true sharedFdsDrained=true$',guest,re.M)]
    require(len(closed)==6 and len({a for a,_,_,_ in closed})==6 and
            {pid for _,pid,_,_ in closed}=={*pids,*result['quickshellPids']},'Incomplete/duplicate native actor retirement')
    closed_by_pid={pid:(actor,count,steps) for actor,pid,count,steps in closed}
    witnessed=[tuple(map(int,row)) for row in re.findall(r'^OMARCHY_GPU_LIVE_RETIREMENT_OBSERVED pid=(\d+) actor=(\d+) compositorStillRunning=true rootOwnedWitness=true gpuProof=false$',guest,re.M)]
    require([pid for pid,_ in witnessed]==pids and all(actors[pid]==actor for pid,actor in witnessed),
            'Missing ordered live-compositor retirement witnesses')
    steps=[r for r in records if r.get('nativeLiveRetirementStep') is True]
    require(set(r['actor'] for r in steps)=={a for a,_,_,_ in closed},'Missing/foreign native retirement steps')
    timings=[tuple(map(int,row)) for row in re.findall(
        r'^BROKER_COORDINATOR_LIVE_RETIREMENT_TIMING actor=(\d+) elapsedMs=(\d+) steps=(\d+)$',guest,re.M)]
    require(len(timings)==6 and len({a for a,_,_ in timings})==6 and
            {a for a,_,_ in timings}=={a for a,_,_,_ in closed},'Missing/duplicate retirement timing receipts')
    for actor,elapsed,count in timings:
        rows=[r for r in steps if r['actor']==actor]
        require(len(rows)==count and all(type(r.get('nativeElapsedUs')) is int and r['nativeElapsedUs']>=0 for r in rows) and
                sum(r['nativeElapsedUs'] for r in rows)<=elapsed*1000+10000,
                'Native/broker retirement timings do not reconcile')
    for pid in [*pids,*result['quickshellPids']]:
        actor,count,total_steps=closed_by_pid[pid]
        require(actors[pid]==actor and count>0,'Retirement completed without actual owned objects')
        rows=[r for r in steps if r['actor']==actor]
        require(len(rows)==total_steps and rows[-1]['phase']==4 and rows[-1]['remaining']==0 and
                sum(r['destroyed'] for r in rows)==count,'Retirement batch totals do not reconcile')
        previous=None;phase=0
        for row in rows:
            require(phase<=row['phase']<=4 and 0<=row['remaining']<=1024 and 0<=row['attempted']<=16 and
                    row['destroyed']==row['attempted'] and row['sharedReferences']==0 and row['ntstatus']==0 and row['flags'] in (0,1) and
                    (row['gpuQuiescenceVerified'] is (row['attempted']>0)) and
                    (not row['flags'] or row['attempted']==row['destroyed']==0) and
                    (previous is None or previous==row['remaining']+row['destroyed']),'Unsafe/unbounded/inconsistent native retirement batch')
            previous=row['remaining'];phase=row['phase']
    group=next(r for r in records if r.get('nativeCoordinatorSummary') is True)
    require(group['liveRetiredActors']==6 and group['liveRetiredWireObjects']==sum(r['destroyed'] for r in steps) and
            group['peakActors']==3 and group['retainedActors']<=1,'Actor slots/budgets were not reclaimed')
    vm=report['ownedQemuControl'];clean=report['disconnectCleanup']
    require(all(vm[k]==0 for k in ('fenceMappingsReleasedAfterVmExit','allocationMappingsReleasedAfterVmExit','allocationSlotMappingsReleasedAfterVmExit')) and
            vm['fenceMappingsCreated']==vm['fenceUnmapAcknowledgements'] and
            vm['allocationMappingsCreated']==vm['allocationUnmapAcknowledgements'] and
            vm['allocationSlotMappingsCreated']==vm['allocationSlotUnmapAcknowledgements'],
            'Aperture mappings still depend on VM-exit cleanup')
    require(all(clean[k]==0 for k in ('hwQueuesReleasedAfterVmExit','syncObjectsReleasedAfterVmExit','gpuReservationsReleasedAfterVmExit','cpuLocksReleasedAfterVmExit')),
            'Native child resources still depend on VM exit')
    for index,pid in enumerate(pids):
        root_position=guest.index(f'BROKER_COORDINATOR_LIVE_RETIRED actor={actors[pid]} pid={pid} ')
        witness_position=guest.index(f'OMARCHY_GPU_LIVE_RETIREMENT_OBSERVED pid={pid} actor={actors[pid]} ')
        require(root_position<witness_position<guest.index('HYPRLAND_STARTUP_EXIT_REQUEST method=hyprctl-dispatch-exit'),
                'Retirement witness follows compositor shutdown')
        if index<4:
            require(guest.index(f'OMARCHY_GPU_LIVE_WARMUP_READY cycle={index+1} pid={pid} ')<root_position and
                    witness_position<guest.index(f'BROKER_COORDINATOR_PEER pid={pids[index+1]} '),
                    'Next client started before previous live retirement completed')
    first_input=next(i for i,r in enumerate(records) if r.get('nativeCompositorRingInput') is True)
    last_input=max(i for i,r in enumerate(records) if r.get('nativeCompositorRingInput') is True)
    for pid in pids[:4]:
        require(max(i for i,r in enumerate(records) if r.get('nativeLiveRetirementStep') is True and r['actor']==actors[pid])<first_input,
                'Warm-up resource reuse did not precede the captured desktop')
    require(last_input<min(i for i,r in enumerate(records) if r.get('nativeLiveRetirementStep') is True and r['actor']==actors[app]),
            'Final application retired before the verified compositor handoffs')
    return pids,dict(gpuLiveRetirementVerified=True,gpuApplicationCycles=5,applicationPids=pids,
        peakConcurrentActors=group['peakActors'],largestApplicationActor=max(actors[p] for p in pids),
        liveRetiredActors=group['liveRetiredActors'],liveRetiredWireObjects=group['liveRetiredWireObjects'],
        deferredApertureMappings=0,retirementBeforeCompositorExitVerified=True)


def validate_content_frame(report,guest,records,frames,source,wallpaper_reference):
    profile=source['manifest']['kernelCpuCapacityProfile']
    require(profile==13,'Unsupported desktop-content profile')
    result=validate(report,guest,records,source,profile=profile)
    require(source['manifest'].get('desktopContentProbe') is True and source['manifest'].get('desktopFenceProfile') is True,
            'Content fixture was not requested')
    require(f'APERTURE_CONTRACT_PASS checks={1644} activeLeases=0 activeCpuBytes=0' in guest and
            'NATIVE_PRIME_CONTRACT_PASS checks=980' in guest,'Missing actual kernel/PRIME controls')
    require('OMARCHY_WALLPAPER_DECODE width=3840 height=2160 sourceImageUnchanged=true ownedRamOverlayOnly=true gpuProof=false' in guest and
            'OMARCHY_GPU_APPLICATION_CONTENT_READY ptyMarkerObserved=true gpuProof=false' in guest and
            'OMARCHY_GPU_APPLICATION_CLOSED clientUnmapped=true gpuProof=false' in guest and
            'NORMAL_ROOT_GRAPHICS_EXIT result=0 normalOmarchySession=false' in guest,'Incomplete content readiness/close/service lifecycle')
    require(not any(t in guest for t in ('GL_OUT_OF_MEMORY','GPU reset (','Quickshell has crashed','qtQuickSoftware=true')),
            'Application/desktop graphics failed or fell back')
    profiles=[r for r in records if r.get('openGlProfileNegotiated') is True]
    require(len(profiles)==1 and profiles[0]['version']==profile and profiles[0]['reservationLimit']==128 and
            profiles[0]['reservedByteLimit']==17179869184,'Wrong per-client address-space agreement')
    clean=report['disconnectCleanup']
    require(clean['cpuAllocationFingerprintsEnabled'] is False and clean['cpuStoreTestRequested'] is False and
            clean['cpuAllocationFingerprintCalls']==clean['cpuAllocationFingerprintBytes']==0,
            'Ordinary GPU mappings incurred diagnostic CPU memory scans')
    require(clean['vendorCpuSlotLimit']==1024 and
            clean['vendorCpuMappedByteLimit']==201326592 and clean['peakVendorCpuBytes']<=201326592 and
            clean['vendorGpuMappedByteLimit']==1073741824 and clean['peakVendorMappedPages']*4096<=1073741824 and
            clean['reportedGpuUsageLimitBytes']==1073741824 and clean['peakReportedGpuUsageBytes']<=1073741824,
            'Desktop content exceeded an unchanged byte guard')
    require(clean['sharedResourceReferenceLimit']==clean['sharedSyncReferenceLimit']==64 and 0<clean['peakSharedResourceHandles']<=64 and clean['peakSharedSyncHandles']<=64,'Aggregate native reference fix not exercised')
    require(all(clean[key]==0 for key in ('vendorCpuSlotQuotaRejections','vendorCpuByteQuotaRejections',
                'gpuReservationQuotaRejections','hwQueueQuotaRejections','syncQuotaRejections',
                'gpuStateRetirementTimeouts')),'Desktop admission or GPU-state retirement failed')
    require(not any(r.get(key) is True for r in records for key in
                ('wireCpuByteAdmissionRejected','nativeCpuByteAdmissionRejected','nativeObjectAdmissionRejected')),
            'A client hit a wire/native admission guard')
    policies=[r for r in records if r.get('nativeGpuStateMappingPolicy') is True]
    require(len(policies)==clean['completedGpuStateMaps'] and len(policies)>0 and
            any(r['retirementRequired'] is False for r in policies) and
            all(r['pagingFenceRetired'] is True and r['cpuFenceWritten'] is False and
                (r['allocationOverlap'] is False or r['retirementRequired'] is True) and
                r['retirementScope'] in ('pagingFence','owningDevice','allDevices') and
                (r['retirementScope']!='owningDevice' or r['allocationOverlap'] is r['retirementRequired'] is True) and
                (r['retirementScope']!='pagingFence' or r['retirementRequired'] is r['allocationOverlap'] is False)
                for r in policies) and any(r['retirementScope']=='owningDevice' for r in policies),
            'Missing actual paging retirement or unsafe allocation-state replacement')
    closed_busy=[r for r in records if r.get('nativeNtReferenceClosedDuringGpuWork') is True]
    require(len(closed_busy)==clean['ntReferencesClosedDuringGpuWork'] and
            all(r['kind'] in ('resource','sync') and r['backingObjectsDestroyed'] is False and
                r['recipientReferencesIndependent'] is True for r in closed_busy),
            'Exported NT reference closure destroyed or lost backing objects')
    for name,digest in source['manifest']['sources'].items():
        if name.endswith(('.h','.cpp')):
            require(source['nativeBuildReceipt']['sources'].get('experimental/windows-native-gpu/'+name)==digest,
                    'Mixed native/guest source snapshot')
    peers={int(pid):bytes.fromhex(path).decode() for pid,path in re.findall(
        r'^BROKER_COORDINATOR_PEER pid=(\d+) primary=(?:true|false) exePathHex=([0-9a-f]+) pidfdAuthenticated=true$',guest,re.M)}
    actor_rows=[(int(pid),int(actor)) for actor,pid in re.findall(
        r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=(\d+) primary=(?:true|false) pidfdAuthenticated=true nativeDriverCount=1$',guest,re.M)]
    require(len({pid for pid,_ in actor_rows})==len(actor_rows),'Unverified actor restart/reconnection')
    actors=dict(actor_rows)
    clients=receipt(guest,'OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX')
    windows=[c for c in clients if c.get('class')=='omarchy-gpu-terminal' and c.get('title')=='Omarchy terminal' and
             c.get('mapped') is True and c.get('hidden') is False and c.get('visible') is True and c.get('xwayland') is False and
             c.get('at')==[1320,693] and c.get('size')==[1200,800] and c.get('floating') is True]
    require(len(windows)==1,'Missing/wrong application window')
    app=windows[0]['pid']
    cycle_pids,lifecycle=validate_lifecycles(report,guest,records,source,actors,peers,app,result)
    require(peers.get(app)=='/opt/wddm-root/bin/alacritty' and app in actors and
            any(f'omarchy-gpu-app[{app}]: ' in line and APPLICATION in line for line in guest.splitlines()) and
            any(f'omarchy-gpu-app[{app}]: ' in line and 'Using OpenGL 3.3 renderer' in line for line in guest.splitlines()),
            'Missing PID-correlated actual NVIDIA application renderer')
    levels=receipt(guest,'OMARCHY_DESKTOP_LAYERS_HEX')['WDDM-1']['levels']
    layers=[row for group in levels.values() for row in group]
    for namespace,size in (('omarchy-background',(3840,2160)),('omarchy-bar',(3840,26))):
        require(len([r for r in layers if r.get('namespace')==namespace and r.get('pid') in result['quickshellPids'] and
                     (r.get('x'),r.get('y'),r.get('w'),r.get('h'),r.get('alpha'))==(0,0,*size,1)])==1,
                'Missing/invisible/non-NVIDIA '+namespace)
    aliases=re.findall(r'^BROKER_COORDINATOR_ALIAS donor=(\d+) recipient=(\d+) kind=2 sourceTicket=(\d+) recipientTicket=(\d+) cookie=(\d+) realFd=true$',guest,re.M)
    donors={actors[pid] for pid in [*cycle_pids,*result['quickshellPids']]}
    compositor=actors[result['compositorPid']]
    require(aliases and all(int(a) in donors and int(b)==compositor and all(int(v)>0 for v in (c,d,e)) for a,b,c,d,e in aliases) and
            all(any(int(a)==donor for a,*_ in aliases) for donor in donors),'Missing/foreign application or shell native FD handoff')
    native=[r for r in records if r.get('nativeSharedReferenceAliased') is True]
    require(len(native)==len(aliases) and all(r.get('kind')=='resource' and r.get('independentNtReference') is True and
            r.get('inheritable') is False and r.get('rawHandleExposed') is False for r in native),'Invalid native resource ownership')
    inputs=[r for r in records if r.get('nativeCompositorRingInput') is True]
    require(len(inputs)==12 and [r['frame'] for r in inputs]==list(range(1,13)) and
            all(r.get('cpuUpload') is False and r.get('cpuReadback') is False and r.get('sourceCopyRetired') is True and
                r.get('sourceReturnedToCommon') is True and 0<r['producerFenceTarget']<=r['producerFenceObserved']<(1<<64)-1 for r in inputs),
            'Incomplete/non-GPU compositor inputs')
    audits=[r for r in records if r.get('nativeCompositorRingAudited') is True]
    captures=[r for r in records if r.get('nativeCompositorBinaryCapture') is True]
    require(len(audits)==len(captures)==2 and set(frames)=={'compositor-copy-1.raw','compositor-copy-2.raw'},'Wrong readback extent')
    require(all(r.get('readbackTiles')==9 and r.get('readbackTileBytes')==3932160 and r.get('backbufferSource') is True and
                r.get('auditFenceObserved')==9*(n+1) for n,r in enumerate(audits)),
            'Missing complete tiled backbuffer/fence proof')
    proof=[]
    for round_,(audit,capture,frame) in enumerate(zip(audits,captures,(1,12)),1):
        name=f'compositor-copy-{round_}.raw';pixels=frames[name]
        require(audit['frame']==frame and audit['auditRound']==capture['frame']==round_ and capture['file']==name and
                all((r['width'],r['height'],r['fourcc'])==(3840,2160,875713112) for r in (audit,capture)) and
                type(pixels) is bytes and len(pixels)==capture['bytes']==33177600 and audit['copyFenceObserved']>=frame,'Wrong/unretired 4K bytes')
        checksum,digest=pixel_digest(pixels)
        require(checksum==audit['fnv1a']==capture['fnv1a'],'Full native capture digest mismatch')
        require(pixels[3::4]==b'\xff'*(3840*2160),'Unexpected output alpha bytes')
        proof.append(dict(frame=frame,rawSha256=digest,rawFnv1a=f'{checksum:016x}',width=3840,height=2160))
    pixels=frames['compositor-copy-2.raw']
    for left,right in ((0,256),(1760,2080),(3584,3840)):
        require(sum(max(pixels[(y*3840+x)*4:(y*3840+x)*4+3])>64 for y in range(26) for x in range(left,right))>50,
                'Missing shell glyphs')
    glyphs=sum(max(pixels[(y*3840+x)*4:(y*3840+x)*4+3])>96 for y in range(705,1040) for x in range(1340,2490))
    require(glyphs>1000,'Application body contains no verified visible text')
    reference=wallpaper_reference
    require(reference['sourceJpegSha256']==WALLPAPER and len(reference['probes'])==689,'Wrong wallpaper reference')
    errors=[]
    for x,y,r,g,b in reference['probes']:
        require(0<=x<3840 and 26<=y<2160 and not (1280<=x<=2560 and 650<=y<=1540),'Wallpaper probe overlaps application')
        actual=pixels[(y*3840+x)*4:(y*3840+x)*4+3][::-1]
        errors.append(sum(abs(a-e) for a,e in zip(actual,(r,g,b)))/3)
    mean=sum(errors)/len(errors)
    require(mean<3 and max(errors)<40,'Captured wallpaper does not match independent shipped-image samples')
    result.update(gpuDesktopFrameVerified=True,fullOmarchyDesktopFrameVerified=True,wallpaperGpuVerified=True,
        applicationGpuVerified=True,applicationPid=app,applicationVisibleGlyphPixels=glyphs,sharedResourceAliases=len(aliases),
        wallpaperSampleCount=len(errors),wallpaperMeanAbsoluteRgbError=mean,frames=proof)
    result.update(lifecycle)
    return result
