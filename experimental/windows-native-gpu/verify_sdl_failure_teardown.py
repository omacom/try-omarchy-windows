"""Verify bounded failure teardown separately from device-loss recovery or desktop acceptance."""
from verify_coordinated_gpu_clients import ROOT
from verify_hyprland_lifecycle_capture import require
from verify_real_sdl_desktop import pixel_audit

BASELINE='61cbbc0597fce31088da5cdc43e82670958dd7dab63b7e396aa43a2c179906f0'
CANDIDATE='cf396dfdf0ae4358c3372e140623341e8c87200ff15784dea41798d391748d69'
QEMU='6bee9d9e18bc0e89757e0364a29e64f1057e4854ec9a7a816b063f8d8f3ed6a5'
GUEST='419f6a165dfa9b59fd7bba66c6df94d248b20e1b7e506b617ca5ee63a63476d0'
FIRST_FAILURE='Injected SDL coordinator failure after frame 64; diagnostic only'


def source_proof(report,source,bridge):
    require(report['driverBridgeSha256']==source['nativeBuildReceipt']['bridgeSha256']==bridge and
            report['qemuSha256']==source['qemuSdlBuildReceipt']['files']['bin/qemu-system-x86_64.exe']==QEMU and
            report['initramfsSha256']==source['manifest']['initramfsSha256']==GUEST,
            'Mixed compiled GPU/guest/runtime inputs')
    require(source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is
            source['qemuArtifactUnchanged'] is source['activeFrameProbeSourcesUnchanged'] is True and
            source['sourceImageSha256Before']==source['sourceImageSha256After']==ROOT and
            source['ownedRuntimeHarnessSha256']=='e6c29c6cf1ee42d93c8c3e858c4d685237a902d05218dbe840533c2069f03286',
            'Source image, source archive or retained harness changed')
    require('--driver-sdl-relay' in source['command'] and '--driver-rootfs-readonly' in source['command'] and
            report['liveNvidiaLinuxUmdLoaded'] is True and report['realWslDxgForwarding'] is False and
            report['capturedPrivateFixturesUsed'] is False,'Foreign/software runtime')


def one(records,key):
    rows=[row for row in records if key in row]
    require(len(rows)==1 and rows[0][key] is True,'Missing/duplicate/failed '+key)
    return rows[0]


def validate_failure_teardown(report,records,source,kind):
    require(kind in ('baseline-stream','fixed-stream','fixed-retirement'),'Unknown failure control')
    source_proof(report,source,BASELINE if kind=='baseline-stream' else CANDIDATE)
    require(source['failureProbeMode']==('retirement' if kind=='fixed-retirement' else 'stream'),
            'Wrong failure probe')
    probe=one(records,'nativeSdlFailureProbe');original=one(records,'nativeCoordinatorFailure')
    require(probe['afterAcceptedFrames']==64 and probe['beforeGuestReply'] is probe['syntheticFailure'] is True and
            probe['gpuDeviceRemoved'] is False and probe['retirementFailureRequested'] is (kind=='fixed-retirement') and
            original['message']==FIRST_FAILURE and original['beforeOwnedVmStop'] is original['beforeGlobalTeardown'] is True and
            records.index(probe)<records.index(original),'First failure was lost or overclaimed')
    ingress=[r for r in records if r.get('nativeCompositorRingInput')]
    require([r['frame'] for r in ingress]==list(range(1,65)),'Failure did not interrupt the intended live stream')
    clean=report.get('disconnectCleanup',{})
    failures=[r for r in records if r.get('nativeSdlCompositorFailure')]
    if kind=='fixed-stream':
        require(report['hostBridgeExit']==1 and clean.get('driverCleanupVerified') is True and not failures and
                not any(r.get('nativeSdlFatalOwnerReaped') for r in records),'Recoverable interruption became fatal')
        detached=one(records,'nativeSdlCompositorDetached');stopped=one(records,'nativeCompositorRingStopped')
        teardown=one(records,'nativeCoordinatorTeardown');input_stop=one(records,'nativeInputChannelStopped')
        require(detached['frames']==detached['producerFenceObserved']==detached['consumerFenceObserved']==64 and
                detached['audits']==1 and detached['qemuState']['liveImportedObjects']==0 and
                detached['qemuState']['attached'] is False and stopped['submitted']==stopped['presented']==64 and
                stopped['liveGpuResources']==0 and stopped['uiThreadJoined'] is True and
                teardown['vmReaped'] is teardown['allWireObjectsReleased'] is True and
                records.index(original)<records.index(detached)<records.index(stopped)<records.index(input_stop)<records.index(teardown),
                'SDL output did not retire before VM/global cleanup')
        vm=report['ownedQemuControl']
        require(vm['ownedQemuExited'] is vm['qemuForcedStop'] is True and vm['qemuExit']==90 and
                all(value==0 for key,value in clean.items() if key.startswith(('live','failed','pending','forced'))) and
                all(value==0 for key,value in vm.items() if key.startswith('live')),'Unreleased owned failure resources')
        return dict(kind=kind,framesRetired=64,liveSdlDetachBeforeVmStop=True,firstFailurePreserved=True,
                    normalNativeCleanupVerified=True,syntheticFailure=True,gpuDeviceLossRecoveryVerified=False,
                    fullDesktopAccepted=False,nearNativePerformanceVerified=False)
    fatal=one(records,'nativeSdlFatalOwnerReaped')
    require(report['hostBridgeExit']==3 and not clean.get('driverCleanupVerified') and
            fatal['ownedQemuReaped'] is fatal['beforeHostFatalExit'] is fatal['driverObjectsRetained'] is
            fatal['qemuForcedStop'] is True and fatal['qemuExit']==90 and
            fatal['inputImplementationChanged'] is False and records.index(original)<records.index(fatal),
            'Fatal host exit lacks prior owned VM reap proof')
    require(failures and all(records.index(row)<records.index(fatal) for row in failures),'Fatal error diagnostics missing')
    if kind=='baseline-stream':
        require(any(row['message']=='SDL consumer device lost' for row in failures),'Original teardown failure absent')
    else:
        require(any(row['phase']=='retirement' and row['message']=='Injected SDL detach failure; diagnostic only'
                    for row in failures),'Wrong retirement fault')
    return dict(kind=kind,framesAccepted=64,firstFailurePreserved=True,ownedVmReapedBeforeHostFatalExit=True,
                normalNativeCleanupVerified=False,syntheticFailure=True,gpuDeviceLossRecoveryVerified=False,
                fullDesktopAccepted=False,nearNativePerformanceVerified=False)


def validate_uninterrupted_regression(report,records,source,frames):
    source_proof(report,source,CANDIDATE)
    require(source['failureProbeMode']=='none' and report['hostBridgeExit']==0 and
            not any(any(key in row for key in ('nativeSdlFailureProbe','nativeCoordinatorFailure','nativeSdlCompositorFailure',
                                               'nativeSdlFatalOwnerReaped')) for row in records),'Normal stream contains failure probe/fault')
    queued=[r for r in records if r.get('nativeSdlCompositorQueued')]
    require([row['frame'] for row in queued]==list(range(1,301)),'Missing/reordered normal frame')
    for row in queued:
        state=row['qemuState']
        require(row['cpuUpload'] is row['ordinaryCpuFenceWait'] is state['poisoned'] is False and
                state['rootAdapterVerified'] is state['attached'] is True and state['liveImportedObjects']==4 and
                state['sourceWidth']==3840 and state['sourceHeight']==2160 and state['sourceFormat']=='bgra8' and
                state['lastQueuedFrame']==state['convertedFrames']==row['frame'] and
                'NVIDIA' in state['renderer'],'Foreign/failed normal GPU output')
    proof=[]
    for index,number in enumerate((1,300),1):
        state=queued[number-1]['qemuState']
        proof.append(dict(frame=number,**pixel_audit(frames[f'compositor-copy-{index}.raw'],
            frames[f'relay-source-{number:016d}.rgba'],frames[f'relay-window-{number:016d}.rgba'],
            state['windowWidth'],state['windowHeight'])))
    detached=one(records,'nativeSdlCompositorDetached');ended=one(records,'nativeCompositorEndRetired')
    require(detached['frames']==detached['producerFenceObserved']==detached['consumerFenceObserved']==300 and
            detached['qemuState']['liveImportedObjects']==0 and ended['beforeFinalGuestReply'] is ended['beforeOwnedVmStop'] is True and
            records.index(detached)<records.index(ended),'Final GPU references did not retire live')
    clean=report['disconnectCleanup'];vm=report['ownedQemuControl']
    require(clean['driverCleanupVerified'] is vm['ownedQemuExited'] is True and vm['qemuExit']==0 and
            vm['qemuForcedStop'] is False and all(value==0 for key,value in clean.items() if key.startswith(('live','failed','pending','forced'))) and
            all(value==0 for key,value in vm.items() if key.startswith('live') or key.endswith('ReleasedAfterVmExit')),
            'Unclean normal regression')
    return dict(ordinaryRegressionPassed=True,frames=proof,frameCount=300,liveSdlDetachVerified=True,
                fullDesktopAccepted=False,nearNativePerformanceVerified=False,gpuDeviceLossRecoveryVerified=False)
