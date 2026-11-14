"""Audit an explicit 1,800-frame GPU stream; historical shorter-stream pins stay frozen."""
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

BRIDGES={'4855a66ba4f331f5637b7ddd73d65b72861622884404646e24f1ee3748b961b5':'77','6c7c8d98046aa2e477adfc08cc2b0f22a264dd3fa6b487a24f712f2c4bb467a5':'78'}
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
CURRENT_DRIVER_GUEST='a90cb16f90fd55772a60f02a1e4148cfc7410698914336b4d4e18de361074b26'
TRACE_PINS={'schema': 1, 'baselineSupervisorSha256': 'f2cfd5a41a7771719d9978170834892734f266d9f544689193bdad5112d2a5f6', 'generatorSha256': 'bcb6948d4fbbf73f954b6b4c237ab828654f61e7da378feb385f3e2141361384', 'generatedSources': {'normal_root_graphics_service.c': '35effb51cb6162577660f9e9b27e0c4a5149d2eb3459917316d4b4edbe749750', 'trace_port.h': '042528cd9f83e170d51124512b120bca5d2d5820353b41850cbd3465187df1c4', 'trace_session.h': 'c0baa2dbf1b623c6a5c33d7c7da71f47e9b1bd7c3c3e99eba1b2d948fdb37fb7'}, 'carrier': 'virtio-serial', 'port': 2, 'portName': 'org.7wdev.graphics.trace', 'wholeGraphicsSession': True, 'maximumTraceBytes': 67108864, 'orderedFifo': True, 'liveHostCapture': True, 'inputImplementationChanged': False, 'gpuProtocolChanged': False, 'performanceGainEstablished': False, 'nearNativePerformanceVerified': False}
DECODER='f13ba5eb77b828513b92ed248a7fd2a1bcd7822a2e1ee149f1b7f92582447c2e'

HARNESS='e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286'
GENERATOR='93e7638d9972be4f31808941351415dad6759182c1f5f4686672263fccf0cf55'
# Chosen from the fixture's resize phases before the candidate captures.
STEADY_WINDOWS=((20,45),(80,140),(200,280),(400,580),(1000,1180),(1500,1780))
ACTIVE_SOURCES={
    'active_probe.c':'8aefc9a0765f1c3cc852bf7d07c13fe1321dfcdf70cd9bf0fb7eb09c8d798341',
    'active_queue_probe.h':'77c25a22bbf3fff840a7a698c10887a8b72259f6bdc52ed6be6005ab7727f38a',
    'gpu-terminal-scene.sh':'a49179c43d0ea1a6c0327daf9e126dc76ccb4be8e9fd31fae74a99325f9bc23e',
    'terminal_pulse.c':'bd6db5881a7b3f3dd23edcd219cd68c4d43885e8588c202ef9d62470386130ec',
}


def validate_sustained_memory_failure(report,records,source,trace_raw):
    """Certify the failed baseline's cause/cleanup, never desktop acceptance."""
    require(report['driverBridgeSha256']=='4855a66ba4f331f5637b7ddd73d65b72861622884404646e24f1ee3748b961b5' and
            report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256'] and
            report['initramfsSha256']==GUESTS['virtio'] and source['manifest']['expectedFrames']==1800 and
            source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT,'Mixed failed-baseline provenance')
    failures=[row for row in records if row.get('nativeCoordinatorFailure')]
    require(len(failures)==1 and failures[0]['message']=='Reported GPU usage limit after residency' and
            failures[0]['beforeGlobalTeardown'] is failures[0]['beforeOwnedVmStop'] is True,'Original residency failure differs')
    clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(report['hostBridgeExit']==1 and clean['driverCleanupVerified'] is True and
            clean['completedCompositorFrames']==527 and clean['failedVendorResidencyRequests']==1 and
            clean['reportedGpuUsageLimitBytes']==1073741824<clean['peakReportedGpuUsageBytes'] and
            vm['ownedQemuExited'] is vm['qemuForcedStop'] is True and vm['qemuExit']==90 and
            all(v==0 for k,v in vm.items() if k.startswith('live')) and
            all(v==0 for k,v in clean.items() if k.startswith(('live','pending'))),'Failure did not preserve ownership and reap the VM')
    budgets=[row for row in records if row.get('nativeMemoryBudgetQueried')]
    require(len(budgets)>0 and {row['group'] for row in budgets}=={0,1} and
            all(row['budget']==1073741824 for row in budgets),'Missing duplicated segment-budget evidence')
    ended=[row for row in records if row.get('nativeSdlCompositorDetached')]
    require(len(ended)==1 and ended[0]['frames']==ended[0]['producerFenceObserved']==ended[0]['consumerFenceObserved']==527 and
            ended[0]['qemuState']['liveImportedObjects']==0 and ended[0]['qemuState']['attached'] is False,
            'Accepted SDL frames did not drain on the failure')
    require(trace_raw.startswith(HEADER),'Failed trace is not the original bounded carrier')
    try:decode_trace(trace_raw)
    except ValueError as error:
        require(str(error)=='Incomplete virtio trace','Failed trace rejection differs')
    else:raise ValueError('Interrupted baseline unexpectedly has a complete trace')
    return dict(accepted=False,frames=527,originalHostFailure=failures[0]['message'],
        segmentBudgetsEachBytes=1073741824,advertisedTotalBudgetBytes=2147483648,enforcedAggregateBytes=1073741824,
        peakReportedGpuUsageBytes=clean['peakReportedGpuUsageBytes'],traceComplete=False,
        acceptedFramesDrained=True,ownedVmReaped=True,nativeCleanupVerified=True,
        naturalShutdownVerified=False,fullDesktopAccepted=False,nearNativePerformanceVerified=False)


def validate_rejected_sustained_candidate(report,guest,records,source,uart_raw,trace_raw,kind):
    """Record startup/eviction failures without treating clean teardown as success."""
    require(kind in ('old-vendor-runtime','mapped-eviction'),'Unknown rejected control')
    require(report['driverBridgeSha256']=='6c7c8d98046aa2e477adfc08cc2b0f22a264dd3fa6b487a24f712f2c4bb467a5' and
            report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256'] and
            source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT,'Mixed rejected candidate')
    merged,trace=merge_session(uart_raw,trace_raw)
    require(guest==merged.replace('\r\n','\n').replace('\r','\n'),'Rejected candidate trace differs')
    clean=report['disconnectCleanup'];vm=report['ownedQemuControl'];count=0 if kind=='old-vendor-runtime' else 2
    require(report['hostBridgeExit']==0 and vm['qemuExit']==0 and vm['qemuForcedStop'] is False and
            clean['driverCleanupVerified'] is True and clean['completedCompositorFrames']==count and
            all(v==0 for k,v in vm.items() if k.startswith('live')) and
            all(v==0 for k,v in clean.items() if k.startswith(('live','pending'))) and
            'NORMAL_ROOT_GRAPHICS_EXIT result=125 normalOmarchySession=false' in guest,'Rejected control cleanup differs')
    if kind=='old-vendor-runtime':
        require(report['initramfsSha256']==GUESTS['virtio'] and clean['completedNativeSubmissions']==0 and
                clean['completedMemoryBudgetQueries']==0 and 'CBackend::create() failed!' in guest,
                'Startup control reached the budget policy')
    else:
        require(report['initramfsSha256']==CURRENT_DRIVER_GUEST and
                'evictionInput count=3 flags=1' in guest and 'evictionAllocation index=2 cpuLease=1' in guest and
                'ioctlFailed nr=30 errno=16' in guest and 'Assertion failed!' in guest and
                clean['attemptedVendorEvictions']==0 and clean['peakReportedGpuUsageBytes']<=1073741824,
                'Missing mapped-lease eviction rejection and compositor assertion')
        budgets=[row for row in records if row.get('nativeMemoryBudgetQueried')]
        require(bool(budgets) and all(row['sharedAggregateBudget'] is True and
                row['localBudgetShareBytes']+row['nonlocalBudgetShareBytes']==1006632960 for row in budgets),
                'Rejected candidate did not select the shared budget')
    return dict(accepted=False,kind=kind,frames=count,completeTrace=trace['complete'],nativeCleanupVerified=True,
        naturalShutdownVerified=True,rootCauseVerified=False,fullDesktopAccepted=False,nearNativePerformanceVerified=False)


def active_timing(guest,source):
    profile=source['manifest']['activeFrameProbe']
    require(profile['baselineGeneratorSha256']=='fafeb1f0324e21e2824b085d676c4ef48fe8f2c383d06b65b5b6fc2d4762252f' and
            profile['compiledFrameDefine']=='HYPRLAND_QUEUE_FRAMES=1800' and profile['renderingCodeChanged'] is False and
            profile['generatorSha256']==GENERATOR and profile['generatedSources']==ACTIVE_SOURCES and
            profile['pulseBinarySha256']=='73d0275e33d8a0241e00790dbd8da3adbfad1a22452e76f3490346a662a4bf09' and
            profile['targetUpdateHz']==60 and profile['notificationRepaints'] is False and
            profile['controllerObserverSleepUs']==2000 and profile['workloadDeadlineSeconds']==90 and
            profile['frameCount']==1800 and profile['baselineFixtureUnchanged'] is True and
            profile['cpuWallClockTimings'] is True and source['activeFrameProbeSourcesUnchanged'] is True and
            profile['inputImplementationChanged'] is profile['gpuTimestamps'] is
            profile['nearNativePerformanceVerified'] is profile['benchmarkAccepted'] is False,
            'Mixed/overclaimed active workload')
    pattern=(r'^HYPRLAND_ACTIVE_QUEUE_TIMING frame=(\d+) audit=([01]) fenceSetupNs=(\d+) '
             r'queueRoundTripNs=(\d+) totalNs=(\d+) queueAttempts=(\d+) monotonicNs=(\d+) '
             r'cpuWallClock=true gpuTimestamp=false$')
    rows=[dict(zip(('frame','audit','fenceSetupNs','queueRoundTripNs','totalNs','queueAttempts','monotonicNs'),map(int,row)))
          for row in re.findall(pattern,guest,re.M)]
    require(len(rows)==guest.count('HYPRLAND_ACTIVE_QUEUE_TIMING ')==1800,'Missing/invalid active timing')
    previous=0
    for index,row in enumerate(rows,1):
        require(row['frame']==index and row['audit']==int(index in (1,1800)) and row['queueAttempts']>=1 and
                row['fenceSetupNs']>0 and row['queueRoundTripNs']>0 and
                row['totalNs']>=row['fenceSetupNs']+row['queueRoundTripNs'] and
                row['monotonicNs']>previous,'Invalid CPU timing or frame order')
        previous=row['monotonicNs']
    duration=(rows[-1]['monotonicNs']-rows[0]['monotonicNs'])/1e9
    require(25<=duration<=90,'Stream did not sustain the preselected 25-second minimum within its deadline')
    steady=[row for row in rows if any(lo<=row['frame']<=hi for lo,hi in STEADY_WINDOWS)]
    deltas=[(rows[i]['monotonicNs']-rows[i-1]['monotonicNs'])/1e6 for i in range(1,len(rows))
            if any(lo<rows[i]['frame']<=hi for lo,hi in STEADY_WINDOWS)]
    return dict(rows=1800,streamDurationSeconds=duration,minimumSustainedSeconds=25,
                steadyRows=len(steady),steadyWindows=[list(w) for w in STEADY_WINDOWS],
                mediansMs={key:statistics.median(row[key]/1e6 for row in steady)
                           for key in ('fenceSetupNs','queueRoundTripNs','totalNs')},
                frameIntervalMedianMs=statistics.median(deltas),
                frameIntervalP95Ms=statistics.quantiles(deltas,n=100)[94],
                queueRetries=sum(row['queueAttempts']-1 for row in rows),
                cpuWallClockOnly=True,gpuTimestamps=False,physicalInputLatencyVerified=False,
                physicalScanoutCadenceVerified=False,nearNativePerformanceVerified=False)


def validate_sustained_trace_desktop(report,guest,records,frames,source,wallpaper_reference,resize_reference,uart_raw,trace_raw=None,trace_observations=()):
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
    budget_proof=None
    if BRIDGES[report['driverBridgeSha256']]=='78':
        budget_rows=rows('nativeMemoryBudgetQueried')
        require(bool(budget_rows) and {row['group'] for row in budget_rows}=={0,1},'Missing paired memory segment reports')
        first=budget_rows[0];local=first['localCapacityWeightBytes'];nonlocal_capacity=first['nonlocalCapacityWeightBytes']
        limit=1073741824;reserve=67108864;total=local+nonlocal_capacity
        require(local>0 and nonlocal_capacity>0 and total<=2**64-1,'Invalid static adapter capacity weights')
        usable=min(total,limit-reserve);shares=(local*usable//total,usable-local*usable//total)
        for row in budget_rows:
            require(row['sharedAggregateBudget'] is row['currentProcessOnly'] is True and row['physicalAdapter']==0 and
                    row['aggregateLimitBytes']==limit and row['presentationReserveBytes']==reserve and
                    row['localCapacityWeightBytes']==local and row['nonlocalCapacityWeightBytes']==nonlocal_capacity and
                    (row['localBudgetShareBytes'],row['nonlocalBudgetShareBytes'])==shares and
                    row['budget']==min(row['nativeBudget'],shares[row['group']]) and 0<=row['available']<=row['budget'] and
                    row['usage']>=0,'Memory segment replies duplicate the aggregate budget or fabricate usage')
        build=source['nativeBuildReceipt']
        require(build['sharedAggregateMemoryBudget'] is build['stableHardwareCapacityWeights'] is True and
                build['presentationReserveBytes']==reserve and build['memoryGuardRaised'] is False,'Budget policy differs from compiled receipt')
        budget_proof=dict(segmentQueries=len(budget_rows),aggregateLimitBytes=limit,presentationReserveBytes=reserve,
            advertisedTotalBudgetBytes=usable,localShareBytes=shares[0],nonlocalShareBytes=shares[1],
            usageUnclamped=True,stableHardwareCapacityWeights=True,memoryGuardRaised=False)
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
            clean['vendorGpuMappedByteLimit']==1073741824 and clean['peakVendorMappedPages']*4096<=1073741824 and
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
    return dict(realGuestNvidiaToSdlVerified=True,traceMode=mode,traceIntegrity=trace_proof,activeTiming=timing,sharedBudget=budget_proof,frames=proof,frameCount=1800,ordinaryGpuOnlyFrames=1798,
                session=session,inputDiscovery=devices,lifecycles=lifecycles,resize=resizing,resizePixels=resize_proof,
                qmpInputResponse=response,terminal=terminal,liveSdlDetachVerified=True,
                peakReportedGpuUsageBytes=clean['peakReportedGpuUsageBytes'],
                normalLauncherVerified=False,fullDesktopAccepted=False,physical4kScanoutVerified=False,
                allFramesDisplayedVerified=False,physicalInputAutomatedVerified=False,nearNativePerformanceVerified=False)
