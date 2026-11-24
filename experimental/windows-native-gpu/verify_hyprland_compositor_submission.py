"""Audit actual compositor handoff with frame-slot pacing; display proof remains separate."""
import argparse
import json
from pathlib import Path
import re
from verify_hyprland_compositor_capture import _validate_readback, require


def validate(report, guest, records, frames, expected_rgb):
    readback=_validate_readback(report,guest,records,frames,expected_rgb,retained_presenter=True)
    rows=lambda key:[row for row in records if key in row]
    opened,ready,presents,released=(rows(key) for key in ('nativeSharedTexturePresenterOpened',
        'nativeCompositorFrameSlotReady','nativeSharedTexturePresented','nativeSharedTexturePresenterReleased'))
    copied,capture_release=rows('nativeCompositorCaptured'),rows('nativeCompositorCaptureReleased')
    shutdown=rows('nativeCompositorShutdownStatistics')
    require(len(opened)==len(released)==len(shutdown)==1 and len(ready)==len(presents)==len(copied)==2,
        'Missing/duplicate paced compositor lifecycle')
    require(all(opened[0].get(key) is True for key in ('nativeSharedTexturePresenterOpened','compositorOutput',
        'frameLatencyWaitable','flipDiscard','windowVisible','sameAdapter')) and
        all(type(opened[0].get(key)) is int and opened[0][key]==value for key,value in
            dict(width=640,height=360,clientWidth=640,clientHeight=360,bufferCount=2,maximumFrameLatency=1).items()),
        'Wrong compositor frame-slot policy')
    require(all(released[0].get(key) is True for key in ('nativeSharedTexturePresenterReleased','gpuWorkRetired',
        'windowDestroyed','classUnregistered','frameLatencyHandleClosed')), 'Window or frame-slot handle not released')
    sequence=[opened[0],ready[0],copied[0],presents[0],ready[1],copied[1],presents[1],shutdown[0],released[0],capture_release[0]]
    require(all(records.index(a)<records.index(b) for a,b in zip(sequence,sequence[1:])), 'Paced lifecycle order mismatch')
    identity=None
    previous_after=0
    for index,(slot,row,frame) in enumerate(zip(ready,presents,readback['frames']),1):
        require(slot.get('nativeCompositorFrameSlotReady') is True and type(slot.get('frame')) is int and slot['frame']==index and
            type(slot.get('maximumFrameLatency')) is int and slot['maximumFrameLatency']==1 and
            type(slot.get('waitMilliseconds')) is int and 0<=slot['waitMilliseconds']<=5000,
            'Frame slot not ready before rendering')
        require(row.get('compositorOutput') is True and row.get('presentSubmissionAccepted') is True and
            row.get('displayProofRequired') is False and row.get('cpuUpload') is False and
            row.get('backBufferState')=='PRESENT' and row.get('windowVisible') is True,
            'Incorrect compositor submission scope')
        require(all(type(row.get(key)) is int and row[key]==value for key,value in
            dict(round=index,width=640,height=360,presentHresult=0,lastPresentCount=index,
                 presentQueueFenceTarget=index,dwmStatisticsFlushes=0).items()), 'Failed/blocked compositor submission')
        fields=('presentProcessId','presentSwapChain','presentCallQpcBefore','presentCallQpcAfter','qpcFrequency',
                'presentQueueFenceObserved','backBufferFnv1a')
        require(all(type(row.get(key)) is int for key in fields) and 0<row['presentProcessId']<1<<32 and
            0<row['presentSwapChain']<1<<64 and 0<row['qpcFrequency']<1<<63 and
            previous_after<row['presentCallQpcBefore']<=row['presentCallQpcAfter']<1<<63 and
            index<=row['presentQueueFenceObserved']<(1<<64)-1 and row['backBufferFnv1a']==int(frame['rawFnv1a'],16),
            'Invalid/unretired compositor identity, timing or pixels')
        current=tuple(row[key] for key in ('presentProcessId','presentSwapChain','qpcFrequency'))
        require(identity is None or current==identity,'Compositor presenter changed between frames')
        require((slot.get('presentProcessId'),slot.get('presentSwapChain'))==current[:2], 'Foreign frame-slot identity')
        identity=current; previous_after=row['presentCallQpcAfter']
    last=shutdown[0]
    require(last.get('nativeCompositorShutdownStatistics') is True and last.get('windowVisible') is True and
        last.get('diagnosticShutdownHoldMilliseconds')==1000 and
        (last.get('presentProcessId'),last.get('presentSwapChain'))==identity[:2], 'Wrong shutdown observation')
    interval=1000*(presents[1]['presentCallQpcBefore']-presents[0]['presentCallQpcBefore'])/identity[2]
    require(0<interval<1000,'Per-frame diagnostic hold remains in the handoff')
    return dict(schema=1,actualCompositorSubmissionVerified=True,frameSlotPacingVerified=True,
        perFrameDisplayStatisticsWaitUsed=False,perFrameDiagnosticHoldUsed=False,
        diagnosticInterPresentMilliseconds=interval,diagnosticShutdownHoldMilliseconds=1000,
        windowsPresentationVerified=False,normalLauncherIntegrationVerified=False,displayStabilityVerified=False,
        desktopVerified=False,nearNativePerformanceVerified=False,readback=readback)


def validate_display(report,guest,records,frames,expected_rgb):
    submission=validate(report,guest,records,frames,expected_rgb)
    presents=[row for row in records if 'nativeSharedTexturePresented' in row]
    ready=[row for row in records if 'nativeCompositorFrameSlotReady' in row]
    shutdown=next(row for row in records if 'nativeCompositorShutdownStatistics' in row)
    # The next frame-slot observation binds display count 1 to the first
    # actual copied frame. The final observation binds count 2 to the second.
    # Neither substitutes Present acceptance or GPU retirement for display.
    previous_sync=0
    observations=[]
    for index,(present,snapshot) in enumerate(zip(presents,(ready[1],shutdown)),1):
        require(type(snapshot.get('statisticsHresult')) is int and snapshot['statisticsHresult']==0 and
            type(snapshot.get('statisticsPresentCount')) is int and snapshot['statisticsPresentCount']==index and
            type(snapshot.get('syncQpc')) is int and
            max(previous_sync,present['presentCallQpcBefore']-1)<snapshot['syncQpc']<1<<63,
            'Missing/stale deferred display observation')
        previous_sync=snapshot['syncQpc']
        observations.append(dict(frame=index,statisticsPresentCount=index,syncQpc=previous_sync,
            observation='next-frame-slot' if index==1 else 'diagnostic-shutdown'))
    return dict(schema=1,actualCompositorDeferredDisplayVerified=True,
        instantDisplayStatisticsComplete=all(row.get('nativeSharedTexturePresented') is True for row in presents),
        normalLauncherIntegrationVerified=False,displayStabilityVerified=False,fullScenePixelCorrectnessVerified=False,
        desktopVerified=False,nearNativePerformanceVerified=False,observations=observations,submission=submission)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--expected-background-rgb',required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--display',action='store_true',help='Also require exact deferred display counts/timing for both frames')
    args=parser.parse_args()
    require(re.fullmatch('[0-9a-fA-F]{6}',args.expected_background_rgb) is not None,'Expected six RGB hex digits')
    report=json.loads(args.report.read_text()); guest=args.report.with_suffix('.log').read_text()
    records=[json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    frames={f'compositor-copy-{index}.raw':(args.report.parent/f'compositor-copy-{index}.raw').read_bytes() for index in (1,2)}
    verifier=validate_display if args.display else validate
    result=verifier(report,guest,records,frames,tuple(bytes.fromhex(args.expected_background_rgb)))
    with args.output.open('x',newline='\n') as stream:
        json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(result,indent=2))
