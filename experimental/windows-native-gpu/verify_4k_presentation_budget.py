"""Audit exact v10 presentation admission and clean 4K compositor shutdown.

This does not accept Quickshell, a normal desktop session or reliable scanout.
"""
import argparse
import json
from pathlib import Path
import re
from verify_4k_compositor_readback import validate as readback
from verify_hyprland_lifecycle_capture import require


def validate(report,guest,records,frames,source):
    require(source['manifest']['kernelCpuCapacityProfile']==10,'Wrong presentation capacity')
    checks=[r for r in records if 'nativePresentationMemoryGuard' in r]
    require(checks and all(r.get('nativePresentationMemoryGuard') is True and r.get('accepted') is True and
        r.get('limitBytes')==1073741824 and all(type(r.get(k)) is int and 0<=r[k]<=1073741824 for k in ('usageBytes','additionalBytes')) and
        r['usageBytes']+r['additionalBytes']<=1073741824 for r in checks),'Failed/missing/overflowed presentation admission')
    stages=[r.get('stage') for r in checks]
    prefix=['device-before','device-after']+['staging-before','staging-after']*3+['readback-before','readback-after','swapchain-before','swapchain-after']
    require(stages[:len(prefix)]==prefix and set(stages)<=set(prefix+['frame-before','frame-admitted','import-before','import-after']),
        'Missing/reordered/unknown resource guard')
    require(stages.count('frame-before')==stages.count('frame-admitted')==12 and
        1<=stages.count('import-before')==stages.count('import-after')<=3,'Wrong per-frame/import admission')
    pairs=[(r['stage'],r['additionalBytes']) for r in checks]
    require(all(33177600<=n<=64*1024*1024 if stage in ('staging-before','readback-before') else
        2*33177600<=n<=128*1024*1024 if stage=='swapchain-before' else n==0 for stage,n in pairs),
        'Missing resource heap estimates or double-charged import')
    require(records.index(checks[len(prefix)-1])<next(i for i,r in enumerate(records) if 'nativeCompositorRingOpened' in r),
        'Published initialized ring before GPU admission')
    for text in ('HYPRLAND_STARTUP_MAIN_RETURN exit=0 windowsPresentation=false',
        'NORMAL_ROOT_GRAPHICS_EXIT result=0 normalOmarchySession=false',
        'NORMAL_ROOT_GRAPHICS_STOP serviceResult=success exitCode=exited exitStatus=0',
        'BROKER_ROUTE_REAP exit=0','KERNEL_RENDER_EXIT unloaded=true nodeRemoved=true',
        'KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true',
        'KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true'):
        require(guest.count(text+'\n')==1,'Missing/duplicate normal shutdown receipt: '+text)
    require('NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL' not in guest and 'NORMAL_ROOT_BOOT_FAILURE' not in guest and
        'HYPRLAND_STARTUP_STOP' not in guest and
        'reboot: Power down' in guest,'Aborted/forced/incomplete OS shutdown')
    clean,vm=report['disconnectCleanup'],report['ownedQemuControl']
    require(report.get('hostBridgeExit')==0 and clean.get('driverCleanupVerified') is True and
        vm.get('ownedQemuExited') is True and vm.get('qemuExit')==0 and vm.get('qemuForcedStop') is False and
        vm.get('fenceControlFailed') is False,'Unclean host/VM teardown')
    require(all(type(v) is int and v==0 for k,v in clean.items()
        if k.startswith(('failed','live','pending','forced')) or k.endswith(('Timeouts','ReleasedAfterVmExit'))),
        'Native failure/pending/live/deferred work')
    for kind in ('fence','allocation','allocationSlot'):
        require(vm[kind+'MappingsCreated']>0 and vm[kind+'MappingsCreated']==vm[kind+'UnmapAcknowledgements'] and
            vm[kind+'MappingsReleasedAfterVmExit']==0,'Deferred/incomplete explicit mapping cleanup: '+kind)
    require(all(v==0 for k,v in vm.items() if k.startswith('live')),'VM retains GPU mappings')
    broker=re.findall(r'^BROKER_ROUTE_EXIT processExited=true retainedViews=0 nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false$',guest,re.M)
    require(len(broker)==1 and int(broker[0][0])>0 and int(broker[0][1])>0 and broker[0][1]==broker[0][2],
        'Broker views not explicitly released')
    captured=readback(report,guest,records,frames,source)
    require(not captured['reportedGpuUsageGuardExceeded'] and not captured['diagnosticChildSignaled'] and
        captured['deferredMappings']==0,'Readback contains failed lifecycle/usage gates')
    return dict(schema=1,presentationMemoryAdmissionVerified=True,clean4kCompositorShutdownVerified=True,
        presentationAdmissionObservations=len(checks),reportedGpuByteLimit=1073741824,
        peakReportedGpuUsageBytes=clean['peakReportedGpuUsageBytes'],
        completedNativeSubmissions=clean['completedNativeSubmissions'],
        explicitFenceUnmaps=vm['fenceUnmapAcknowledgements'],explicitAllocationUnmaps=vm['allocationUnmapAcknowledgements'],
        explicitAllocationSlotUnmaps=vm['allocationSlotUnmapAcknowledgements'],brokerViewsReleased=int(broker[0][2]),
        physicalGpuQuotaVerified=False,quickshellGpuVerified=False,desktopVerified=False,
        normalOmarchySessionVerified=False,allFramesDisplayedVerified=False,nearNativePerformanceVerified=False,readback=captured)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    records=[json.loads(line) for line in (a.directory/'driver.host.log').read_text().splitlines() if line.startswith('{')]
    frames={f'compositor-copy-{n}.raw':(a.directory/f'compositor-copy-{n}.raw').read_bytes() for n in (1,2)}
    result=validate(json.loads((a.directory/'driver.json').read_text()),(a.directory/'driver.log').read_text(),records,frames,json.loads((a.directory/'source.json').read_text()))
    with a.output.open('x',encoding='utf-8',newline='\n') as f:json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps(result,indent=2))
