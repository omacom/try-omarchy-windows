"""Audit bounded real D3D12 copy/evict/reuse before a graphical frame stream.

This is an additional SDK buffer diagnostic, not shader, API-completeness,
physical scanout, or matched-native performance acceptance.
"""
import re
from verify_hyprland_lifecycle_capture import require
from virtio_trace import merge_session

HEAPS=28
HEAP_BYTES=50331648
BLOCK_BYTES=4096
TRANSFER_BYTES=3*BLOCK_BYTES


def pattern_hash(pass_number,heap):
    """Independent Python oracle; never import the guest's C++ implementation."""
    value=0xcbf29ce484222325
    for block in range(3):
        for position in range(BLOCK_BYTES):
            byte=(73*heap+151*pass_number+29*block+((17*position)^(position//16)))%256
            value=((value^byte)*0x100000001b3)&0xffffffffffffffff
    return value


def validate_pressure_transfers(guest,records,source):
    manifest=source['manifest']
    require(source['preStreamPressureSourcesUnchanged'] is True and
            manifest['preStreamD3D12Pressure']==dict(heaps=28,heapBytes=HEAP_BYTES,passes=2,blocks=3,blockBytes=BLOCK_BYTES,
                declaredVirtualBytes=HEAPS*HEAP_BYTES,ordinaryFrames=1800,inputChanged=False,shaderRenderingChanged=False,
                residentMemoryGuardRaised=False,baselineGuestSha256='3a8ded9a0ea28aa6252e316d91dca7ebbe3fc8ad152d12b8e01f62a566d937a5'),
            'Mixed pressure fixture or relaxed memory/input guard')
    require(not any(token in guest for token in ('D3D12_VA_PRESSURE_FAILURE','HYPRLAND_VA_PRESSURE_LOAD_FAILURE')),
            'Pressure diagnostic failed')
    device=re.findall(r'^D3D12_VA_PRESSURE_DEVICE queryResult=0 mesaVendor=(\d+) driverBytes=24 nativeDevice=true nativeQueue=true adapterLuid=([0-9a-f]{16})$',guest,re.M)
    channel=[r for r in records if r.get('nativeSdlGraphicsChannelOpened')]
    # linux_ioctl_bridge.cpp exposes GuestLuidLow=0x57475055. It is a virtual
    # guest identity, not the Windows adapter LUID. The encompassing desktop
    # auditor separately proves the exact host bridge, driver and root adapter.
    require(len(device)==len(channel)==1 and device[0][1]=='0000000057475055' and
            re.fullmatch('[0-9a-f]{16}',channel[0]['adapterLuid']) is not None and
            channel[0]['socketPeerVerified'] is True,
            'Pressure probe virtual adapter or authenticated host channel differs')
    begin='D3D12_VA_PRESSURE_BEGIN heaps=28 heapBytes=50331648 passes=2 totalVirtualBytes=1409286144 preStream=true nvidia=true ordinaryFrameCodeChanged=false'
    require(guest.splitlines().count(begin)==1,'Missing/duplicate pressure start')
    allocations=re.findall(r'^D3D12_VA_PRESSURE_ALLOCATION index=(\d+) gpuVa=(\d+) bytes=50331648 createNotResident=true cpuMapped=false$',guest,re.M)
    require(len(allocations)==guest.count('D3D12_VA_PRESSURE_ALLOCATION ')==HEAPS,'Incomplete pressure allocations')
    addresses=[]
    for expected,(index,address) in enumerate(allocations):
        address=int(address)
        require(int(index)==expected and 0<address<=(1<<48)-HEAP_BYTES and address%65536==0 and
                all(not (address<other+HEAP_BYTES and other<address+HEAP_BYTES) for other in addresses),
                'Pressure allocations overlap or have invalid addresses/order')
        addresses.append(address)
    cycles=re.findall(r'^D3D12_VA_PRESSURE_CYCLE pass=(\d+) index=(\d+) target=(\d+) gpuVa=(\d+) fnv1a=([0-9a-f]{16}) priorFnv1a=([0-9a-f]{16}) comparedBytes=(\d+) priorComparedBytes=(\d+) gpuRetired=true evictAccepted=true cpuFenceWrite=false$',guest,re.M)
    require(len(cycles)==guest.count('D3D12_VA_PRESSURE_CYCLE ')==2*HEAPS,'Incomplete/invalid retired pressure cycles')
    for ordinal,cycle in enumerate(cycles):
        pass_number,index=divmod(ordinal,HEAPS)
        p,h,target,address,actual,prior,compared,prior_compared=cycle
        require((int(p),int(h),int(target),int(address))==(pass_number,index,ordinal+1,addresses[index]) and
                int(actual,16)==pattern_hash(pass_number,index) and
                int(prior,16)==(pattern_hash(pass_number-1,index) if pass_number else 0) and
                int(compared)==TRANSFER_BYTES and int(prior_compared)==(TRANSFER_BYTES if pass_number else 0),
                'Wrong GPU copy contents, evicted contents, stable address, or fence sequence')
    complete='D3D12_VA_PRESSURE_COMPLETE allocations=28 cycles=56 gpuVirtualBytes=1409286144 diagnosticUploadBytes=688128 diagnosticReadbackBytes=1032192 nativeObjectsReleased=true ordinaryFramesStarted=false'
    retired='HYPRLAND_VA_PRESSURE_RETIRED beforeFirstQueue=true nativeObjectsReleased=true frameLoopUnchanged=true'
    require(guest.splitlines().count(complete)==guest.splitlines().count(retired)==1 and
            guest.index(begin)<guest.index('D3D12_VA_PRESSURE_ALLOCATION index=0 ')<
            guest.index('D3D12_VA_PRESSURE_CYCLE pass=0 index=0 ')<guest.index(complete)<guest.index(retired)<
            guest.index('HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=1 '),
            'Diagnostic did not release its own native objects before the desktop stream')
    first=[i for i,row in enumerate(records) if row.get('nativeCompositorRingInput') and row.get('frame')==1]
    require(len(first)==1,'Missing first real compositor input')
    before=records[:first[0]]
    mapping=[r for r in before if r.get('nativeGpuMappingAccounted')]
    require(bool(mapping) and max(r['totalPages'] for r in mapping)>262144 and
            all(r['ownedReservationChecked'] is True and r['residentUsageGuardBytes']==1073741824 and
                r['standalonePages']<=r['standaloneLimitPages']==262144 and
                r['totalPages']-r['standalonePages']<=r['reservationLimitPages']==16777216 for r in mapping),
            'Pressure did not exercise extended owned GPU mappings before frame one')
    require(sum(bool(r.get('nativeAsyncSubmissionRetired')) for r in before)>=56 and
            not any(r.get('cpuValueWrittenByBridge') is True for r in before),
            'Missing real native retirement or fabricated CPU completion')
    return dict(accepted=True,allocations=HEAPS,declaredVirtualBytes=HEAPS*HEAP_BYTES,copyEvictCycles=56,
        independentlyCheckedNewBuffers=56,independentlyCheckedEvictedBuffers=28,
        diagnosticUploadBytes=688128,diagnosticReadbackBytes=1032192,
        peakVirtualBytesBeforeFrameOne=max(r['totalPages'] for r in mapping)*4096,
        nativeObjectsReleasedBeforeStream=True,ordinaryFrameCodeChanged=False,inputChanged=False,
        shaderCoverageExtended=False,performanceAccepted=False)


def validate_rejected_pressure(report,guest,records,source,uart_raw,trace_raw):
    """Preserve the failed Mesa vendor-field control without desktop acceptance."""
    require(report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256']==
            '260def3126c56c8dbfcf8a4be400788c2300fb163a5115af47fbfc95c5544e1d' and
            report['initramfsSha256']==source['manifest']['initramfsSha256']==
            '2de1f22132a83551ccf09cba3e431a8bc3426f62b9f58e0a456221e9fe97a838',
            'Mixed rejected pressure build')
    merged,trace=merge_session(uart_raw,trace_raw)
    require(guest==merged.replace('\r\n','\n').replace('\r','\n') and trace['complete'] is True,
            'Rejected pressure trace differs')
    failure='D3D12_VA_PRESSURE_FAILURE stage=MesaNativeDevice hresult=80004005 retainUntilOwnedVmExit=true'
    clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(guest.splitlines().count(failure)==1 and 'D3D12_VA_PRESSURE_BEGIN ' not in guest and
            not any(r.get('nativeCompositorRingInput') for r in records) and
            report['hostBridgeExit']==vm['qemuExit']==clean['completedCompositorFrames']==0 and
            vm['ownedQemuExited'] is clean['driverCleanupVerified'] is True and
            vm['qemuForcedStop'] is vm['fenceControlFailed'] is False and
            all(v==0 for k,v in clean.items() if k.startswith(('live','pending','forced')) or k.endswith('Timeouts')) and
            all(v==0 for k,v in vm.items() if k.startswith('live') or k.endswith('ReleasedAfterVmExit')) and
            source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is
            source['preStreamPressureSourcesUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==
            '3954123192111ce2d28cafb9dd108e1ca14c288bfc2429ed899b0f36aeccf942',
            'Rejected diagnostic failure or native/VM ownership differs')
    return dict(accepted=False,frames=0,failureStage='MesaNativeDevice',pressureStarted=False,
        traceComplete=True,nativeCleanupVerified=True,ownedVmReaped=True,sourceImageUnchanged=True,
        fullDesktopAccepted=False,nearNativePerformanceVerified=False)
