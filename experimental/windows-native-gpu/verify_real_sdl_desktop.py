"""Audit real guest NVIDIA buffers through QEMU SDL; retain product limits."""
import hashlib
import re
from functools import lru_cache
from verify_coordinated_gpu_clients import ROOT, RENDERER, pixel_digest
from verify_hyprland_lifecycle_capture import require
from verify_omarchy_live_gpu_lifecycles import receipt, validate_lifecycles, WALLPAPER
from verify_uwsm_gpu_session import session_metadata
from verify_uwsm_native_input_devices import input_discovery
from verify_uwsm_demand_staging import terminal_retirement
from verify_sustained_uwsm_resize import resize_metadata, resize_pixels
from verify_qmp_input_response import response_metadata
from verify_qmp_input_delivery import snapshot

BRIDGE='c7a58f467fb9c29e53f0c6dff006fc0b0efa5f6990628586695fb3751e21ac6d'
QEMU='5427390c60d90071000b0be5402fcb1f7eaac187e276c21c9544aa7d7ee065d3'
GUEST='ce16f1c661fc17d062f722b746b0b2537e8fd24e3321e82ff32d421ebd152433'
HARNESS='e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286'


def bgra_to_rgba(raw):
    result=bytearray(raw)
    result[0::4]=raw[2::4];result[2::4]=raw[0::4]
    return bytes(result)


@lru_cache(maxsize=4)
def linear_blit(source,sw,sh,width,height):
    """Full-viewport GL_LINEAR reference, returned in glReadPixels row order.

    Fixed from compiled ui/sdl2-gl.c and ui/egl-helpers.c, before capture.
    Integer pixel-center weights, edge clamp, nearest UNORM code value.
    This deliberately avoids image-library antialias downsampling kernels.
    """
    require(len(source)==sw*sh*4 and 0<width<=sw and 0<height<=sh,'Invalid linear blit extent')
    dx,dy=2*width,2*height;denominator=dx*dy
    xs=[]
    for x in range(width):
        left,weight=divmod((2*x+1)*sw-width,dx)
        xs.append((max(0,min(sw-1,left))*4,max(0,min(sw-1,left+1))*4,weight))
    result=bytearray(width*height*4)
    for y in range(height):
        top,fy=divmod((2*y+1)*sh-height,dy)
        first=max(0,min(sh-1,top))*sw*4;second=max(0,min(sh-1,top+1))*sw*4
        out=(height-y-1)*width*4
        for a,b,fx in xs:
            wa=(dx-fx)*(dy-fy);wb=fx*(dy-fy);wc=(dx-fx)*fy;wd=fx*fy
            for channel in range(4):
                value=(source[first+a+channel]*wa+source[first+b+channel]*wb+
                       source[second+a+channel]*wc+source[second+b+channel]*wd)
                result[out+channel]=(value+denominator//2)//denominator
            out+=4
    return bytes(result)


def pixel_audit(native,source,window,width,height):
    require(len(native)==len(source)==33177600 and source==bgra_to_rgba(native),
            'Complete SDL source differs from actual guest GPU buffer')
    require(type(width) is int and type(height) is int and len(window)==width*height*4,
            'Incomplete SDL window')
    expected=linear_blit(source,3840,2160,width,height)
    maximum=max(abs(a-b) for a,b in zip(window,expected))
    require(maximum<=2,'SDL window differs from independent full GL_LINEAR blit')
    return dict(sourceExact=True,windowWidth=width,windowHeight=height,
                maximumChannelError=maximum,sourceSha256=hashlib.sha256(source).hexdigest(),
                windowSha256=hashlib.sha256(window).hexdigest())


def validate_real_sdl_desktop(report,guest,records,frames,source,wallpaper_reference,resize_reference):
    def rows(key):
        values=[row for row in records if key in row]
        require(all(row[key] is True for row in values),'Failed '+key)
        return values
    def one(key):
        values=rows(key);require(len(values)==1,'Missing/duplicate '+key);return values[0]
    manifest=source['manifest'];clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256']==BRIDGE and
            report['qemuSha256']==QEMU and report['initramfsSha256']==manifest['initramfsSha256']==GUEST and
            source['ownedRuntimeHarnessSha256']==HARNESS,'Mixed compiled guest/host/runtime provenance')
    require(source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT,'Source changed')
    artifact=source['qemuSdlBuildReceipt']
    require(artifact['revision']=='39f4b02c901c0e5597358ca55f332a34b35fce6e' and
            artifact['archiveSha256']=='86f8a801a184182dc1cde56c0e7241f1958bbd3195f7823707ea16a23c2226c1' and
            artifact['files']['bin/qemu-system-x86_64.exe']==QEMU and
            artifact['files']['bin/SDL2.dll']=='2e44d5bae9a97859eccde14ad4467df14bae7468594bba7fe8eb417f517ce31b',
            'Different QEMU/SDL build')
    require(manifest['expectedFrames']==300 and manifest['kernelCpuCapacityProfile']==13 and
            manifest['ordinaryUid']==1000 and manifest['desktopGeometryProbe']==[3840,2160] and
            manifest['coordinatedNativeActors'] is True and manifest['mesaWaylandGpuProbe'] is True and
            manifest['qtQuickSoftwareProbe'] is False and manifest['sdlStreamEndProbe']==dict(
                finalReplyRetiresOutput=True,lateFramesRejected=True,ackBeforeVmStop=True), 'Wrong guest fixture')
    require('--driver-sdl-relay' in source['command'] and '--driver-rootfs-readonly' in source['command'] and
            report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
            report['capturedPrivateFixturesUsed'] is False,'Foreign/software GPU path')
    require(report['hostBridgeExit']==0 and vm['ownedQemuExited'] is True and vm['qemuExit']==0 and
            vm['qemuForcedStop'] is vm['fenceControlFailed'] is False and clean['driverCleanupVerified'] is True,
            'Unclean owned runtime exit')
    require(all(value==0 for key,value in clean.items() if key.startswith(('failed','live','pending','forced')) or
                key.endswith('Timeouts')) and all(value==0 for key,value in vm.items() if
                key.startswith('live') or key.endswith('ReleasedAfterVmExit')),'Unreleased/failed native work')
    require(clean['completedCompositorFrames']==300 and clean['acceptedAsyncSubmissions']==clean['completedNativeSubmissions']>0 and
            clean['acceptedGpuQueueWaits']==clean['satisfiedGpuQueueWaits']>0 and
            clean['reportedGpuUsageLimitBytes']==1073741824 and clean['peakReportedGpuUsageBytes']<=1073741824 and
            clean['vendorCpuMappedByteLimit']==201326592 and clean['peakVendorCpuBytes']<=201326592 and
            clean['vendorGpuMappedByteLimit']==1073741824 and clean['peakVendorMappedPages']*4096<=1073741824 and
            clean['cpuAllocationFingerprintCalls']==clean['cpuAllocationFingerprintBytes']==0,'GPU work or budget guard differs')
    require(not any(token in guest for token in ('HYPRLAND_HOST_QUEUE_FAILURE','GL_OUT_OF_MEMORY','Quickshell has crashed',
            'qtQuickSoftware=true','llvmpipe','softpipe')) and
            'HYPRLAND_HOST_QUEUE_END_ACK frame=300 outputDrained=true' in guest and
            'HYPRLAND_HOST_QUEUE_END frames=300 target=300 failed=0' in guest and
            'NORMAL_ROOT_GRAPHICS_EXIT result=0 normalOmarchySession=false' in guest,'Guest graphics failed')
    peers={int(pid):bytes.fromhex(path).decode() for pid,path in re.findall(
        r'^BROKER_COORDINATOR_PEER pid=(\d+) primary=(?:true|false) exePathHex=([0-9a-f]+) pidfdAuthenticated=true$',guest,re.M)}
    actors={int(pid):int(actor) for actor,pid in re.findall(
        r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=(\d+) primary=(?:true|false) pidfdAuthenticated=true nativeDriverCount=1$',guest,re.M)}
    compositor=[pid for pid,path in peers.items() if path=='/usr/bin/Hyprland']
    shells=[pid for pid,path in peers.items() if path=='/usr/bin/quickshell' and
            any(f'omarchy-shell[{pid}]: ' in line and RENDERER in line for line in guest.splitlines())]
    require(len(compositor)==len(shells)==1 and len(actors)==7,'Missing authenticated NVIDIA desktop clients')
    app=snapshot(guest,'OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX')[0]['pid']
    client_result=dict(compositorPid=compositor[0],quickshellPids=shells)
    _,lifecycles=validate_lifecycles(report,guest,records,source,actors,peers,app,client_result)
    session=session_metadata(guest,source,compositor[0]);devices=input_discovery(guest,source)
    resizing=resize_metadata(guest,source);resize_proof=resize_pixels(frames,resize_reference)
    response=response_metadata(guest,frames)
    terminal=terminal_retirement(guest,records,compositor[0])
    opened=one('nativeSdlCompositorOpened');channel=one('nativeSdlGraphicsChannelOpened')
    require(opened['exportHandlesClosed']==3 and opened['inputImplementation']==channel['inputImplementation']=='official-qemu-sdl' and
            opened['session']==channel['session'] and opened['adapterLuid']==channel['adapterLuid'] and
            channel['socketPeerVerified'] is channel['dedicatedQmp'] is True and
            not rows('nativeSharedTexturePresenterOpened'),'Wrong SDL/input ownership')
    inputs=rows('nativeCompositorRingInput');outputs=rows('nativeCompositorRingFrame');queued=rows('nativeSdlCompositorQueued')
    replies=re.findall(r'^HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=(\d+) audit=([01]) retries=\d+ syncFileClosed=true eglSyncDestroyed=true guestCpuMap=false sourceCopyRetired=true displayVerified=false$',guest,re.M)
    require(len(inputs)==len(outputs)==len(queued)==len(replies)==300,'Missing/duplicate frame handoff')
    for number,(ingress,egress,relay,reply) in enumerate(zip(inputs,outputs,queued,replies),1):
        require(ingress['frame']==egress['frame']==relay['frame']==int(reply[0])==number and
                int(reply[1])==int(number in (1,300)) and ingress['sourceCopyRetired'] is ingress['sourceReturnedToCommon'] is True and
                ingress['inputCopyFenceObserved']>=number and ingress['producerFenceObserved']>=ingress['producerFenceTarget']>0 and
                ingress['cpuUpload'] is ingress['cpuReadback'] is relay['cpuUpload'] is relay['ordinaryCpuFenceWait'] is False and
                egress['cpuReadback'] is relay['audit'] is (number in (1,300)) and relay['gpuReuseWaitQueued'] is (number>1),
                'Frame order, real fence retirement or GPU-only ordinary handoff differs')
        state=relay['qemuState']
        require(state['attached'] is state['rootAdapterVerified'] is state['independentFences'] is True and
                state['sourceWidth']==3840 and state['sourceHeight']==2160 and state['sourceFormat']=='bgra8' and
                state['lastQueuedFrame']==state['convertedFrames']==number and state['auditFrames']==1+int(number==300) and
                state['liveImportedObjects']==4 and state['adapterLuid']==opened['adapterLuid'] and
                state['vendor']=='NVIDIA Corporation' and 'RTX 5090' in state['renderer'] and state['poisoned'] is False and
                all(state[key] is False for key in ('cpuTextureUpload','physicalInputVerified','physicalScanoutVerified',
                    'guestGpuScanoutVerified','nearNativePerformanceVerified')),'Wrong/overclaimed SDL GPU state')
    audits=rows('nativeCompositorRingAudited');captures=rows('nativeCompositorBinaryCapture');proof=[]
    require(len(audits)==len(captures)==2,'Wrong complete readback count')
    for index,number in enumerate((1,300),1):
        raw=frames[f'compositor-copy-{index}.raw'];state=queued[number-1]['qemuState']
        checksum,_=pixel_digest(raw)
        require(audits[index-1]['frame']==number and captures[index-1]['frame']==index and
                audits[index-1]['fnv1a']==captures[index-1]['fnv1a']==checksum and
                audits[index-1]['auditFenceObserved']==index*9 and audits[index-1]['copyFenceObserved']>=number,
                'Native GPU capture/fence differs')
        src=frames[f'relay-source-{number:016d}.rgba'];window=frames[f'relay-window-{number:016d}.rgba']
        audit=pixel_audit(raw,src,window,state['windowWidth'],state['windowHeight'])
        require(f'{pixel_digest(src)[0]:016x}'==state['sourceFnv1a'] and
                f'{pixel_digest(window)[0]:016x}'==state['windowFnv1a'],'Actual SDL readback digest differs')
        proof.append(dict(frame=number,**audit))
    last=frames['compositor-copy-2.raw'];reference=wallpaper_reference
    require(reference['sourceJpegSha256']==WALLPAPER and len(reference['probes'])==689,'Wrong wallpaper oracle')
    errors=[sum(abs(a-b) for a,b in zip(last[(y*3840+x)*4:(y*3840+x)*4+3][::-1],(r,g,b)))/3
            for x,y,r,g,b in reference['probes']]
    require(sum(errors)/len(errors)<3 and max(errors)<40,'Wrong complete desktop wallpaper')
    for left,right in ((0,256),(1760,2080),(3584,3840)):
        require(sum(max(last[(y*3840+x)*4:(y*3840+x)*4+3])>64 for y in range(26) for x in range(left,right))>50,
                'Missing Omarchy bar glyphs')
    require(sum(max(last[(y*3840+x)*4:(y*3840+x)*4+3])>96 for y in range(705,1040)
                for x in range(1340,2490))>1000,'Missing readable application pixels')
    end=one('nativeSdlCompositorDetached');end_state=end['qemuState'];retired=one('nativeCompositorEndRetired')
    require(end['frames']==end['producerFenceObserved']==end['consumerFenceObserved']==retired['frame']==300 and end['audits']==2 and
            retired['beforeFinalGuestReply'] is retired['beforeOwnedVmStop'] is True and end_state['liveImportedObjects']==0 and
            all(end_state[key] is False for key in ('attached','channelProgramLive','importContextLive','localScanoutLive','poisoned')),
            'Final cross-process GPU references did not retire live')
    require(records.index(end)<records.index(one('nativeCompositorRingStopped'))<records.index(one('nativeCoordinatorTeardown')),
            'Output detach follows VM/global cleanup')
    require(one('nativeInputChannelStopped')['acknowledgements']==33,'Incomplete isolated QMP input control')
    return dict(realGuestNvidiaToSdlVerified=True,frames=proof,frameCount=300,ordinaryGpuOnlyFrames=298,
                session=session,inputDiscovery=devices,lifecycles=lifecycles,resize=resizing,resizePixels=resize_proof,
                qmpInputResponse=response,terminal=terminal,liveSdlDetachVerified=True,
                peakReportedGpuUsageBytes=clean['peakReportedGpuUsageBytes'],
                normalLauncherVerified=False,fullDesktopAccepted=False,physical4kScanoutVerified=False,
                allFramesDisplayedVerified=False,physicalInputAutomatedVerified=False,nearNativePerformanceVerified=False)
