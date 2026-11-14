"""Verify one labelled pre-native refusal and subsequent real GPU rendering.

This does not accept the unresolved natural 16-reference failure. Original
source/SDL pixels, real fences, official input, budgets and live retirement
remain mandatory. Only the precisely validated injected diagnostic is allowed.
"""
from pathlib import Path
import hashlib
import re
from verify_hyprland_lifecycle_capture import require
from verify_qmp_input_delivery import snapshot

BASE = '5133d26db61ae839b4cc22057f8606ccb3b47ab953ba241c3d221b4fb949ad7e'
MANIFEST = 'a31a36c638221afda3c848a9493c3f61afc9464c683b91b2b5bf85acd6aa8928'
GUEST = '4bbe278ff20753def5ddd55b59e2270508036a2639676f640f01ec1f56f56e8f'


def validate_probe_controls(receipt,log):
    require(receipt['stateControlsPassed'] is receipt['invalidBuildsRejected'] is
            receipt['normalBrokerByteIdenticalToGuest130'] is receipt['diagnosticBinaryContainsInjection'] is True and
            receipt['normalBinaryContainsInjection'] is receipt['injectedNativeReply'] is receipt['officialInputChanged'] is False and
            receipt['allQuotasUnchanged'] is True and receipt['threshold']==100 and
            receipt['origin']=='guest-before-native' and receipt['originLabelVersion']==2 and
            receipt['failedSourceTicketRecorded'] is True and
            receipt['normalSha256']==receipt['baselineSha256']=='c6bced9dc18e7cafb0eb7868ed7d7c6ed1cc09e974d9b8d01c2f504e0dca54b8' and
            receipt['diagnosticSha256']=='309ad3b66fc15f9f595d19c1c0b6c7ac3b244dfcd13a1739c8b79618aceeee78',
            'Wrong diagnostic build or changed ordinary broker')
    marker=('DIAGNOSTIC_ALIAS_REFUSAL primaryOnly=true acceptedFramesSequential=true oneResourceRefusal=true '
            'syncAliasesUnchanged=true invalidThresholdRejected=true injectedOriginTyped=true nativeCallsMade=false gpuProof=false')
    require(log.count(marker)==1 and all(log.count('EXPECTED_CONFIGURATION_REJECTION '+name)==1 for name in
            ('missing-optin','zero-threshold','late-threshold')),'Missing diagnostic state/build rejection controls')
    return dict(stateAndBuildControlsVerified=True,ordinaryBrokerByteIdenticalToGuest130=True,
                ordinaryBrokerContainsInjection=False,diagnosticOriginTyped=True,failedResourceIdentityRecorded=True,
                allQuotasUnchanged=True,officialInputChanged=False,gpuRecoveryEstablishedByTheseControls=False)


def injection_metadata(guest, records):
    def single(pattern, prefix):
        rows=list(re.finditer(pattern,guest,re.M))
        require(len(rows)==1 and guest.count(prefix)==1,'Missing/duplicate/malformed '+prefix)
        return rows[0]
    actors=[(int(actor),int(pid),role) for actor,pid,role in re.findall(
        r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=(\d+) primary=(true|false) pidfdAuthenticated=true nativeDriverCount=1$',guest,re.M)]
    require(len(actors)==len({a for a,_,_ in actors})==len({p for _,p,_ in actors})==8,'Wrong authenticated GPU actor set')
    primary=[(a,p) for a,p,role in actors if role=='true']
    require(len(primary)==1,'Missing unique presentation owner')
    actor,pid=primary[0]
    armed=single(r'^BROKER_DIAGNOSTIC_ALIAS_REFUSAL_ARMED actor=(\d+) pid=(\d+) completedFrames=100 threshold=100 actualNativeReplyAccepted=true testOnly=true gpuProof=false$',
                 'BROKER_DIAGNOSTIC_ALIAS_REFUSAL_ARMED ')
    require(tuple(map(int,armed.groups()))==(actor,pid),'Refusal armed outside the authenticated primary')
    injected=single(r'^BROKER_DIAGNOSTIC_ALIAS_REFUSAL_INJECTED actor=(\d+) pid=(\d+) donor=(\d+) sourceTicket=(\d+) kind=2 completedFrames=(\d+) errno=12 origin=guest-before-native nativeAliasCalled=false testOnly=true resourcesReleased=false gpuProof=false$',
                    'BROKER_DIAGNOSTIC_ALIAS_REFUSAL_INJECTED ')
    recipient,client,donor,source_ticket,completed=map(int,injected.groups())
    require((recipient,client)==(actor,pid) and 100<=completed<=1700 and source_ticket>0 and
            donor!=actor and donor in {a for a,_,_ in actors} and armed.end()<injected.start(),
            'Wrong injected actor, source or accepted-frame boundary')
    ready=snapshot(guest,'OMARCHY_MULTIWINDOW_READY_CLIENTS_HEX')
    donor_pid=next(p for a,p,_ in actors if a==donor)
    peers={int(p):bytes.fromhex(path).decode() for p,path in re.findall(
        r'^BROKER_COORDINATOR_PEER pid=(\d+) primary=(?:true|false) exePathHex=([0-9a-f]+) pidfdAuthenticated=true$',guest,re.M)}
    require(peers.get(pid)=='/usr/bin/Hyprland' and
            (donor_pid in {c['pid'] for c in ready} or peers.get(donor_pid)=='/usr/bin/quickshell'),
            'Refusal source is not a live desktop client')
    snap=single(r'^BROKER_ALIAS_REFUSAL_SNAPSHOT actor=(\d+) kind=2 errno=12 liveFdReferences=(\d+) reapedFdReferences=(\d+) pendingReaps=(\d+) afterAliasAdmissionRefused=true diagnosticInjected=true registryLocked=true resourcesReleased=false gpuProof=false$',
                'BROKER_ALIAS_REFUSAL_SNAPSHOT ')
    owner,live,reaped,pending=map(int,snap.groups())
    require(owner==actor and 0<live+reaped<=16 and 0<=pending<=16 and injected.end()<snap.start() and
            'afterNativeAdmissionRefused=' not in guest and 'BROKER_ALIAS_REFUSAL_SNAPSHOT_UNAVAILABLE' not in guest,
            'Snapshot origin is ambiguous or reference counts exceed the fixed actor guard')
    references=re.findall(r'^BROKER_ALIAS_REFUSAL_REFERENCE actor=(\d+) kind=2 cookie=(\d+) sourceActor=(\d+) sourceTicket=(\d+) recipientTicket=(\d+) kernelFdReaped=(true|false) registryLocked=true gpuProof=false$',guest,re.M)
    require(len(references)==guest.count('BROKER_ALIAS_REFUSAL_REFERENCE ')==live+reaped and
            len({int(r[1]) for r in references})==len({int(r[4]) for r in references})==len(references) and
            sum(r[5]=='true' for r in references)==reaped and
            all(int(a)==actor and int(cookie)>0 and int(src) in {x for x,_,_ in actors} and int(ticket)>0 and int(alias)>0
                for a,cookie,src,ticket,alias,_ in references) and
            not any((int(r[2]),int(r[3]))==(donor,source_ticket) for r in references),
            'Refusal reference snapshot is inconsistent or published the refused alias')
    egl=single(r'^libEGL debug: EGL user error 0x3003 \(EGL_BAD_ALLOC\) in eglCreateImageKHR: createImageFromDmaBufs failed$',
               'EGL_BAD_ALLOC')
    require(snap.end()<egl.start(),'EGL error does not follow the injected capacity reply')
    aliases=list(re.finditer(r'^BROKER_COORDINATOR_ALIAS donor=(\d+) recipient=(\d+) kind=([12]) sourceTicket=(\d+) recipientTicket=(\d+) cookie=(\d+) realFd=true$',guest,re.M))
    retry=[r for r in aliases if tuple(map(int,r.groups()[:4]))==(donor,actor,2,source_ticket)]
    require(len(retry)==1 and retry[0].start()>egl.end() and
            all(int(v)>0 for v in retry[0].groups()),'The same refused resource was not successfully aliased later')
    native=[r for r in records if r.get('nativeSharedReferenceAliased')]
    require(len(native)==len(aliases)>0 and
            all(r.get('independentNtReference') is True and r.get('inheritable') is r.get('rawHandleExposed') is False for r in native) and
            sum(r['kind']=='resource' for r in native)==sum(r.group(3)=='2' for r in aliases),
            'Native recipient references do not reconcile with real FD aliases')
    acknowledgements=list(re.finditer(r'^HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=(\d+) audit=[01] retries=\d+ syncFileClosed=true eglSyncDestroyed=true guestCpuMap=false sourceCopyRetired=true displayVerified=false$',guest,re.M))
    before=[int(r.group(1)) for r in acknowledgements if r.end()<injected.start()]
    after=[int(r.group(1)) for r in acknowledgements if r.start()>injected.end()]
    require(before and after and before[-1]<=completed<=before[-1]+1 and completed<=after[0]<=completed+1 and
            int(acknowledgements[-1].group(1))==1800 and retry[0].end()<acknowledgements[-1].start(),
            'Injected boundary or subsequent GPU work is inconsistent')
    require(not any(r.get('nativeSharedAliasAdmissionRejected') for r in records) and
            'BROKER_ROUTE rejected=true' not in guest and 'BROKER_COORDINATOR_ACTOR failed=true' not in guest and
            guest.count('EGL_BAD_ALLOC')==1 and 'BROKER_OBJECT failed=true' not in guest,
            'A separate native/driver/protocol failure occurred')
    return dict(origin='guest-before-native',diagnosticOnly=True,injectionCount=1,
                armedAfterAcceptedFrame=100,injectedAfterAcceptedFrame=completed,
                primaryActor=actor,primaryPid=pid,donorActor=donor,sourceTicket=source_ticket,
                snapshotLiveFdReferences=live,snapshotReapedFdReferences=reaped,pendingReaps=pending,
                sameResourceRetryVerified=True,retryRecipientTicket=int(retry[0].group(5)),
                retryCookie=int(retry[0].group(6)),eglBadAllocObserved=True,
                subsequentGpuHandoffs=1800-completed,actualNativeQuotaRefusalExercised=False,
                naturalReferenceLimitCauseResolved=False,physicalDisplayContinuityVerified=False)


def validate_injected_alias_gpu_recovery(**inputs):
    path=Path(__file__).resolve().parent/'verify_multiapp_desktop.py';raw=path.read_bytes()
    require(hashlib.sha256(raw).hexdigest()==BASE,'Frozen CPU384 auditor changed')
    source=inputs['source']
    require(source['diagnosticAliasRefusalEnabled'] is True and source['diagnosticAliasRefusalOriginLabelVersion']==2 and
            source['diagnosticAliasRefusalExpectedOrigin']=='guest-before-native' and source['aliasCapacityRecoveryEnabled'] is True,
            'Diagnostic refusal provenance is absent or ambiguous')
    refusal=injection_metadata(inputs['guest'],inputs['records'])
    text=raw.decode('utf-8')
    changes=[
        ("MANIFEST = '6bd54199bed5dcef9eebf92f49835103faff23d73c0c5dfec0a1c6062efcc92a'",f"MANIFEST = '{MANIFEST}'"),
        ("GUEST = '79c64bf444fd604b6559d790eb8382b0fc292f8641b2eafd458206d5e5f87d68'",f"GUEST = '{GUEST}'"),
        ("not any(s in guest for s in ('BROKER_ALIAS_REFUSAL_', 'BROKER_ROUTE errno=', 'EGL_BAD_ALLOC'))",
         'VALIDATED_SINGLE_INJECTED_DIAGNOSTIC')]
    for old,new in changes:
        require(text.count(old)==1,'Candidate auditor derivation boundary changed')
        text=text.replace(old,new,1)
    namespace={'__name__':'_single_injected_alias_gpu','__file__':str(path),'VALIDATED_SINGLE_INJECTED_DIAGNOSTIC':True}
    exec(compile(text,str(path),'exec'),namespace)
    proof=namespace['validate_multiapp_desktop'](**inputs)
    proof.update(controlledPreNativeAliasRefusalGpuRecoveryVerified=True,injectedRefusal=refusal,
                 actualNativeQuotaRefusalRecoveryVerified=False,intermittentAliasLimitResolved=False,
                 normalLauncherVerified=False,fullDesktopAccepted=False,nearNativePerformanceVerified=False)
    return proof


def validate_nonaccepted_injection(number,**inputs):
    """Keep initial label defects and an unexpected calendar overlay rejected."""
    from verify_coordinated_gpu_clients import ROOT
    from verify_multiapp_desktop import canonical_sha
    from verify_real_sdl_desktop import pixel_audit
    from virtio_trace import merge_session
    require(number in (151,153),'Unknown nonaccepted diagnostic observation')
    report=inputs['report'];source=inputs['source'];guest=inputs['guest'];records=inputs['records']
    require(report['driverBridgeSha256']=='60f4bff9a8b2afde7526f6966bdec315a971eab81356ba954f7cfdb223c95ab3' and
            report['hostBridgeExit']==0 and report['disconnectCleanup']['driverCleanupVerified'] is True and
            report['disconnectCleanup']['completedCompositorFrames']==1800 and
            report['ownedQemuControl']['qemuExit']==0 and report['ownedQemuControl']['qemuForcedStop'] is False and
            source['sourceImageUnchanged'] is True and source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT,
            'Wrong observation provenance, GPU count or cleanup')
    merged,_=merge_session(inputs['uart_raw'],inputs['trace_raw'])
    require(guest==merged.replace('\r\n','\n').replace('\r','\n'),'Observation differs from the lossless live trace')
    queued=[r for r in records if r.get('nativeSdlCompositorQueued')]
    require([r['frame'] for r in queued]==list(range(1,1801)),'Missing observation GPU handoffs')
    if number==151:
        require(report['initramfsSha256']=='06f5d6bd318300e55b1e8b2ad7fae1e190a64a26ab03896bc36d8dc725946dfb' and
                canonical_sha(source['manifest'])=='cb30454ea33bbfecb2baa272bbeb3043698f4ee92c283e75e0c9a1624a0f45d8' and
                'diagnosticAliasRefusalOriginLabelVersion' not in source and
                guest.count('BROKER_DIAGNOSTIC_ALIAS_REFUSAL_INJECTED ')==1 and
                'completedFrames=154 errno=12 origin=guest-before-native nativeAliasCalled=false' in guest and
                'afterNativeAdmissionRefused=true registryLocked=true resourcesReleased=false' in guest and
                'diagnosticInjected=true' not in guest,'Initial origin-label defect was relabelled as accepted')
        pixels=[]
        for number_,index in ((1,1),(1800,2)):
            state=queued[number_-1]['qemuState']
            pixels.append(dict(frame=number_,**pixel_audit(inputs['frames'][f'compositor-copy-{index}.raw'],
                inputs['frames'][f'relay-source-{number_:016d}.rgba'],inputs['frames'][f'relay-window-{number_:016d}.rgba'],
                state['windowWidth'],state['windowHeight'])))
        return dict(run=151,controlledRecoveryAccepted=False,reason='inherited snapshot origin label incorrect for pre-native injection',
                    completedHandoffs=1800,nativeCleanupVerified=True,completeSourceWindowPixelChecks=pixels,
                    nativeQuotaRefusalVerified=False,fullDesktopAccepted=False)
    refusal=injection_metadata(guest,records)
    try:
        validate_injected_alias_gpu_recovery(**inputs)
    except ValueError as error:
        require(str(error)=='Wrong complete desktop wallpaper','Unexpected rejection of the calendar observation: '+str(error))
    else:
        raise ValueError('Calendar observation no longer fails the original wallpaper oracle')
    reference=inputs['wallpaper_reference'];last=inputs['frames']['compositor-copy-2.raw']
    errors=[sum(abs(a-b) for a,b in zip(last[(y*3840+x)*4:(y*3840+x)*4+3][::-1],(r,g,b)))/3
            for x,y,r,g,b in reference['probes'] if not (2740<=x<=3820 and 140<=y<=900)]
    require(len(errors)==601 and sum(e>=40 for e in errors)==20 and max(errors)==101 and
            sum(errors)/len(errors)>3,'Calendar observation no longer matches its strict oracle rejection')
    return dict(run=153,controlledRecoveryAccepted=False,reason='unexpected calendar overlay occludes the fixed wallpaper probes',
                injectedRefusal=refusal,completedHandoffs=1800,nativeCleanupVerified=True,
                completeSourceWindowPixelChecksPassedBeforeWallpaperRejection=True,
                originalWallpaperGuardRejected=True,wallpaperSampleCount=601,
                wallpaperMeanRgbError=sum(errors)/len(errors),wallpaperMaximumRgbError=max(errors),
                wallpaperSamplesOver40=20,calendarIdentityEstablishedByVisualInspection=True,
                physicalInputCauseEstablished=False,nativeQuotaRefusalVerified=False,fullDesktopAccepted=False)
