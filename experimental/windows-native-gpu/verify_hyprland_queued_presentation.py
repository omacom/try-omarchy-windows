"""Audit a bounded GPU staging ring; never infer a complete desktop or all-frame scanout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from verify_hyprland_lifecycle_capture import require, validate as lifecycle
from verify_hyprland_compositor_capture import fnv


def validate(report,guest,records,frames,expected_rgb,*,profile=4,expected_frames=12,background_regions=None,normal_root=False):
    require(type(expected_frames) is int and 2<=expected_frames<=1800,'Invalid expected frame count')
    total=expected_frames
    life=lifecycle(report,guest,profile=profile,normal_root=normal_root)
    regions=background_regions if background_regions is not None else ((0,640,128,160),(0,640,280,360))
    require(type(regions) is tuple and len(regions)>0,'Missing independent background regions')
    for index,region in enumerate(regions):
        require(type(region) is tuple and len(region)==4 and all(type(value) is int for value in region), 'Invalid reference region')
        left,right,low,high=region
        require(0<=left<right<=640 and 0<=low<high<=360,'Reference region outside image')
        require(all(right<=a or b<=left or high<=c or d<=low for a,b,c,d in regions[:index]),'Overlapping reference regions')
    require(type(expected_rgb) is tuple and len(expected_rgb)==3 and
            all(type(value) is int and 0<=value<=255 for value in expected_rgb),'Invalid independent background')
    require('HYPRLAND_HOST_QUEUE_FAILURE' not in guest,'Guest queue failed')
    replies=re.findall(r'^HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=(\d+) audit=([01]) retries=(\d+) syncFileClosed=true eglSyncDestroyed=true guestCpuMap=false sourceCopyRetired=true displayVerified=false$',guest,re.M)
    # stdout and the shim's stderr share one guest serial port. Preserve an
    # interleaved prefix on the final summary, while requiring every per-frame
    # reply as a complete line and exactly one exact successful end receipt.
    require(len(replies)==guest.count('HYPRLAND_HOST_QUEUE_RESULT ')==total and
            guest.count('HYPRLAND_HOST_QUEUE_END ')==1 and
            guest.count(f'HYPRLAND_HOST_QUEUE_END frames={total} target={total} failed=0\n')==1,'Incomplete queued workload')
    def one(key):
        found=[row for row in records if key in row]
        require(len(found)==1 and found[0][key] is True,'Missing/failed/duplicate '+key)
        return found[0]
    opened=one('nativeCompositorRingOpened')
    require(opened.get('slots')==opened.get('sourceCacheLimit')==3 and opened.get('uiThreadOwned') is True and
            opened.get('ordinaryCpuReadback') is False,'Wrong presentation topology')
    window=one('nativeSharedTexturePresenterOpened')
    require(window.get('sameAdapter') is True and window.get('windowVisible') is True and window.get('cpuUpload') is False and
            window.get('width')==640 and window.get('height')==360,'Wrong native window')
    inputs=[row for row in records if 'nativeCompositorRingInput' in row]
    presents=[row for row in records if 'nativeCompositorWindowSubmitted' in row]
    outputs=[row for row in records if 'nativeCompositorRingFrame' in row]
    require(len(inputs)==len(presents)==len(outputs)==total,'Missing/duplicate native frame')
    require(all(row.get(key) is True for key,rows in
            (('nativeCompositorRingInput',inputs),('nativeCompositorWindowSubmitted',presents),('nativeCompositorRingFrame',outputs))
            for row in rows),'Failed native frame receipt')
    identities=set(); sequence=[]
    for index,(source,present,output,reply) in enumerate(zip(inputs,presents,outputs,replies),1):
        audit=index in (1,total)
        require(int(reply[0])==index and int(reply[1])==int(audit),'Guest frame/audit order mismatch')
        require(source.get('frame')==present.get('frame')==output.get('frame')==index,'Native frame order mismatch')
        require(type(source.get('slot')) is int and 0<=source['slot']<3 and output.get('slot')==source['slot'],'Invalid staging slot')
        target,observed,copied=(source.get(key) for key in ('producerFenceTarget','producerFenceObserved','inputCopyFenceObserved'))
        require(all(type(value) is int and 0<value<(1<<64)-1 for value in (target,observed,copied)) and
                observed>=target and copied>=index,'Unretired producer/input copy')
        require(type(source.get('sourceTicket')) is int and source['sourceTicket']>0 and
                all(source.get(key) is True for key in ('sourceCopyRetired','sourceReturnedToCommon')) and
                all(source.get(key) is False for key in ('cpuReadback','cpuUpload')),'Wrong ingress ownership/copy path')
        require(output.get('cpuReadback') is audit and output.get('cpuUpload') is False and output.get('displayVerified') is False,
                'Ordinary frame readback or unsupported display claim')
        require(present.get('presentHresult')==present.get('countHresult')==0 and present.get('lastPresentCount')==index and
                present.get('cpuUpload') is False and present.get('displayVerified') is False,'Native Present not accepted')
        pid,chain,before,after,frequency=(present.get(key) for key in ('presentProcessId','presentSwapChain','presentCallQpcBefore','presentCallQpcAfter','qpcFrequency'))
        require(all(type(value) is int and value>0 for value in (pid,chain,before,after,frequency)) and after>=before,'Invalid Present identity/timing')
        identities.add((pid,chain,frequency)); sequence.append(before)
        require(records.index(source)<records.index(output) and records.index(present)<records.index(output),'Frame published before ingress/Present receipt')
    require(len(identities)==1 and all(a<b for a,b in zip(sequence,sequence[1:])),'Mixed/nonadvancing presentation sequence')
    slots=[row for row in records if 'nativeCompositorFrameSlotReady' in row]
    pid,chain,_=next(iter(identities))
    require(len(slots)==total and all(row.get('nativeCompositorFrameSlotReady') is True and row.get('frame')==index and
            row.get('maximumFrameLatency')==1 and row.get('presentProcessId')==pid and row.get('presentSwapChain')==chain
            for index,row in enumerate(slots,1)),'Missing/foreign/unbounded frame-slot wait')
    audits=[row for row in records if 'nativeCompositorRingAudited' in row]
    binary=[row for row in records if 'nativeCompositorBinaryCapture' in row]
    require(len(audits)==len(binary)==2 and set(frames)=={'compositor-copy-1.raw','compositor-copy-2.raw'},'Audit copy count mismatch')
    require(all(row.get(key) is True for key,rows in (('nativeCompositorRingAudited',audits),('nativeCompositorBinaryCapture',binary))
            for row in rows),'Failed audit receipt')
    checked=[]
    for round_,(audit,file,frame) in enumerate(zip(audits,binary,(1,total)),1):
        name=f'compositor-copy-{round_}.raw'; pixels=frames[name]
        require(audit.get('frame')==frame and audit.get('auditRound')==file.get('frame')==round_ and file.get('file')==name,
                'Audit frame order mismatch')
        require(all(row.get('width')==640 and row.get('height')==360 and row.get('fourcc')==875713112 for row in (audit,file)),
                'Wrong audit storage layout')
        require(type(audit.get('copyFenceObserved')) is int and frame<=audit['copyFenceObserved']<(1<<64)-1,'Audit GPU copy unretired')
        require(type(pixels) is bytes and len(pixels)==file.get('bytes')==921600,'Wrong audit byte count')
        checksum=fnv(pixels)
        require(checksum==audit.get('fnv1a')==file.get('fnv1a'),'Audit native/file digest mismatch')
        known=0
        # Late startup warning banners extend beyond the earlier two-capture
        # test's top 32 rows. These conservative regions exclude the dynamic
        # first 128 rows and cursor. Full-scene correctness remains false.
        for left,right,low,high in regions:
            for y in range(low,high):
                for x in range(left,right):
                    offset=(y*640+x)*4
                    require((pixels[offset+2],pixels[offset+1],pixels[offset])==expected_rgb,'Independent background mismatch')
                    known+=1
        checked.append(dict(frame=frame,rawSha256=hashlib.sha256(pixels).hexdigest(),rawFnv1a=f'{checksum:016x}',knownBackgroundPixelsVerified=known))
    released=one('nativeSharedTexturePresenterReleased')
    require(all(released.get(key) is True for key in ('gpuWorkRetired','windowDestroyed','classUnregistered','frameLatencyHandleClosed')),
            'Native window resources not released')
    stopped=one('nativeCompositorRingStopped')
    require(stopped.get('submitted')==stopped.get('presented')==total and stopped.get('completedAudits')==2 and
            all(type(stopped.get(key)) is int and 1<=stopped[key]<=3 for key in ('peakQueued','peakSources')) and
            all(stopped.get(key) is True for key in ('workHandleClosed','inputEventClosed','uiThreadJoined')) and
            stopped.get('liveGpuResources')==0,'Ring did not retire and close')
    clean=report['disconnectCleanup']
    require(clean.get('completedCompositorFrames')==total and clean.get('failedCompositorFrames')==0 and
            clean.get('compositorCaptureAttempts')==0,'Mixed/failed compositor path')
    observation=one('nativeCompositorRingDisplayObservation')
    last_display=(observation.get('statisticsHresult')==0 and observation.get('statisticsPresentCount')==total and
                  observation.get('presentProcessId')==pid and observation.get('presentSwapChain')==chain and
                  type(observation.get('syncQpc')) is int and observation['syncQpc']>=sequence[-1] and
                  observation.get('windowVisible') is True)
    return dict(schema=1,actualCompositorQueuedSubmissionVerified=True,framesSubmitted=total,ordinaryFramesWithoutCpuReadback=total-2,
        auditCopies=2,sourceCopyRetiredBeforeAcknowledgement=True,lastFrameDeferredDisplayVerified=last_display,
        allFramesDisplayedVerified=False,normalLauncherIntegrationVerified=False,sustainedRenderingVerified=False,
        fullScenePixelCorrectnessVerified=False,desktopVerified=False,compositor4kVerified=False,nearNativePerformanceVerified=False,
        lifecycle=life,frames=checked)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--expected-background-rgb',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    require(re.fullmatch('[0-9a-fA-F]{6}',args.expected_background_rgb) is not None,'Expected six RGB hex digits')
    records=[json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    frames={f'compositor-copy-{i}.raw':(args.report.parent/f'compositor-copy-{i}.raw').read_bytes() for i in (1,2)}
    result=validate(json.loads(args.report.read_text()),args.report.with_suffix('.log').read_text(),records,frames,tuple(bytes.fromhex(args.expected_background_rgb)))
    with args.output.open('x',newline='\n') as stream: json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(result,indent=2))
