"""Verify native 4K GPU readback only; preserve failed desktop/lifecycle gates."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from verify_hyprland_compositor_capture import fnv
from verify_hyprland_lifecycle_capture import require


NATIVE_SHA256='a0c95013376eee6b0688485e82100e763d3ce0c5d0c0e93a23e9b94daa7e40ed'
PRESENTATION_NATIVE_SHA256='6586c6e6583d45974048709bae24ae5c719b15cb9a107e4b22e5fe9b5eb4274e'
PRESENTATION_NATIVE_BUILDS=(PRESENTATION_NATIVE_SHA256,'d2db1e9260950b6185ed9c575e3cdfdd15a3540b76202515ac4f3fb63e9fd156')
QEMU_SHA256='dcd422e6a7882af4ede4d6a79915dcfa6b344253b0b008cd6cebb55e62889111'
ROOT_SHA256='3954123192111ce2d28cafb9dd108e1ca14c288bfc2429ed899b0f36aeccf942'


def validate(report,guest,records,frames,source):
    manifest=source['manifest'];clean=report['disconnectCleanup']
    version=manifest.get('kernelCpuCapacityProfile')
    require(version in (9,10),'Unsupported exact 4K readback profile')
    natives=PRESENTATION_NATIVE_BUILDS if version==10 else (NATIVE_SHA256,)
    usage_limit=(1024 if version==10 else 512)*1024*1024
    require(report.get('driverBridgeSha256')==source['nativeBuildReceipt'].get('bridgeSha256') and report.get('driverBridgeSha256') in natives and
            report.get('qemuSha256')==QEMU_SHA256 and report.get('initramfsSha256')==manifest.get('initramfsSha256'),
            'Wrong native/QEMU/private-image provenance')
    require(source.get('sourceImageSha256Before')==source.get('sourceImageSha256After')==ROOT_SHA256 and
            source.get('sourceImageUnchanged') is True and manifest.get('ordinaryUid')==1000 and
            manifest.get('desktopGeometryProbe')==[3840,2160] and
            manifest.get('mesaWaylandGpuProbe') is True and manifest.get('qtQuickSoftwareProbe') is False,
            'Wrong 4K hardware-only/read-only inputs')
    require(report.get('liveNvidiaLinuxUmdLoaded') is True and 'qtQuickSoftware=true' not in guest and
            f'kernelCpuCapacityVerified=true profile={version} cpuByteLimit=201326592 cpuSlots=512 fenceSlots=256\n' in guest,
            'Missing genuine runtime/exact capacity receipt')
    def one(key):
        found=[r for r in records if key in r]
        require(len(found)==1 and found[0][key] is True,'Missing/failed/duplicate '+key)
        return found[0]
    profile=one('openGlProfileNegotiated')
    require(profile.get('version')==version and profile.get('gpuMappedByteLimit')==1024*1024*1024 and
            profile.get('reportedGpuUsageLimitBytes')==usage_limit and profile.get('cpuMappedByteLimit')==192*1024*1024 and
            profile.get('cpuApertureSlots')==512 and profile.get('kernelCapacityRequired') is True,'Wrong exact GPU budgets')
    require(clean.get('vendorGpuMappedByteLimit')==profile['gpuMappedByteLimit'] and
            clean.get('reportedGpuUsageLimitBytes')==profile['reportedGpuUsageLimitBytes'] and
            clean.get('vendorCpuMappedByteLimit')==profile['cpuMappedByteLimit'] and clean.get('vendorCpuSlotLimit')==512,
            'Cleanup/accounting guards disagree with negotiated profile')
    owner=one('ownedQemuReadonlyRootAttached')
    require(all(owner.get(k) is True for k in ('fileNodeReadonly','rawNodeReadonly','sourceWriteSharingDenied')),'Missing read-only owner policy')
    window=one('nativeSharedTexturePresenterOpened')
    require(window.get('width')==3840 and window.get('height')==2160 and window.get('sameAdapter') is True and
            window.get('cpuUpload') is False and window.get('windowVisible') is True,'Wrong hardware window/adapter')
    opened=one('nativeCompositorRingOpened')
    require(opened.get('slots')==3 and opened.get('sourceCacheLimit')==3 and opened.get('ordinaryCpuReadback') is False and
            opened.get('uiThreadOwned') is True,'Wrong bounded GPU ring')
    replies=re.findall(r'^HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=(\d+) audit=([01]) retries=(\d+) syncFileClosed=true eglSyncDestroyed=true guestCpuMap=false sourceCopyRetired=true displayVerified=false$',guest,re.M)
    require(len(replies)==12 and guest.count('HYPRLAND_HOST_QUEUE_RESULT ')==12 and
            guest.count('HYPRLAND_HOST_QUEUE_END frames=12 target=12 failed=0\n')==1,'Incomplete 12-frame GPU diagnostic')
    inputs=[r for r in records if 'nativeCompositorRingInput' in r]
    outputs=[r for r in records if 'nativeCompositorRingFrame' in r]
    presents=[r for r in records if 'nativeCompositorWindowSubmitted' in r]
    require(len(inputs)==len(outputs)==len(presents)==12,'Missing/duplicate native frame')
    identities=set();previous=0
    for n,(source_frame,output,present,reply) in enumerate(zip(inputs,outputs,presents,replies),1):
        audit=n in (1,12)
        require(all(r.get(k) is True for r,k in ((source_frame,'nativeCompositorRingInput'),(output,'nativeCompositorRingFrame'),(present,'nativeCompositorWindowSubmitted'))),'Failed native frame')
        require(source_frame.get('frame')==output.get('frame')==present.get('frame')==int(reply[0])==n and
                int(reply[1])==int(audit),'Mixed frame/audit order')
        target,observed,copied=(source_frame.get(k) for k in ('producerFenceTarget','producerFenceObserved','inputCopyFenceObserved'))
        require(all(type(v) is int and 0<v<(1<<64)-1 for v in (target,observed,copied)) and observed>=target and copied>=n,
                'Unretired producer/input copy')
        require(type(source_frame.get('sourceTicket')) is int and source_frame['sourceTicket']>0 and
                source_frame.get('sourceCopyRetired') is True and source_frame.get('sourceReturnedToCommon') is True and
                source_frame.get('cpuReadback') is False and source_frame.get('cpuUpload') is False,'Wrong source ownership/copy route')
        require(type(source_frame.get('slot')) is int and 0<=source_frame['slot']<3 and output.get('slot')==source_frame['slot'] and
                output.get('cpuReadback') is audit and output.get('cpuUpload') is False and output.get('displayVerified') is False,
                'Ordinary readback/upload or wrong staging slot')
        require(present.get('presentHresult')==present.get('countHresult')==0 and present.get('lastPresentCount')==n and
                present.get('cpuUpload') is False and present.get('displayVerified') is False,'Present failure/unsupported display claim')
        pid,chain,before,after,frequency=(present.get(k) for k in ('presentProcessId','presentSwapChain','presentCallQpcBefore','presentCallQpcAfter','qpcFrequency'))
        require(all(type(v) is int and v>0 for v in (pid,chain,before,after,frequency)) and before>previous and after>=before,
                'Wrong Present timing/identity')
        previous=before;identities.add((pid,chain,frequency))
        require(records.index(source_frame)<records.index(output) and records.index(present)<records.index(output),'Published frame before GPU handoff')
    require(len(identities)==1,'Mixed native swapchains')
    audits=[r for r in records if 'nativeCompositorRingAudited' in r]
    binary=[r for r in records if 'nativeCompositorBinaryCapture' in r]
    require(len(audits)==len(binary)==2 and set(frames)=={'compositor-copy-1.raw','compositor-copy-2.raw'},'Wrong full-resolution audit count')
    accepted=[]
    for n,(audit,file,frame) in enumerate(zip(audits,binary,(1,12)),1):
        name=f'compositor-copy-{n}.raw';pixels=frames[name]
        require(audit.get('nativeCompositorRingAudited') is True and file.get('nativeCompositorBinaryCapture') is True and
                audit.get('frame')==frame and audit.get('auditRound')==file.get('frame')==n and file.get('file')==name,'Wrong audit/source frame')
        require(all(r.get('width')==3840 and r.get('height')==2160 and r.get('fourcc')==875713112 for r in (audit,file)),
                'Wrong 4K storage layout')
        require(type(audit.get('copyFenceObserved')) is int and frame<=audit['copyFenceObserved']<(1<<64)-1,'Unretired audit GPU copy')
        require(type(pixels) is bytes and len(pixels)==file.get('bytes')==3840*2160*4,'Wrong/truncated 4K bytes')
        checksum=fnv(pixels)
        require(checksum==audit.get('fnv1a')==file.get('fnv1a'),'Native/file pixel digest mismatch')
        accepted.append(dict(frame=frame,width=3840,height=2160,bytes=len(pixels),rawSha256=hashlib.sha256(pixels).hexdigest(),rawFnv1a=f'{checksum:016x}'))
    release=one('nativeSharedTexturePresenterReleased');stopped=one('nativeCompositorRingStopped')
    require(all(release.get(k) is True for k in ('gpuWorkRetired','windowDestroyed','classUnregistered','frameLatencyHandleClosed')) and
            stopped.get('submitted')==stopped.get('presented')==12 and stopped.get('completedAudits')==2 and
            stopped.get('liveGpuResources')==0 and all(stopped.get(k) is True for k in ('workHandleClosed','inputEventClosed','uiThreadJoined')),
            'GPU ring/window resources not released')
    require(clean.get('completedCompositorFrames')==12 and clean.get('failedCompositorFrames')==0 and
            clean.get('compositorCaptureAttempts')==0,'Wrong/mixed compositor workload')
    # These diagnostic failures are retained as findings, never rewritten to
    # satisfy the stricter ordinary-session/lifecycle/desktop acceptance gates.
    qemu=report['ownedQemuControl'];observation=one('nativeCompositorRingDisplayObservation')
    return dict(schema=1,compositor4kReadbackVerified=True,framesSubmitted=12,auditCopies=2,ordinaryFramesWithoutCpuReadback=10,
        sourceCopyRetiredBeforeAcknowledgement=True,cpuUploadUsed=False,frames=accepted,
        reportedGpuUsageGuardExceeded=clean['peakReportedGpuUsageBytes']>clean['reportedGpuUsageLimitBytes'],
        diagnosticChildSignaled='NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL' in guest,
        deferredMappings=qemu['fenceMappingsReleasedAfterVmExit']+qemu['allocationMappingsReleasedAfterVmExit'],
        observedPresentCount=observation.get('statisticsPresentCount'),nativeCleanupVerified=clean.get('driverCleanupVerified') is True,
        fullScenePixelCorrectnessVerified=False,quickshellGpuVerified=False,normalOmarchySessionVerified=False,
        allFramesDisplayedVerified=False,desktopVerified=False,compositor4kVerified=False,nearNativePerformanceVerified=False)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    records=[json.loads(line) for line in (a.directory/'driver.host.log').read_text().splitlines() if line.startswith('{')]
    frames={f'compositor-copy-{n}.raw':(a.directory/f'compositor-copy-{n}.raw').read_bytes() for n in (1,2)}
    result=validate(json.loads((a.directory/'driver.json').read_text()),(a.directory/'driver.log').read_text(),records,frames,json.loads((a.directory/'source.json').read_text()))
    with a.output.open('x',encoding='utf-8',newline='\n') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps(result,indent=2))
