"""Audit residency ownership independently of rendered-desktop acceptance."""
from verify_hyprland_lifecycle_capture import require
from verify_sustained_virtio_desktop import ROOT,CURRENT_DRIVER_GUEST,HARNESS,WINDOW_AUDIT_QEMU
from virtio_trace import merge_session

BRIDGES={
    '8cd595a47db709102b6d8ac39fbbc216e955f91a883ecc1f7c6e326da202789c':'80',
    'b0abb37c2181a0376d6131f0cd150a1520c40757b592bc432bea3d8c38265f28':'81',
}

def validate_ownership(report,records):
    """Replay every transfer and native retirement in the instrumented host."""
    clean=report['disconnectCleanup'];held=peak=deferred=reused=reclaimed=destroyed=0
    requests=native_calls=0
    for row in records:
        if row.get('nativeGuestEvictionDeferred') is True:
            count=row['count']
            require(0<count<=64 and row['flags'] in (0,1,2,3) and
                    row['nativeCall'] is False and row['cpuLeasePreserved'] is True,
                    'Logical eviction dropped native ownership')
            held+=count;deferred+=count;requests+=1;peak=max(peak,held)
            require(row['retainedReferences']==held,'Deferred reference total differs')
        elif row.get('nativeDeferredResidencyAdmitted') is True:
            count=row['requested'];reuse=row['reusedCount'];native=row['nativeCount']
            require(0<count<=64 and 0<=reuse<=held and native>=0 and count==reuse+native,
                    'Residency admission created or lost a reference')
            held-=reuse;reused+=reuse
            require(row['retainedReferences']==held,'Reused reference total differs')
        elif row.get('nativeDeferredResidencyReclaimed') is True:
            count=row['count']
            require(0<count<=64 and count<=held and row['ntstatus']==0 and
                    row['cpuMapped'] is False and row['allGpuWorkRetired'] is row['nativeCall'] is True and
                    row['retainedBefore']==held and row['retainedAfter']==held-count,
                    'Native eviction lacks unmapped, retired reference ownership')
            held-=count;reclaimed+=count;native_calls+=1
        elif row.get('nativeDeferredResidencyDestroyed') is True:
            count=row['references']
            require(0<count<=held and row['synchronousNativeDestroy'] is True and
                    row['retainedBefore']==held and row['retainedAfter']==held-count,
                    'Native allocation destruction lost retained ownership')
            held-=count;destroyed+=count
    require(requests>0 and reused>0 and reclaimed>0 and held==0 and deferred==reused+reclaimed+destroyed,
            'Missing real eviction/reuse/reclamation or retained reference leak')
    require(clean['deferredResidencyOptIn'] is True and
            clean['deferredEvictionRequests']==requests and clean['reusedResidencyReferences']==reused and
            clean['reclaimedResidencyReferences']==reclaimed and clean['destroyedRetainedResidencyReferences']==destroyed and
            clean['peakRetainedResidencyReferences']==peak and clean['liveRetainedResidencyReferences']==0 and
            clean['attemptedVendorEvictions']==clean['completedVendorEvictions']==native_calls and
            clean['failedVendorEvictions']==0,'Residency summary differs from complete event replay')
    require(not any(row.get('nativeVendorEviction') or row.get('nativeDeferredResidencyAdmissionRejected')
                    for row in records),'Unexpected immediate eviction or uncertain native admission')
    return dict(exactLedgerReplayed=True,deferredReferences=deferred,reusedReferences=reused,
                reclaimedReferences=reclaimed,destroyedReferences=destroyed,nativeEvictionBatches=native_calls,
                peakRetainedReferences=peak,liveRetainedReferences=held,
                unmappedRetiredReclamationReported=True,fullDesktopAccepted=False)

def validate_rejected_attempt(report,guest,records,source,uart,trace):
    """A clean teardown and safe residency path cannot rescue a crashed desktop."""
    bridge=report['driverBridgeSha256'];clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(bridge in BRIDGES and bridge==source['nativeBuildReceipt']['bridgeSha256'] and
            report['qemuSha256']==WINDOW_AUDIT_QEMU and
            report['initramfsSha256']==source['manifest']['initramfsSha256']==CURRENT_DRIVER_GUEST and
            source['ownedRuntimeHarnessSha256']==HARNESS and source['deferredResidencyOptIn'] is True and
            source['memoryGuardRaised'] is False,'Mixed deferred residency attempt provenance')
    require(source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is source['qemuArtifactUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT,'Changed owned test inputs')
    installed=dict(installedRuntimeMatchesSnapshot=True,driverVersion='32.0.16.2101',
        driverStorePackage='nvami.inf_amd64_00e5dc1fb00a1261',libraryCount=2)
    require(source['installedNvidiaRuntimeBefore']==source['installedNvidiaRuntimeAfter']==installed,
            'Installed driver changed')
    merged,trace_proof=merge_session(uart,trace)
    require(guest==merged.replace('\r\n','\n').replace('\r','\n'),'Rejected attempt trace replay differs')
    count=clean['completedCompositorFrames']
    require(report['hostBridgeExit']==0 and vm['qemuExit']==0 and vm['qemuForcedStop'] is False and
            clean['driverCleanupVerified'] is True and 0<count<1800 and
            all(v==0 for k,v in vm.items() if k.startswith('live')) and
            all(v==0 for k,v in clean.items() if k.startswith(('live','pending'))) and
            clean['reportedGpuUsageLimitBytes']==1073741824 and clean['peakReportedGpuUsageBytes']<=1073741824,
            'Rejected candidate exceeded a guard or did not retire ownership')
    require('NORMAL_ROOT_GRAPHICS_EXIT result=125 normalOmarchySession=false' in guest and
            'Assertion failed!' in guest and 'ioctlFailed nr=12 errno=22' in guest and
            'HYPRLAND_HOST_QUEUE_END frames=1800 target=1800 failed=0' not in guest,
            'Missing retained mapping failure and desktop rejection')
    queued=[row for row in records if row.get('nativeSdlCompositorQueued') is True]
    require([row['frame'] for row in queued]==list(range(1,count+1)) and
            all(row['cpuUpload'] is False and row['qemuState']['vendor']=='NVIDIA Corporation' and
                row['qemuState']['rootAdapterVerified'] is row['qemuState']['independentFences'] is True
                for row in queued),'Rejected attempt lacks the reported GPU handoffs')
    budgets=[row for row in records if row.get('nativeMemoryBudgetQueried') is True]
    require(bool(budgets) and {row['group'] for row in budgets}=={0,1} and
            all(row['sharedAggregateBudget'] is row['currentProcessOnly'] is True and
                row['aggregateLimitBytes']==1073741824 and row['presentationReserveBytes']==67108864 and
                row['localBudgetShareBytes']+row['nonlocalBudgetShareBytes']==1006632960 and
                row['budget']==min(row['nativeBudget'],row['localBudgetShareBytes'] if row['group']==0 else row['nonlocalBudgetShareBytes'])
                for row in budgets),'Shared memory budget changed or fabricated usage')
    ownership=validate_ownership(report,records) if BRIDGES[bridge]=='81' else dict(
        exactLedgerReplayed=False,aggregateCountersOnly=True,fullDesktopAccepted=False)
    quotas=[row for row in records if row.get('nativeGpuVaAdmissionRejected') is True]
    if BRIDGES[bridge]=='81':
        require(len(quotas)==clean['gpuVaQuotaRejections']>0 and
                all(row['reason']=='aggregate mapped address budget' and row['memoryGuardRaised'] is row['nativeCall'] is False and
                    0<=row['usedPages']<=row['limitPages']==262144 and row['requestedPages']>row['limitPages']-row['usedPages']
                    for row in quotas),'Missing exact native mapped-address quota rejection')
    return dict(accepted=False,frames=count,traceComplete=trace_proof['complete'],ownership=ownership,
                mappedAddressQuotaObserved=bool(quotas),quotaRejections=quotas,
                nativeCleanupVerified=True,naturalShutdownVerified=True,sourceImageUnchanged=True,
                rootCauseVerified=False,fullDesktopAccepted=False,nearNativePerformanceVerified=False)
