"""Audit every handoff in an explicit 300-frame 4K NVIDIA desktop workload."""
import json
import re
from verify_coordinated_gpu_clients import pixel_digest, validate
from verify_hyprland_lifecycle_capture import require
from verify_omarchy_live_gpu_lifecycles import APPLICATION, WALLPAPER, receipt, validate_lifecycles
from verify_uwsm_gpu_session import session_metadata
from verify_uwsm_native_input_devices import input_discovery
from verify_uwsm_demand_staging import terminal_retirement
from verify_reclaimed_staging_input_response import delivery_metadata
from verify_qmp_input_response import response_metadata

EXPECTED_FRAMES = 300


def validate_content_frame(report,guest,records,frames,source,wallpaper_reference):
    require(source['manifest'].get('expectedFrames')==EXPECTED_FRAMES and source['manifest'].get('sustainedDesktopProbe')==dict(expectedFrames=300,bounded=True),'Missing exact 300-frame fixture')
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
    require(len(inputs)==EXPECTED_FRAMES and [r['frame'] for r in inputs]==list(range(1,EXPECTED_FRAMES+1)) and
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
    for round_,(audit,capture,frame) in enumerate(zip(audits,captures,(1,EXPECTED_FRAMES)),1):
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

def reclaimed_staging_admission(records, backpressure):
    limit, reserve = 1073741824, 64 * 1024 * 1024
    checks = [row for row in records if 'nativePresentationMemoryGuard' in row]
    require(checks and all(row.get('nativePresentationMemoryGuard') is True and row.get('accepted') is True and
            row.get('limitBytes') == limit and all(type(row.get(k)) is int and 0 <= row[k] <= limit for k in ('usageBytes', 'additionalBytes')) and
            row['usageBytes'] + row['additionalBytes'] <= limit for row in checks), 'Missing/overflowing presentation admission')
    prefix = ['device-before', 'device-after', 'staging-before', 'staging-after', 'readback-before', 'readback-after', 'swapchain-before', 'swapchain-after']
    stages = [row.get('stage') for row in checks]
    require(stages[:8] == prefix and set(stages) <= set(prefix + ['frame-before', 'frame-admitted', 'import-before', 'import-after']),
            'Missing/reordered presentation initialization')
    require(type(backpressure) is int and 0 <= backpressure <= 4096 and stages.count('frame-before') == EXPECTED_FRAMES + backpressure and
            stages.count('frame-admitted') == EXPECTED_FRAMES and 1 <= stages.count('import-before') == stages.count('import-after') <= 3,
            'Missing frame/import admission')
    for row in checks:
        size, stage = row['additionalBytes'], row['stage']
        require(33177600 <= size <= 64 * 1024 * 1024 if stage == 'staging-before' else
                256 * 3840 * 4 <= size <= 4 * 1024 * 1024 if stage == 'readback-before' else
                2 * 33177600 <= size <= 128 * 1024 * 1024 if stage == 'swapchain-before' else size == 0,
                'Wrong staging/readback/swapchain estimate')
    live, generations, last_inputs = {}, {}, {}
    created = trimmed = historical_bytes = live_bytes = pending_trim = 0
    staging_checks = [row for row in checks if row['stage'].startswith('staging-')]
    positions = {id(row): index for index, row in enumerate(records)}
    last_frame_position = positions[id(checks[7])]
    pressure_count = reallocated = 0
    opened = stopped = 0
    input_frames = []
    for position, row in enumerate(records):
        require(not stopped or not any(key in row for key in ('nativeCompositorStagingAllocated', 'nativeCompositorStagingTrimmed',
                'nativeCompositorRingInput', 'nativeResidencyStagingPressure')), 'Staging work after presentation teardown')
        if row.get('nativeCompositorStagingAllocated') is True:
            slot, size, generation = row.get('slot'), row.get('bytes'), row.get('generation')
            require(type(slot) is int and 0 <= slot < 3 and slot not in live and type(size) is int and size > 0 and type(generation) is int and
                    generation == generations.get(slot, 0) + 1 and row.get('capacity') == 3 and row.get('onDemand') is True,
                    'Staging creation collides with a live slot or replays its generation')
            require(created < EXPECTED_FRAMES + 1 and len(staging_checks) >= (created + 1) * 2, 'Unbounded or unguarded staging creation')
            before, after = staging_checks[created * 2:created * 2 + 2]
            require(before['stage'] == 'staging-before' and after['stage'] == 'staging-after' and before['additionalBytes'] == size and
                    positions[id(before)] < positions[id(after)] < position, 'Staging published before memory admission')
            require((slot == 0 and generation == 1 and row.get('phase') == 'initialization') if created == 0 else
                    row.get('phase') == 'serialized-submit' and opened == 1, 'Staging created outside its serialized phase')
            created += 1
            historical_bytes += size
            live_bytes += size
            reallocated += int(generation > 1)
            live[slot] = (generation, size)
            generations[slot] = generation
            require(row.get('preparedTextures') == created and row.get('preparedBytes') == historical_bytes and
                    row.get('liveTextureBytes') == live_bytes, 'Staging byte accounting does not reconcile')
        if row.get('nativeCompositorRingOpened') is True:
            opened += 1
            require(opened == 1 and row.get('slots') == row.get('sourceCacheLimit') == 3 and row.get('onDemandStaging') is True and
                    row.get('initialStagingTextures') == 1 and row.get('ordinaryCpuReadback') is False and set(live) == {0},
                    'Changed staging capacity or eager startup allocation')
        if row.get('nativeCompositorRingInput') is True:
            slot, frame = row.get('slot'), row.get('frame')
            require(slot in live and type(frame) is int and 1 <= frame <= EXPECTED_FRAMES, 'Frame references unprepared staging')
            block = ';'.join(check['stage'] for check in checks if last_frame_position < positions[id(check)] < position) + ';'
            require(re.fullmatch(r'(?:frame-before;)+(?:staging-before;staging-after;)?(?:import-before;import-after;)?frame-admitted;', block),
                    'Input copy precedes its own memory admission')
            last_frame_position = position
            last_inputs[slot] = (live[slot][0], frame)
            input_frames.append(frame)
        if row.get('nativeCompositorStagingTrimmed') is True:
            slot, generation, size, reused = row.get('slot'), row.get('generation'), row.get('bytes'), row.get('reusedAfter')
            require(type(slot) is int and slot in (1, 2) and type(generation) is int and type(size) is int and
                    live.get(slot) == (generation, size) and last_inputs.get(slot) == (generation, reused) and
                    type(reused) is int and reused > 0 and row.get('slotOccupied') is False and row.get('capacity') == 3 and
                    row.get('serializedResidency') is True and row.get('commandsReset') is True and
                    all(type(row.get(k)) is int and reused <= row[k] <= EXPECTED_FRAMES for k in ('inputCopyFenceObserved', 'outputCopyFenceObserved')),
                    'Staging reclaimed before actual copy retirement or with a stale owner')
            del live[slot]
            live_bytes -= size
            trimmed += 1
            pending_trim += size
            require(row.get('liveTextureBytes') == live_bytes and 0 in live, 'Reclamation removed the audit slot or miscounted bytes')
        if row.get('nativeResidencyStagingPressure') is True:
            usage, after = row.get('usageBefore'), row.get('usageAfter')
            require(type(usage) is int and limit - reserve < usage <= limit and type(after) is int and 0 <= after <= limit and
                    row.get('limitBytes') == limit and row.get('reserveBytes') == reserve and row.get('targetBytes') == reserve - (limit - usage) and
                    row.get('releasedBytes') == pending_trim and row.get('accepted') is True and row.get('beforeNativeResidency') is True,
                    'Pressure reclamation bypassed the unchanged residency guard or lost its trim receipt')
            pending_trim = 0
            pressure_count += 1
        if 'nativeCompositorRingStopped' in row:
            stopped += 1
            require(row.get('nativeCompositorRingStopped') is True and row.get('stagingTexturesCreated') == row.get('stagingTexturesReleased') == created and
                    row.get('stagingTextureBytes') == historical_bytes and row.get('stagingTexturesTrimmed') == trimmed and
                    row.get('liveStagingTextureBytes') == row.get('liveGpuResources') == 0, 'Staging not completely reclaimed after GPU retirement')
    require(opened == stopped == 1 and 1 <= created <= EXPECTED_FRAMES + 1 and len(staging_checks) == created * 2 and pending_trim == 0 and
            input_frames == list(range(1, EXPECTED_FRAMES + 1)) and positions[id(checks[-1])] < last_frame_position,
            'Incomplete staging initialization, trim attribution or teardown')
    return dict(capacity=3, initialTextures=1, createdTextures=created, trimmedTextures=trimmed,
                reallocatedTextures=reallocated, historicalTextureBytes=historical_bytes,
                residencyPressureObservations=pressure_count, safeReclamationObserved=trimmed > 0,
                gpuUsageLimitBytes=limit, physicalGpuQuotaVerified=False)



def native_frame_sequence(guest, records):
    total=EXPECTED_FRAMES
    replies=re.findall(r'^HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=(\d+) audit=([01]) retries=(\d+) syncFileClosed=true eglSyncDestroyed=true guestCpuMap=false sourceCopyRetired=true displayVerified=false$',guest,re.M)
    require(len(replies)==guest.count('HYPRLAND_HOST_QUEUE_RESULT ')==total and
            guest.count('HYPRLAND_HOST_QUEUE_END ')==1 and
            guest.count(f'HYPRLAND_HOST_QUEUE_END frames={total} target={total} failed=0\n')==1 and
            'HYPRLAND_HOST_QUEUE_FAILURE' not in guest,'Incomplete actual 300-frame guest sequence')
    def rows(key):
        values=[r for r in records if key in r]
        require(all(r[key] is True for r in values),'Failed '+key)
        return values
    inputs=rows('nativeCompositorRingInput')
    outputs=rows('nativeCompositorRingFrame')
    presents=rows('nativeCompositorWindowSubmitted')
    slots=rows('nativeCompositorFrameSlotReady')
    require(len(inputs)==len(outputs)==len(presents)==len(slots)==total,'Missing/duplicate native handoff')
    opened=rows('nativeSharedTexturePresenterOpened')
    require(len(opened)==1 and opened[0].get('width')==3840 and opened[0].get('height')==2160 and
            all(opened[0].get(k) is True for k in ('sameAdapter','windowVisible','frameLatencyWaitable')) and
            opened[0].get('maximumFrameLatency')==1,'Wrong 4K native presenter')
    identities=set();times=[]
    for index,(source,output,present,slot,reply) in enumerate(zip(inputs,outputs,presents,slots,replies),1):
        require(source.get('frame')==output.get('frame')==present.get('frame')==slot.get('frame')==int(reply[0])==index and
                int(reply[1])==int(index in (1,total)),'Actual frame/audit sequence differs')
        require(type(source.get('slot')) is int and 0<=source['slot']<3 and output.get('slot')==source['slot'] and
                source.get('sourceCopyRetired') is True and source.get('sourceReturnedToCommon') is True and
                source.get('cpuReadback') is source.get('cpuUpload') is False and
                type(source.get('inputCopyFenceObserved')) is int and index<=source['inputCopyFenceObserved']<(1<<64)-1,
                'Unretired/non-GPU 4K ingress')
        require(output.get('cpuReadback') is (index in (1,total)) and output.get('cpuUpload') is False and output.get('displayVerified') is False,
                'Ordinary handoff read back to CPU or claimed scanout')
        require(present.get('presentHresult')==present.get('countHresult')==0 and present.get('lastPresentCount')==index and
                present.get('cpuUpload') is False and present.get('displayVerified') is False and slot.get('maximumFrameLatency')==1,
                'Present failed or frame latency is unbounded')
        pid,chain,before,after,frequency=(present.get(k) for k in ('presentProcessId','presentSwapChain','presentCallQpcBefore','presentCallQpcAfter','qpcFrequency'))
        require(all(type(v) is int and v>0 for v in (pid,chain,before,after,frequency)) and after>=before and
                slot.get('presentProcessId')==pid and slot.get('presentSwapChain')==chain,'Wrong Present timing/owner')
        identities.add((pid,chain,frequency));times.append(before)
        require(records.index(source)<records.index(output) and records.index(present)<records.index(output),'Published frame before GPU/Present handoff')
    require(len(identities)==1 and all(a<b for a,b in zip(times,times[1:])),'Mixed/nonadvancing Present sequence')
    stopped=rows('nativeCompositorRingStopped')
    require(len(stopped)==1 and stopped[0].get('submitted')==stopped[0].get('presented')==total and
            stopped[0].get('completedAudits')==2 and stopped[0].get('liveGpuResources')==0 and
            stopped[0].get('displayVerified') is False,'Incomplete ring drain')
    return dict(actualGpuHandoffs=total,ordinaryGpuOnlyHandoffs=total-2,completeBufferAudits=2,
                everyNativeHandoffAudited=True,allFramesDisplayedVerified=False,performanceBenchmark=False)


def validate_sustained_uwsm_gpu_capture(report, guest, records, frames, source, wallpaper_reference, owner_output):
    capture=validate_content_frame(report,guest,records,frames,source,wallpaper_reference)
    session=session_metadata(guest,source,capture['compositorPid'])
    devices=input_discovery(guest,source)
    sequence=native_frame_sequence(guest,records)
    staging=reclaimed_staging_admission(records,report['disconnectCleanup']['compositorBackpressure'])
    terminal=terminal_retirement(guest,records,capture['compositorPid'])
    delivery=delivery_metadata(guest,records,source,owner_output)
    response=response_metadata(guest,frames)
    return dict(capture=capture,session=session,inputDiscovery=devices,frameSequence=sequence,staging=staging,
                terminal=terminal,inputDelivery=delivery,inputResponse=response,
                bounded300FrameGpuInputResponseVerified=True,hostWindowIntegrationVerified=False,
                normalLauncherVerified=False,fullDesktopAccepted=False,allFramesDisplayedVerified=False,
                nearNativePerformanceVerified=False)
