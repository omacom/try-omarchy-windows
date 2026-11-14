"""Audit real application buffer churn in the bounded 300-frame NVIDIA scene."""
import json
import re
from verify_sustained_uwsm_gpu import native_frame_sequence
from verify_sustained_uwsm_gpu import require, validate_content_frame, reclaimed_staging_admission
from verify_uwsm_gpu_session import session_metadata
from verify_uwsm_native_input_devices import input_discovery
from verify_uwsm_demand_staging import terminal_retirement
from verify_reclaimed_staging_input_response import delivery_metadata
from verify_qmp_input_response import GUEST_SOURCES, response_metadata
from verify_qmp_input_delivery import snapshot

RESIZE_SOURCE='d1d2de1cbbef7d9c29cf65bcce4cc194199d1d71f13c30bec20ca2d4d16bd6bf'
PLAN=dict(sizes=[[1440,960],[1600,1000],[1200,800]],phaseThresholds=[0,50,150],ordinaryHandoffsContinue=True)


def resize_metadata(guest, source):
    require(source['manifest'].get('applicationResizeProbe')==PLAN and
            source['manifest']['sources'].get('hyprland_startup_probe.c')==RESIZE_SOURCE,
            'Missing exact application-resize fixture')
    ready=snapshot(guest,'OMARCHY_GPU_APPLICATION_READY_CLIENTS_HEX')
    require(isinstance(ready,list) and len(ready)==1,'Missing unique original application')
    app=ready[0]
    rows=re.findall(r'^OMARCHY_GPU_RESIZE phase=(\d+) pid=(\d+) width=(\d+) height=(\d+) beforeQueued=(\d+) afterQueued=(\d+) beforeCommit=(\d+) observedCommit=(\d+) gpuProof=false$',guest,re.M)
    require(len(rows)==guest.count('OMARCHY_GPU_RESIZE phase=')==3,'Missing/duplicate resize phases')
    previous=app;proof=[]
    for phase,raw in enumerate(rows,1):
        number,pid,width,height,before,after,committed,observed=map(int,raw)
        require(number==phase and pid==app['pid'] and [width,height]==PLAN['sizes'][phase-1] and
                observed>=committed+3 and 0<=before<=after<300,'Wrong/unsettled resize witness')
        old=snapshot(guest,f'OMARCHY_GPU_RESIZE_{phase}_BEFORE_ACTIVE_HEX')
        new=snapshot(guest,f'OMARCHY_GPU_RESIZE_{phase}_AFTER_ACTIVE_HEX')
        for client in (old,new):
            require(client.get('pid')==app['pid'] and client.get('address')==app['address'] and
                    client.get('class')=='omarchy-gpu-terminal' and client.get('title')=='Omarchy terminal' and
                    client.get('mapped') is True and client.get('hidden') is False and client.get('floating') is True and
                    client.get('xwayland') is False,'Resize affected a different/invisible application')
        require(old.get('size')==previous.get('size') and old.get('at')==previous.get('at') and
                new.get('size')==[width,height] and new.get('at')==[(3840-width)//2,26+(2134-height)//2],
                'Actual resize geometry/centering is wrong')
        if phase==1:
            require(before==after==0 and guest.index('OMARCHY_GPU_RESIZE phase=1 ')<
                    guest.index('HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=1 '),'First GPU buffer precedes initial resize')
        else:
            require(PLAN['phaseThresholds'][phase-1]<=before<PLAN['phaseThresholds'][phase] if phase==2 else
                    PLAN['phaseThresholds'][phase-1]<=before<300,'Resize did not occur at its bounded stream phase')
            require(after>before and guest.index(f'HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame={before} ')<
                    guest.index(f'OMARCHY_GPU_RESIZE phase={phase} ')<
                    guest.index('HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=300 '),
                    'GPU handoffs did not continue across resize or final capture came first')
        proof.append(dict(phase=phase,pid=pid,size=[width,height],at=new['at'],beforeQueued=before,afterQueued=after,
                          compositorCommitsDuringResize=observed-committed))
        previous=new
    require(previous['size']==[1200,800] and previous['at']==[1320,693],'Final application layout not restored')
    return dict(sameNvidiaApplicationPid=app['pid'],phases=proof,ordinaryHandoffsContinued=True,
                physicalWindowResizeVerified=False,allFramesDisplayedVerified=False)


def resize_pixels(frames, reference):
    require(reference.get('plannedBeforeCapture') is True and reference.get('applicationBackgroundRgb')==[26,27,38] and
            reference.get('backgroundChannelTolerance')==4 and reference.get('firstWindowAt')==[1200,613] and
            reference.get('firstWindowSize')==[1440,960] and reference.get('restoredWindowAt')==[1320,693] and
            reference.get('restoredWindowSize')==[1200,800] and
            reference.get('wallpaperSourceJpegSha256')=='37746404ae9a4357442e520c34ea105780232ac3d08a9f289f00156e49f37c4e',
            'Wrong independent pre-capture pixel plan')
    coords=[(x,y) for x in (1208,2632) for y in range(650,1541,7)]+[(x,1565) for x in range(1250,2601,19)]
    require([(x,y) for x,y,*_ in reference['probes']]==coords,'Changed/duplicate resize pixel coverage')
    first,last=frames['compositor-copy-1.raw'],frames['compositor-copy-2.raw']
    require(len(first)==len(last)==33177600,'Wrong full 4K buffers')
    matched=0;errors=[];changes=[]
    for x,y,r,g,b in reference['probes']:
        offset=(y*3840+x)*4
        old=first[offset:offset+3][::-1];new=last[offset:offset+3][::-1]
        matched+=int(all(abs(a-e)<=4 for a,e in zip(old,(26,27,38))))
        errors.append(sum(abs(a-e) for a,e in zip(new,(r,g,b)))/3)
        changes.append(sum(abs(a-e) for a,e in zip(old,new))/3)
    require(matched>=len(coords)*0.98 and sum(errors)/len(errors)<3 and max(errors)<40 and
            sum(changes)/len(changes)>25,'Actual GPU pixels do not show the resized panel and restored wallpaper')
    return dict(firstResizedAndFinalRestoredGpuFrameVerified=True,independentProbes=len(coords),
                firstPanelBackgroundMatches=matched,restoredWallpaperMeanRgbError=sum(errors)/len(errors),
                meanChangedRgb=sum(changes)/len(changes),middleResizePixelsVerified=False)


def validate_resize_uwsm_gpu_capture(report, guest, records, frames, source, wallpaper_reference, owner_output, resize_reference):
    capture=validate_content_frame(report,guest,records,frames,source,wallpaper_reference)
    metadata=resize_metadata(guest,source)
    pixels=resize_pixels(frames,resize_reference)
    session=session_metadata(guest,source,capture['compositorPid'])
    devices=input_discovery(guest,source)
    sequence=native_frame_sequence(guest,records)
    staging=reclaimed_staging_admission(records,report['disconnectCleanup']['compositorBackpressure'])
    terminal=terminal_retirement(guest,records,capture['compositorPid'])
    delivery=delivery_metadata(guest,records,source,owner_output,guest_sources={**GUEST_SOURCES,'hyprland_startup_probe.c':RESIZE_SOURCE})
    response=response_metadata(guest,frames)
    return dict(capture=capture,resize=metadata,resizePixels=pixels,session=session,inputDiscovery=devices,
                frameSequence=sequence,staging=staging,terminal=terminal,inputDelivery=delivery,inputResponse=response,
                bounded300FrameApplicationResizeGpuVerified=True,hostWindowIntegrationVerified=False,
                normalLauncherVerified=False,fullDesktopAccepted=False,allFramesDisplayedVerified=False,
                nearNativePerformanceVerified=False)
