"""Verify the explicitly negotiated residency/address-space separation."""
from verify_hyprland_lifecycle_capture import require
from verify_deferred_residency import validate_ownership

def validate_managed_state(report,records,source,guest,require_virtual_pressure=True):
    clean=report['disconnectCleanup'];manifest=source['manifest']
    policy=dict(coordinatorObjectsVersion=6,reservedAddressBytes=68719476736,standaloneMappingBytes=1073741824,
        residentUsageGuardBytes=1073741824,cpuMappingBytes=201326592,rendererCodeChanged=False,inputCodeChanged=False,
        physicalMemoryGuardRaised=False,virtualAddressAccountingSeparated=True)
    require(manifest['managedGpuVaPolicy']==policy and source['managedMappingNegotiationVersion']==6 and
            source['guestCompileInputVerification']=='frozen source inputs from the exact guest build' and
            source['memoryGuardRaised'] is False,'Mixed managed mapping contract or source guard')
    contract=[r for r in records if r.get('nativeCoordinatedObjectCapacity')]
    require(len(contract)==1 and contract[0]['version']==6 and contract[0]['reservationMappingsSeparated'] is True and
            contract[0]['reservedAddressBytes']==68719476736 and contract[0]['cpuBytes']==201326592 and
            contract[0]['reportedGpuBytes']==1073741824 and contract[0]['memoryBudgetsMultiplied'] is False and
            'BROKER_COORDINATOR_MANAGED_MAPPING_CAPACITY version=6 reservedAddressBytes=68719476736 standaloneMappingBytes=1073741824 residentUsageBytes=1073741824 cpuBytes=201326592' in guest,
            'Missing exact guest/host mapping agreement')
    rows=[r for r in records if r.get('nativeGpuMappingAccounted')]
    require(bool(rows) and any(r['reservationBacked'] is True for r in rows) and
            all(0<=r['standalonePages']<=r['standaloneLimitPages']==262144 and
                r['standalonePages']<=r['totalPages']<=r['standalonePages']+r['reservationLimitPages'] and
                r['reservationLimitPages']==16777216 and r['residentUsageGuardBytes']==1073741824 and
                r['ownedReservationChecked'] is True for r in rows),'Address mappings bypass ownership or standalone capacity')
    crossed_original_limit=max(r['totalPages'] for r in rows)>262144
    if require_virtual_pressure:
        require(crossed_original_limit,'The extended virtual mapping boundary was not exercised')
    require(clean['reservationMappingAccountingSeparated'] is True and clean['standaloneGpuMappedByteLimit']==1073741824 and
            clean['vendorGpuMappedByteLimit']==69793218560 and clean['gpuVaQuotaRejections']==0 and
            clean['peakStandaloneMappedPages']==max(r['standalonePages'] for r in rows)<=262144 and
            clean['peakVendorMappedPages']==max(r['totalPages'] for r in rows) and
            clean['reportedGpuUsageLimitBytes']==1073741824 and clean['peakReportedGpuUsageBytes']<=1073741824,
            'Address separation changed a physical guard or hid quota rejections')
    budgets=[r for r in records if r.get('nativeMemoryBudgetQueried')]
    require(bool(budgets) and {r['group'] for r in budgets}=={0,1} and
            all(r['sharedAggregateBudget'] is r['currentProcessOnly'] is True and
                r['aggregateLimitBytes']==1073741824 and r['presentationReserveBytes']==67108864 and
                r['localBudgetShareBytes']+r['nonlocalBudgetShareBytes']==1006632960 and
                r['budget']==min(r['nativeBudget'],r['localBudgetShareBytes'] if r['group']==0 else r['nonlocalBudgetShareBytes'])
                for r in budgets),'Aggregate residency budget was multiplied or usage fabricated')
    build=source['nativeBuildReceipt']
    require(build['managedMappingNegotiationVersion']==6 and build['deferredResidencyOptIn'] is True and
            build['gpuProtocolChanged'] is True and build['officialInputChanged'] is build['memoryGuardRaised'] is False,
            'Build does not record the explicit mapping protocol change')
    ownership=validate_ownership(report,records)
    return dict(**ownership,mappingAgreementVersion=6,virtualMappingsExceedOriginalQuota=crossed_original_limit,
        extendedVirtualMappingBoundaryAccepted=crossed_original_limit,
        peakVirtualMappedBytes=clean['peakVendorMappedPages']*4096,
        peakStandaloneMappedBytes=clean['peakStandaloneMappedPages']*4096,
        reportedResidentGuardBytes=1073741824,reportedResidentGuardUnchanged=True,
        physicalMemoryGuardRaised=False,officialInputChanged=False)
