"""Audit guest121 pressure plus the unchanged 1,800-frame desktop.

Derived from managed desktop verifier SHA-256 892515f6f71aa3979c6eb111e73ec185087bb487ee8f128d7e431b9bbcb72a9a.
Historical auditors remain unchanged; diagnostic GPU transfers are additional.
"""
import hashlib
import re
import statistics
from verify_coordinated_gpu_clients import ROOT, RENDERER, pixel_digest
from verify_hyprland_lifecycle_capture import require
from verify_omarchy_live_gpu_lifecycles import receipt, validate_lifecycles, WALLPAPER
from verify_uwsm_gpu_session import session_metadata
from verify_uwsm_native_input_devices import input_discovery
from verify_uwsm_demand_staging import terminal_retirement
from verify_sustained_uwsm_resize import resize_metadata, resize_pixels
from verify_qmp_input_response import response_metadata
from verify_qmp_input_delivery import snapshot

from verify_real_sdl_desktop import pixel_audit
from virtio_trace import merge_session,decode_trace,HEADER

BRIDGES={'260def3126c56c8dbfcf8a4be400788c2300fb163a5115af47fbfc95c5544e1d':'82'}
QEMU_BUILDS={
    '5427390c60d90071000b0be5402fcb1f7eaac187e276c21c9544aa7d7ee065d3':(
        '39f4b02c901c0e5597358ca55f332a34b35fce6e','86f8a801a184182dc1cde56c0e7241f1958bbd3195f7823707ea16a23c2226c1'),
    '6bee9d9e18bc0e89757e0364a29e64f1057e4854ec9a7a816b063f8d8f3ed6a5':(
        '10d6857c79b5d297117926dbd8002ac133050d7d','320be928e5d0976665556b159aef9afbc8885eb3b10172124f02861461dc2963'),
}
WINDOW_AUDIT_QEMU='6bee9d9e18bc0e89757e0364a29e64f1057e4854ec9a7a816b063f8d8f3ed6a5'
OFFICIAL_INPUT={
    'hw/input/virtio-input-hid.c':'31b9150dc410ad5e2ce48a58f3d0e923dec4e012e62c7be05749786445f1aa2f',
    'include/ui/sdl2.h':'a2c75d65b71bba559706520583aa125ccc0777229ec82e760146d0b1669286d4',
    'ui/input.c':'1b450d44f6825b0d982ba616ff53da8c32481020689aee7659167140bb13dee1',
    'ui/sdl2.c':'1214c6ec9b028fb721bead9f93888807da8c260603b5149a7662a90bacf31ea6',
}
GUESTS={'uart':'419f6a165dfa9b59fd7bba66c6df94d248b20e1b7e506b617ca5ee63a63476d0','virtio':'f7fc96ede796c5b7cf876a27075444b81567a9e4be5326a29ce8119ba6ef772e'}
CURRENT_DRIVER_GUEST='7e2588a50b64403651610f5ed2d922d6868388bca4b0d0b078ab7697cad03850'
TRACE_PINS={'schema': 1, 'baselineSupervisorSha256': 'f2cfd5a41a7771719d9978170834892734f266d9f544689193bdad5112d2a5f6', 'generatorSha256': 'bcb6948d4fbbf73f954b6b4c237ab828654f61e7da378feb385f3e2141361384', 'generatedSources': {'normal_root_graphics_service.c': '35effb51cb6162577660f9e9b27e0c4a5149d2eb3459917316d4b4edbe749750', 'trace_port.h': '042528cd9f83e170d51124512b120bca5d2d5820353b41850cbd3465187df1c4', 'trace_session.h': 'c0baa2dbf1b623c6a5c33d7c7da71f47e9b1bd7c3c3e99eba1b2d948fdb37fb7'}, 'carrier': 'virtio-serial', 'port': 2, 'portName': 'org.7wdev.graphics.trace', 'wholeGraphicsSession': True, 'maximumTraceBytes': 67108864, 'orderedFifo': True, 'liveHostCapture': True, 'inputImplementationChanged': False, 'gpuProtocolChanged': False, 'performanceGainEstablished': False, 'nearNativePerformanceVerified': False}
DECODER='f13ba5eb77b828513b92ed248a7fd2a1bcd7822a2e1ee149f1b7f92582447c2e'

HARNESS='e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286'
GENERATOR='b3271daaffb03097d5998702b5670615ca6e8d975813cd15af84ab38a7f4ce38'
# Chosen from the fixture's resize phases before the candidate captures.
STEADY_WINDOWS=((20,45),(80,140),(200,280),(400,580),(1000,1180),(1500,1780))
ACTIVE_SOURCES={
    'active_probe.c':'8aefc9a0765f1c3cc852bf7d07c13fe1321dfcdf70cd9bf0fb7eb09c8d798341',
    'active_queue_probe.h':'cfca45615d3b9b95e736b8b4215696b4571b4f526e12becbddeddd652378e8ea',
    'gpu-terminal-scene.sh':'a49179c43d0ea1a6c0327daf9e126dc76ccb4be8e9fd31fae74a99325f9bc23e',
    'terminal_pulse.c':'bd6db5881a7b3f3dd23edcd219cd68c4d43885e8588c202ef9d62470386130ec',
}


from verify_pressure_frame_timing import active_timing
from verify_gpu_va_pressure import validate_pressure_transfers
from verify_managed_residency_state import validate_managed_state

def validate_pressure_mapping_desktop(report,guest,records,frames,source,wallpaper_reference,resize_reference,uart_raw,trace_raw=None,trace_observations=(),require_virtual_pressure=True):
    def rows(key):
        values=[row for row in records if key in row]
        require(all(row[key] is True for row in values),'Failed '+key)
        return values
    def one(key):
        values=rows(key);require(len(values)==1,'Missing/duplicate '+key);return values[0]
    manifest=source['manifest'];clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    mode=source['traceMode'];require(mode=='virtio','Sustained run requires the inspected live carrier')
    vendor=manifest.get('nativeDriverRuntimeSnapshot');GUEST=CURRENT_DRIVER_GUEST if vendor else GUESTS[mode]
    if vendor:
        expected_files={'libnvwgf2umx.so':dict(sha256='8c27070d8a1eeab35287d71f921e226664a33cefcd1bfe604f7fb4f188bc4f71',bytes=59850136),
            'libnvidia-gpucomp.so':dict(sha256='7fd26aafecebcfb134fc15d205bb3b6814118c7f07cdf8d723760dc4d8b9b8d0',bytes=126069920)}
        require(vendor['privateOnly'] is vendor['signedInstalledDriver'] is vendor['sourceDriverUnchanged'] is True and
                vendor['redistributionApproved'] is False and vendor['driverVersion']=='32.0.16.2101' and
                vendor['driverStorePackage']=='nvami.inf_amd64_00e5dc1fb00a1261' and vendor['files']==expected_files and
                vendor['collectorSha256']=='4556f5414dfc3926c5b63fe5c0a823f4d22a6703cf576feaa1106974bd5b4f0f',
                'Mixed or publishable vendor runtime snapshot')
        installed=dict(installedRuntimeMatchesSnapshot=True,driverVersion='32.0.16.2101',
            driverStorePackage='nvami.inf_amd64_00e5dc1fb00a1261',libraryCount=2)
        require(source['installedNvidiaRuntimeBefore']==source['installedNvidiaRuntimeAfter']==installed,
                'Installed driver changed during the comparison')
    require(source['traceProbeSourcesUnchanged'] is True and source['traceDecoderSha256']==DECODER and
            report['qemuSha256']==WINDOW_AUDIT_QEMU,'Mixed carrier decoder or QEMU build')
    configured=rows('nativeGraphicsTraceConfigured');trace_proof=None
    if mode=='virtio':
        require(manifest['virtioTraceProbe']==TRACE_PINS and len(configured)==1 and
                configured[0]['carrier']=='virtio-serial' and configured[0]['port']==2 and
                configured[0]['liveHostCapture'] is True and configured[0]['inputImplementationChanged'] is
                configured[0]['gpuProtocolChanged'] is False,'Wrong live trace carrier')
        merged,trace_proof=merge_session(uart_raw,trace_raw)
        require(guest==merged.replace('\r\n','\n').replace('\r','\n'),'Guest replay differs from complete live carrier')
        require([row['frame'] for row in trace_observations]==[1,100,600,1200,1800] and
                all(row['event']=='virtioTraceObserved' and 0<row['bytes']<=len(trace_raw) for row in trace_observations) and
                all(a['bytes']<b['bytes'] for a,b in zip(trace_observations,trace_observations[1:])),
                'Trace was not observed growing during the GPU stream')
    else:
        require(not configured and trace_raw is None and not trace_observations and
                guest==uart_raw.decode('utf-8',errors='replace').replace('\r\n','\n').replace('\r','\n'),
                'UART baseline carrier differs')

    timing=active_timing(guest,source)
    transport=rows('nativeGpuTransportConfigured')
    if report['driverBridgeSha256'] in BRIDGES:
        require(len(transport)==1 and transport[0]['tcpNoDelay'] is
                transport[0]['framedReplyScatterGather'] is True and
                transport[0]['inputPathChanged'] is False and
                1<=transport[0]['tcpNoDelayOptionBytes']<=4,'Missing GPU transport configuration')
    else:
        require(not transport,'Baseline unexpectedly contains candidate transport')
    require(report['driverBridgeSha256'] in BRIDGES and
            report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256'] and
            report['qemuSha256'] in QEMU_BUILDS and report['initramfsSha256']==manifest['initramfsSha256']==GUEST and
            source['ownedRuntimeHarnessSha256']==HARNESS,'Mixed compiled guest/host/runtime provenance')
    budget_proof=validate_managed_state(report,records,source,guest,require_virtual_pressure=require_virtual_pressure)
    require(require_virtual_pressure is True,"Pressure desktop may not skip the virtual boundary")
    pressure_proof=validate_pressure_transfers(guest,records,source)
    require(source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT,'Source changed')
    artifact=source['qemuSdlBuildReceipt']
    qemu=report['qemuSha256'];revision,archive=QEMU_BUILDS[qemu]
    require(artifact['revision']==revision and artifact['archiveSha256']==archive and
            artifact['files']['bin/qemu-system-x86_64.exe']==qemu and
            artifact['files']['bin/SDL2.dll']=='2e44d5bae9a97859eccde14ad4467df14bae7468594bba7fe8eb417f517ce31b',
            'Different QEMU/SDL build')
    if qemu==WINDOW_AUDIT_QEMU:
        provenance=source['qemuWindowAuditProvenance']
        require(source['qemuArtifactUnchanged'] is True and provenance['inputFiles']==OFFICIAL_INPUT and
                provenance['graphicsWindowAuditSchema']==1 and
                provenance['graphicsRelayPatchSha256']=='1c9bc3c162dcea37b6b58db944de1c290a01d4e566cb62b3618c27e9dde3cc3f' and
                provenance['graphicsRelaySources']['wddm-sdl-relay.c']=='b693e81e993c2ed9f98ef86937c3cb4e21923a9e13e17f1e9c92a61444436cd8',
                'Mixed graphics diagnostic/official input source')
    require(manifest['expectedFrames']==1800 and manifest['kernelCpuCapacityProfile']==13 and
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
    require(clean['completedCompositorFrames']==1800 and clean['acceptedAsyncSubmissions']==clean['completedNativeSubmissions']>0 and
            clean['acceptedGpuQueueWaits']==clean['satisfiedGpuQueueWaits']>0 and
            clean['reportedGpuUsageLimitBytes']==1073741824 and clean['peakReportedGpuUsageBytes']<=1073741824 and
            clean['vendorCpuMappedByteLimit']==201326592 and clean['peakVendorCpuBytes']<=201326592 and
            clean['standaloneGpuMappedByteLimit']==1073741824 and clean['peakStandaloneMappedPages']*4096<=1073741824 and
            clean['cpuAllocationFingerprintCalls']==clean['cpuAllocationFingerprintBytes']==0,'GPU work or budget guard differs')
    require(not any(token in guest for token in ('HYPRLAND_HOST_QUEUE_FAILURE','GL_OUT_OF_MEMORY','Quickshell has crashed',
            'qtQuickSoftware=true','llvmpipe','softpipe')) and
            'HYPRLAND_HOST_QUEUE_END_ACK frame=1800 outputDrained=true' in guest and
            'HYPRLAND_HOST_QUEUE_END frames=1800 target=1800 failed=0' in guest and
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
    require(len(inputs)==len(outputs)==len(queued)==len(replies)==1800,'Missing/duplicate frame handoff')
    for number,(ingress,egress,relay,reply) in enumerate(zip(inputs,outputs,queued,replies),1):
        require(ingress['frame']==egress['frame']==relay['frame']==int(reply[0])==number and
                int(reply[1])==int(number in (1,1800)) and ingress['sourceCopyRetired'] is ingress['sourceReturnedToCommon'] is True and
                ingress['inputCopyFenceObserved']>=number and ingress['producerFenceObserved']>=ingress['producerFenceTarget']>0 and
                ingress['cpuUpload'] is ingress['cpuReadback'] is relay['cpuUpload'] is relay['ordinaryCpuFenceWait'] is False and
                egress['cpuReadback'] is relay['audit'] is (number in (1,1800)) and relay['gpuReuseWaitQueued'] is (number>1),
                'Frame order, real fence retirement or GPU-only ordinary handoff differs')
        state=relay['qemuState']
        require(state['attached'] is state['rootAdapterVerified'] is state['independentFences'] is True and
                state['sourceWidth']==3840 and state['sourceHeight']==2160 and state['sourceFormat']=='bgra8' and
                state['lastQueuedFrame']==state['convertedFrames']==number and state['auditFrames']==1+int(number==1800) and
                state['liveImportedObjects']==4 and state['adapterLuid']==opened['adapterLuid'] and
                state['vendor']=='NVIDIA Corporation' and 'RTX 5090' in state['renderer'] and state['poisoned'] is False and
                all(state[key] is False for key in ('cpuTextureUpload','physicalInputVerified','physicalScanoutVerified',
                    'guestGpuScanoutVerified','nearNativePerformanceVerified')),'Wrong/overclaimed SDL GPU state')
    audits=rows('nativeCompositorRingAudited');captures=rows('nativeCompositorBinaryCapture');proof=[]
    require(len(audits)==len(captures)==2,'Wrong complete readback count')
    for index,number in enumerate((1,1800),1):
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
        if qemu==WINDOW_AUDIT_QEMU:
            info=state['windowAudit'];width=state['windowWidth'];height=state['windowHeight']
            require(info['schema']==1 and info['frame']==number and info['glError']==0 and
                    info['logicalWidth']==info['drawableWidth']==info['captureWidth']==width and
                    info['logicalHeight']==info['drawableHeight']==info['captureHeight']==height and
                    info['capturePixels']==width*height and info['shown'] is info['doubleBuffered'] is True and
                    info['hidden'] is info['minimized'] is info['allBytesZero'] is
                    info['visibilityProvesPixelOwnership'] is info['physicalScanoutVerified'] is False and
                    info['opaqueAlphaPixels']==window[3::4].count(255)==width*height and
                    info['zeroAlphaPixels']==window[3::4].count(0)==0 and info['colorWriteMask']==15 and
                    info['viewport']==dict(x=0,y=0,width=width,height=height),'Invalid drawable/window capture diagnostics')
            audit['windowAudit']=info
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
    require(end['frames']==end['producerFenceObserved']==end['consumerFenceObserved']==retired['frame']==1800 and end['audits']==2 and
            retired['beforeFinalGuestReply'] is retired['beforeOwnedVmStop'] is True and end_state['liveImportedObjects']==0 and
            all(end_state[key] is False for key in ('attached','channelProgramLive','importContextLive','localScanoutLive','poisoned')),
            'Final cross-process GPU references did not retire live')
    require(records.index(end)<records.index(one('nativeCompositorRingStopped'))<records.index(one('nativeCoordinatorTeardown')),
            'Output detach follows VM/global cleanup')
    require(one('nativeInputChannelStopped')['acknowledgements']==33,'Incomplete isolated QMP input control')
    return dict(realGuestNvidiaToSdlVerified=True,traceMode=mode,traceIntegrity=trace_proof,activeTiming=timing,sharedBudget=budget_proof,pressureDiagnostic=pressure_proof,frames=proof,frameCount=1800,ordinaryGpuOnlyFrames=1798,
                session=session,inputDiscovery=devices,lifecycles=lifecycles,resize=resizing,resizePixels=resize_proof,
                qmpInputResponse=response,terminal=terminal,liveSdlDetachVerified=True,
                peakReportedGpuUsageBytes=clean['peakReportedGpuUsageBytes'],
                normalLauncherVerified=False,fullDesktopAccepted=False,physical4kScanoutVerified=False,
                allFramesDisplayedVerified=False,physicalInputAutomatedVerified=False,nearNativePerformanceVerified=False)
