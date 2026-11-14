"""Audit native NVIDIA NT-reference lifetime only; no guest/client acceptance."""
import re
from verify_hyprland_lifecycle_capture import require


def validate(resource,sync):
    resource=resource.replace('\r\n','\n');sync=sync.replace('\r\n','\n')
    pattern=r'^NATIVE_RESOURCE_ALIAS vendor=(\d+) device=(\d+) width=130 height=73 pixels=9490 originalResourceReleasedBeforeImport=true originalNtClosedBeforeImport=true aliasNtClosedBeforeCopy=true gpuClear=true gpuCopyRetired=true cpuUpload=false inheritable=false guest=false desktop=false$'
    found=re.findall(pattern,resource,re.M)
    require(len(found)==1 and resource.count('NATIVE_RESOURCE_ALIAS vendor=')==1,'Missing/failed native resource lifetime')
    vendor,device=map(int,found[0]);require(vendor==0x10de and device>0,'Software/foreign native renderer')
    complete='NATIVE_RESOURCE_ALIAS_COMPLETE ownedReferencesReleased=true cleanup=true guest=false desktop=false\n'
    require(resource.endswith(complete) and resource.count(complete)==1 and 'FAILURE' not in resource,'Resource cleanup failed')
    begin=f'NATIVE_SYNC_BEGIN vendor={vendor} device={device} cpuWritesFence=false guest=false desktop=false\n'
    alias='NATIVE_SYNC_ALIAS independentNtReference=true originalNtClosedBeforeImport=true inheritable=false\n'
    released='NATIVE_SYNC_SOURCE_RELEASED originalDestroyed=true ntHandleClosed=true aliasesLive=true\n'
    end='NATIVE_SYNC_COMPLETE gpuSignals=16 originalReleasedAfter=8 ntClosedBeforeLast=8 aliasesReleased=true cleanup=true guest=false desktop=false\n'
    require(all(sync.count(line)==1 for line in (begin,alias,released,end)) and sync.endswith(end) and 'FAILURE' not in sync,
        'Missing/failed fence alias lifetime')
    statuses=re.findall(r'^NATIVE_SYNC_(?:NT|HR|CLEANUP) .*status=([0-9a-f]{8})$',sync,re.M)
    require(statuses and all(value=='00000000' for value in statuses),'Failed native API/cleanup status')
    stages={'NT':{'OpenAdapter':1,'CreateDevice':2,'CreateSource':1,'ExportNt':1,'DuplicateNt':1,'ImportKmt':1,'DestroySource':1,'ReexportRecipient':1},
        'HR':{'Factory':1,'D3D12Device':1,'ImportD3D12':1,'Queue':1,'QueueSignal':8,'QueueSignalAfterClose':8,'ReimportD3D12':1}}
    for family,values in stages.items():
        for stage,count in values.items():
            require(sync.count(f'NATIVE_SYNC_{family} stage={stage} status=00000000\n')==count,'Missing/duplicate native stage: '+stage)
    require(sync.index(alias)<sync.index('NATIVE_SYNC_NT stage=ImportKmt status=00000000\n')<
        sync.index('NATIVE_SYNC_HR stage=ImportD3D12 status=00000000\n'),'Imported before independent NT reference')
    require(sync.index('NATIVE_SYNC_NT stage=DestroySource status=00000000\n')<sync.index(released),
        'Source-release marker precedes native destruction')
    retired=re.findall(r'^NATIVE_SYNC_GPU_RETIRED target=(\d+) source=(\d+) recipient=(\d+) d3d12=(\d+) sourceAlive=([01])$',sync,re.M)
    require(len(retired)==16 and sync.count('NATIVE_SYNC_GPU_RETIRED ')==16,'Missing/duplicate GPU fence retirement')
    for n,row in enumerate(retired,1):
        target,source,recipient,d3d12,alive=map(int,row)
        require(target==n and all(0<v<(1<<64)-1 for v in (source,recipient,d3d12)) and recipient>=n and d3d12>=n and
            (alive==1 and source>=n if n<=8 else alive==0 and source==n),'Unretired/mixed GPU signal or invalid dead-source access')
        position=sync.index('NATIVE_SYNC_GPU_RETIRED target='+str(n)+' ')
        require(position<sync.index(released) if n<=8 else position>sync.index(released),'Source release order mismatch')
    require(sync.count('NATIVE_SYNC_CLEANUP kind=fence status=00000000\n')==1 and
        sync.count('NATIVE_SYNC_CLEANUP kind=device status=00000000\n')==2 and
        sync.count('NATIVE_SYNC_CLEANUP kind=adapter status=00000000\n')==1,'Incomplete native fence/device release')
    return dict(schema=1,nativeResourceAliasLifetimeVerified=True,nativeFenceAliasLifetimeVerified=True,
        vendor=vendor,device=device,pixelsVerified=9490,gpuSignalsVerified=16,sourceFenceNotReadAfterRelease=True,
        cpuUploadUsed=False,inheritableHandlesUsed=False,guestSharedReferenceAliasVerified=False,
        secondGuestGpuClientVerified=False,quickshellGpuVerified=False,desktopVerified=False,nearNativePerformanceVerified=False)
