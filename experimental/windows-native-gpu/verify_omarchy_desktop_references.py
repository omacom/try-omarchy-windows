"""Audit a bounded GPU desktop capture, without accepting a production desktop."""
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
    require(clean['vendorCpuSlotLimit']==1024 and
            clean['vendorCpuMappedByteLimit']==201326592 and clean['peakVendorCpuBytes']<=201326592 and
            clean['vendorGpuMappedByteLimit']==1073741824 and clean['peakVendorMappedPages']*4096<=1073741824 and
            clean['reportedGpuUsageLimitBytes']==1073741824 and clean['peakReportedGpuUsageBytes']<=1073741824,
            'Desktop content exceeded an unchanged byte guard')
    require(clean['sharedResourceReferenceLimit']==clean['sharedSyncReferenceLimit']==64 and 16<clean['peakSharedResourceHandles']<=64 and clean['peakSharedSyncHandles']<=64,'Aggregate native reference fix not exercised')
    require(all(clean[key]==0 for key in ('vendorCpuSlotQuotaRejections','gpuReservationQuotaRejections',
                'hwQueueQuotaRejections','syncQuotaRejections')),'Desktop admission failed')
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
    donors={actors[pid] for pid in [app,*result['quickshellPids']]}
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
    return result
