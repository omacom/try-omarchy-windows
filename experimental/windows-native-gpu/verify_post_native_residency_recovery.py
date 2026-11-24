"""Audit historical residency observations, including the rejected candidate.

One explicit host-policy refusal is allowed after successful native residency.
Run169 lost the device after a four-allocation rollback. A passing single-list
observation does not establish that this historical implementation is safe.
This is not actual Windows quota exhaustion or desktop product acceptance.
Original traces and reports are passed intact to the derived auditors.
"""
from pathlib import Path
import hashlib
from verify_hyprland_lifecycle_capture import require

BASE='5133d26db61ae839b4cc22057f8606ccb3b47ab953ba241c3d221b4fb949ad7e'
OWNERSHIP='842edb5afb01247221a1327c920a1d4586c6e75a83ec7783e30fd2cbf6c40bad'
BRIDGE='ef549c3aa3a1e5f4b775348dd6058ae49b233d1848e4907ca2e99241174dff63'
BUILD='351c8a8385c1d70e44fe3957878561b18914908487574b972a4bb847066f05c5'
MANIFEST='2185b3547247dd6ad7224795af04e5c0d0c2656af8fa8c085a840726ead0ef57'
GUEST='564839aae7e73fc114c2f85ed60e0d18c0933928c6263f2b60eb998a03cc8371'


def validate_nonaccepted_device_loss(report,records,source):
    """Retain the multi-allocation counterexample without inventing missing data."""
    require(report['driverBridgeSha256']==BRIDGE and
            source['nativeBuildReceipt']['bridgeSha256']==BRIDGE and
            source['diagnosticResidencyRejectionEnabled'] is True,
            'Wrong rejected native candidate')
    events=[(i,r) for i,r in enumerate(records) if 'nativeDiagnosticResidencyRejection' in r]
    undo=[(i,r) for i,r in enumerate(records) if 'nativeDeferredResidencyBudgetRejected' in r]
    losses=[(i,r) for i,r in enumerate(records) if 'nativeAsyncRetirementFailed' in r]
    require(len(events)==len(undo)==len(losses)==1,'Missing unique rollback/device-loss sequence')
    ei,event=events[0];ri,rollback=undo[0];li,loss=losses[0]
    require(ei<ri<li and event['nativeDiagnosticResidencyRejection'] is True and
            event['origin']=='host-policy-after-native-residency' and
            event['nativeCount']==4 and event['nativeMakeResidentStatus']==259 and
            event['completedFrames']==116 and event['pagingTarget']==7063 and
            event['pagingRetired'] is event['actualUsageWithinGuard'] is True and
            event['nativeQuotaExceeded'] is event['gpuCompletionFabricated'] is False,
            'Rejected run no longer has its recorded four-allocation boundary')
    require(rollback['rollbackVerified'] is rollback['rollbackNativeCall'] is True and
            rollback['rolledBackReferences']==rollback['newNativeReferences']==4 and
            rollback['rollbackNtstatus']==rollback['rollbackOutcome']==0 and
            loss['nativeAsyncRetirementFailed'] is True and loss['reason']=='device loss',
            'Successful Evict/device-loss counterexample was altered')
    handoffs=[r['frame'] for r in records if r.get('nativeCompositorRingInput')]
    require(handoffs==list(range(1,117)),'Incomplete or extra pre-failure GPU sequence')
    clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(clean['driverCleanupVerified'] is False and clean['completedCompositorFrames']==116 and
            clean['liveHwQueues']>0 and clean['liveVendorResidencyAttempts']>0 and
            vm['ownedQemuExited'] is vm['qemuForcedStop'] is True and vm['qemuExit']==90 and
            vm['fenceMappingsReleasedAfterVmExit']>0 and vm['allocationMappingsReleasedAfterVmExit']>0,
            'Failed cleanup/forced VM release cannot be relabelled successful')
    return dict(observation='device-loss-after-four-allocation-rollback',accepted=False,
        nativeCount=4,rejectedAfterGpuHandoff=116,nativeMakeResidentStatus=259,
        nativeEvictionStatus=0,subsequentCompleteDesktopFrames=0,deviceLossObserved=True,
        liveNativeCleanupVerified=False,forcedVmStop=True,fullDesktopAccepted=False,
        historicalRollbackImplementationSafe=False,exactDeviceLossCauseEstablished=False,
        completeGuestTraceAvailable=False,nativeQuotaExhaustionVerified=False)


def rejection_metadata(report,guest,records,*,threshold=100,frame_range=(100,200)):
    injected=[(i,r) for i,r in enumerate(records) if 'nativeDiagnosticResidencyRejection' in r]
    rejected=[(i,r) for i,r in enumerate(records) if 'nativeDeferredResidencyBudgetRejected' in r]
    require(len(injected)==len(rejected)==1,'Missing/extra native residency policy refusal')
    index,event=injected[0];returned,rollback=rejected[0]
    require(event['nativeDiagnosticResidencyRejection'] is rollback['nativeDeferredResidencyBudgetRejected'] is True and
            all(type(event[k]) is int for k in ('threshold','completedFrames','nativeCount','nativeMakeResidentStatus','pagingTarget')) and
            all(type(rollback[k]) is int for k in ('requested','newNativeReferences','rolledBackReferences',
                'rollbackOutcome','rollbackNtstatus','reportedUsageBytes','limitBytes')),
            'Malformed native rejection receipt types')
    completed=event['completedFrames'];native=event['nativeCount']
    require(event['origin']=='host-policy-after-native-residency' and event['threshold']==threshold and
            frame_range[0]<=completed<=frame_range[1] and 0<native<=64 and event['nativeMakeResidentStatus'] in (0,0x103) and
            0<=event['pagingTarget']<2**64-1 and event['pagingRetired'] is event['actualUsageWithinGuard'] is True and
            event['nativeQuotaExceeded'] is event['gpuCompletionFabricated'] is False,
            'Wrong origin, paging retirement or preselected rejection boundary')
    require(returned>index and rollback['injected'] is rollback['rollbackVerified'] is True and
            rollback['newNativeReferences']==rollback['rolledBackReferences']==native and
            native<=rollback['requested']<=64 and rollback['rollbackOutcome']==rollback['rollbackNtstatus']==0 and
            rollback['reportedUsageObserved'] is True and 0<rollback['reportedUsageBytes']<=rollback['limitBytes']==1073741824 and
            rollback['logicalOwnershipUnchanged'] is rollback['rollbackNativeCall'] is True and
            rollback['guestResidencyPublished'] is rollback['reusedReferencesReleased'] is False,
            'Native eviction did not restore the unpublished reference increments')
    require(not any(r.get('nativeDeferredResidencyAdmitted') for r in records[index+1:returned]),
            'Guest ownership was published before rollback completed')
    before=[r['frame'] for r in records[:index] if r.get('nativeCompositorRingInput')]
    after=[r['frame'] for r in records[returned+1:] if r.get('nativeCompositorRingInput')]
    require(before==list(range(1,completed+1)) and after==list(range(completed+1,1801)) and len(after)>=1600,
            'Incomplete ordered GPU work before/after native rollback')
    require(guest.count('ioctlFailed nr=11 errno=12')==1 and
            not any(s in guest for s in ('EGL_BAD_ALLOC','BROKER_ROUTE errno=',
                'BROKER_ALIAS_REFUSAL_','BROKER_COORDINATOR_ACTOR failed=true','BROKER_OBJECT failed=true')),
            'Missing sole guest OOM or an additional allocation/protocol failure')
    require(report['disconnectCleanup']['failedVendorResidencyRequests']==1,
            'The real refused residency request was hidden or duplicated')
    return dict(origin=event['origin'],diagnosticOnly=True,injectionCount=1,
                rejectedAfterGpuHandoff=completed,nativeResidencyStatus=event['nativeMakeResidentStatus'],
                pagingRetirementVerified=True,nativeEvictionStatus=0,rolledBackNewReferences=native,
                previousAndReusedReferencesPreserved=True,guestOutOfMemoryIoctl=11,
                subsequentGpuHandoffs=len(after),nativeQuotaExhaustion=False,
                memoryPressureRecoveryVerified=False,physicalDisplayContinuityVerified=False)


def rollback_ownership():
    path=Path(__file__).resolve().parent/'verify_deferred_residency.py';raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==OWNERSHIP,'Frozen ownership auditor changed')
    old='    for row in records:\n        if row.get(\'nativeGuestEvictionDeferred\') is True:'
    new='''    for row in records:
        if row.get('nativeDeferredResidencyBudgetRejected') is True:
            require(row['injected'] is row['rollbackVerified'] is row['rollbackNativeCall'] is True and
                    row['rollbackOutcome']==row['rollbackNtstatus']==0 and
                    row['newNativeReferences']==row['rolledBackReferences']>0 and
                    row['logicalOwnershipUnchanged'] is True and row['reusedReferencesReleased'] is False,
                    'Uncertain rollback entered the native ownership ledger')
            native_calls+=1
        elif row.get('nativeGuestEvictionDeferred') is True:'''
    text=raw.decode('utf-8');require(text.count(old)==1,'Ownership derivation boundary changed')
    namespace={'__name__':'_post_native_rollback_ownership','__file__':str(path)}
    exec(compile(text.replace(old,new,1),str(path),'exec'),namespace)
    return namespace['validate_ownership']


def _desktop_namespace(bridge,build):
    path=Path(__file__).resolve().parent/'verify_multiapp_desktop.py';raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==BASE,'Frozen CPU384 auditor changed')
    text=raw.decode('utf-8')
    for old,new in [
        ("BRIDGE = '60f4bff9a8b2afde7526f6966bdec315a971eab81356ba954f7cfdb223c95ab3'",f"BRIDGE = '{bridge}'"),
        ("BUILD = '273976348b49c8d2a6ff1cc8ddaf341663beb08014e2c933c47022a3d6cd2168'",f"BUILD = '{build}'"),
        ("MANIFEST = '6bd54199bed5dcef9eebf92f49835103faff23d73c0c5dfec0a1c6062efcc92a'",f"MANIFEST = '{MANIFEST}'"),
        ("GUEST = '79c64bf444fd604b6559d790eb8382b0fc292f8641b2eafd458206d5e5f87d68'",f"GUEST = '{GUEST}'")]:
        require(text.count(old)==1,'Candidate identity derivation changed');text=text.replace(old,new,1)
    namespace={'__name__':'_post_native_residency_desktop','__file__':str(path)}
    exec(compile(text,str(path),'exec'),namespace)
    return namespace


def validate_post_native_residency_recovery(**inputs):
    source=inputs['source'];build=source['nativeBuildReceipt']
    require(source['diagnosticResidencyRejectionEnabled'] is source['residencyBudgetRollbackCandidate'] is True and
            source['diagnosticResidencyRejectionOrigin']=='host-policy-after-native-residency' and
            source['actualNativeQuotaExhaustion'] is False and
            build['diagnosticResidencyRejectionEnabled'] is build['binaryContainsInjectionMarker'] is True and
            build['diagnosticResidencyRejectionFrame']==100 and build['allGuardsUnchanged'] is True,
            'Wrong diagnostic residency build or overclaimed native quota origin')
    refusal=rejection_metadata(inputs['report'],inputs['guest'],inputs['records'])
    namespace=_desktop_namespace(BRIDGE,BUILD)
    original=namespace['derived'];ownership=rollback_ownership()
    def derived(name,replacements=()):
        replacements=list(replacements)
        if name=='verify_managed_mapping_desktop.py':
            replacements.append(("all(value==0 for key,value in clean.items() if key.startswith(('failed','live','pending','forced')) or",
                "all((value==1 if key=='failedVendorResidencyRequests' else value==0) for key,value in clean.items() if key.startswith(('failed','live','pending','forced')) or",1))
        result=original(name,replacements)
        if name=='verify_managed_residency_state.py':result['validate_ownership']=ownership
        return result
    namespace['derived']=derived
    proof=namespace['validate_multiapp_desktop'](**inputs)
    proof.update(controlledPostNativeResidencyRollbackVerified=True,rejectedResidency=refusal,
                 actualNativeQuotaExhaustionVerified=False,memoryPressureRecoveryVerified=False,
                 normalLauncherVerified=False,fullDesktopAccepted=False,nearNativePerformanceVerified=False)
    return proof


def validate_ordinary_residency_backend(**inputs):
    source=inputs['source'];build=source['nativeBuildReceipt']
    require(source['diagnosticResidencyRejectionEnabled'] is build['diagnosticResidencyRejectionEnabled'] is
            build['binaryContainsInjectionMarker'] is False and build['diagnosticResidencyRejectionFrame']==0 and
            build['allGuardsUnchanged'] is True and not any(r.get(k) for r in inputs['records'] for k in
                ('nativeDiagnosticResidencyRejection','nativeDeferredResidencyBudgetRejected')),
            'Ordinary build contains or exercised a diagnostic rejection')
    namespace=_desktop_namespace('d5235d0820b5918f390b044e46aebfba3a75ed093c6e4b36fbab4e2e834afb3a',
        'b273ad7b7f24e543916bc37405102395e6c3b45a36cc4ebc9019c9c7af7bd00a')
    proof=namespace['validate_multiapp_desktop'](**inputs)
    proof.update(ordinaryResidencyBackendCompatibilityVerified=True,diagnosticRejectionEnabled=False,
                 actualRollbackExercised=False,memoryPressureRecoveryVerified=False)
    return proof


def validate_nonaccepted_ordinary_coverage(**inputs):
    """Preserve the full auditor's zero-reuse rejection, alongside copy pixels."""
    from verify_real_sdl_desktop import pixel_audit
    try:
        validate_ordinary_residency_backend(**inputs)
    except ValueError as error:
        require(str(error)=='Missing real eviction/reuse/reclamation or retained reference leak',
                'The ordinary observation has a different rejection: '+str(error))
    else:
        raise ValueError('The zero-reuse observation no longer fails the original auditor')
    path=Path(__file__).resolve().parent/'verify_deferred_residency.py';raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==OWNERSHIP,'Frozen coverage observer changed')
    text=raw.decode('utf-8');old='requests>0 and reused>0 and reclaimed>0'
    require(text.count(old)==1,'Zero-reuse observation boundary changed')
    namespace={'__name__':'_nonaccepted_zero_reuse','__file__':str(path)}
    exec(compile(text.replace(old,'requests>0 and reused==0 and reclaimed>0',1),str(path),'exec'),namespace)
    ownership=namespace['validate_ownership'](inputs['report'],inputs['records'])
    report=inputs['report'];clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(clean['reusedResidencyReferences']==0 and clean['deferredEvictionRequests']>0 and
            clean['reclaimedResidencyReferences']>0 and clean['liveRetainedResidencyReferences']==0 and
            clean['reclaimedResidencyReferences']+clean['destroyedRetainedResidencyReferences']>0 and
            report['hostBridgeExit']==0 and vm['qemuExit']==0 and vm['qemuForcedStop'] is False and
            clean['driverCleanupVerified'] is True and clean['completedCompositorFrames']==1800 and
            all(v==0 for k,v in clean.items() if k.startswith(('failed','live','pending','forced')) or k.endswith('Timeouts')) and
            all(v==0 for k,v in vm.items() if k.startswith('live') or k.endswith('ReleasedAfterVmExit')),
            'The ordinary observation has a failure or retained references in addition to missing reuse coverage')
    pixels=[]
    for frame,index in ((1,1),(1800,2)):
        rows=[r for r in inputs['records'] if r.get('nativeSdlCompositorQueued') and r['frame']==frame]
        require(len(rows)==1,'Incomplete ordinary copy observation');state=rows[0]['qemuState']
        pixels.append(dict(frame=frame,**pixel_audit(inputs['frames'][f'compositor-copy-{index}.raw'],
            inputs['frames'][f'relay-source-{frame:016d}.rgba'],inputs['frames'][f'relay-window-{frame:016d}.rgba'],
            state['windowWidth'],state['windowHeight'])))
    return dict(compatibilityAccepted=False,originalAuditorRejected=True,reason='reference reuse was not exercised',
                completedGpuHandoffs=1800,residencyReferenceReuseExercised=False,
                ownership=ownership,completeCopyPixels=pixels,nativeCleanupBeforeNaturalVmExitVerified=True,fullDesktopAccepted=False)


def validate_nonaccepted_cursor_observation(**inputs):
    refusal=rejection_metadata(inputs['report'],inputs['guest'],inputs['records'])
    try:
        validate_post_native_residency_recovery(**inputs)
    except ValueError as error:
        require(str(error)=='Wrong complete desktop wallpaper','Different cursor observation failure: '+str(error))
    else:
        raise ValueError('Cursor observation no longer fails the original wallpaper oracle')
    raw=inputs['frames']['compositor-copy-2.raw'];rows=[]
    for x,y,r,g,b in inputs['wallpaper_reference']['probes']:
        if 2740<=x<=3820 and 140<=y<=900:continue
        error=sum(abs(a-b) for a,b in zip(raw[(y*3840+x)*4:(y*3840+x)*4+3][::-1],(r,g,b)))/3
        rows.append((error,x,y))
    failed=[(x,y) for error,x,y in rows if error>=40]
    require(len(rows)==601 and failed==[(1056,960)] and max(r[0] for r in rows)==53 and
            sum(r[0] for r in rows)/len(rows)<3,'Different wallpaper interference observation')
    return dict(controlledRecoveryAccepted=False,originalWallpaperGuardRejected=True,
                reason='visible cursor overlaps one fixed wallpaper probe',rejectedResidency=refusal,
                completeCopyPixelChecksPassedBeforeWallpaperRejection=True,wallpaperProbeCount=601,
                wallpaperProbesOver40=1,rejectedProbe=[1056,960],maximumWallpaperError=53,
                cursorOverlapIdentifiedByVisualInspection=True,physicalInputCauseVerified=False,
                fullDesktopAccepted=False,memoryPressureRecoveryVerified=False)
