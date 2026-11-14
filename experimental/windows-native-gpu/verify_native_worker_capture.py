"""Audit actual 4K guest texture copies in a separate Windows GPU process.

This verifies one ordinary guest graphics client and one Windows consumer.
Independent guest GPU clients, compositor integration and performance are open.
"""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact,require,validate_cleanup,validate_oracle_frames,verify as verify_4k


def validate_worker_receipts(report,guest,records,pixel_audit,*,present=False):
    require(type(present) is bool,'Presentation scope must be explicit')
    validate_cleanup(report,records)
    require(exact(pixel_audit,lab4kRenderingVerified=True,sharedHandoffVerified=True,full4kDiagnosticVerified=True,
                  guestDesktopAcceleratedByThisBackend=False,nativePerformanceMeasured=False,graphicsBackendProductReady=False),
            'Missing successful independently audited 4K workload')
    frames=pixel_audit['frames']; validate_oracle_frames(frames)
    rounds=re.findall(r'^GPU_TRIANGLE_ROUND round=(\d+) verifiedPixels=(\d+) trianglePixels=(\d+) backgroundPixels=(\d+) maxChannelError=(\d+) referenceHash=([0-9a-f]+) observedHash=([0-9a-f]+) fenceTarget=(\d+) fenceObserved=(\d+)$',guest,re.M)
    require(len(rounds)==2 and 'LINUX_RUNTIME_NVIDIA_UMD_STILL_LOADED=true' in guest,
            'Missing genuine guest shader/runtime proof')
    for n,(round_,frame) in enumerate(zip(rounds,frames),1):
        require(tuple(map(int,round_[:5]))==(n,8294400,2332800,5961600,frame['maximumChannelError']) and
                int(round_[5],16)==frame['referenceFnv1a'] and int(round_[6],16)==frame['fnv1a'] and
                int(round_[7])==n<=int(round_[8])<(1<<64)-1,
                'Guest shader pixels/fence differ from independent native capture')
    require(pixel_audit.get('dxgiPresentationVerified') is present and all(frame.get('backbufferBytesMatch') is present for frame in frames),
            'Wrong or incomplete presentation/pixel audit selection')
    lines=guest.splitlines()
    def once(line):
        require(lines.count(line)==1,'Missing or repeated native-worker guest receipt: '+line)
        return lines.index(line)
    contracts=[once(l) for l in (
        'OBJECT_CONTRACT_PASS checks=561 held=0 active=0 forgedRejected=true ownerIsolation=true dupLifetime=true reapRollback=true',
        'RESOURCE_FD_CONTRACT_PASS checks=15 held=0 active=0 typeIsolation=true ownerIsolation=true dupLifetime=true dmaBuf=false',
        'APERTURE_CONTRACT_PASS checks=581 activeLeases=0 activeCpuBytes=0',
        'KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false',
        'BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')]
    require(contracts==sorted(contracts),'Wrong kernel/broker prerequisite order')
    starts=[l for l in lines if l.startswith('BROKER_ROUTE_START ')]
    require(len(starts)==1 and re.fullmatch(r'BROKER_ROUTE_START protocol=3 clientPid=[1-9]\d* clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false',starts[0]),
            'Wrong broker identity, protocol or guest-client scope')
    ends=[l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')]
    require(len(ends)==1,'Missing broker exit')
    match=re.fullmatch(r'BROKER_ROUTE_EXIT processExited=true retainedViews=0 nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false',ends[0])
    require(match is not None,'Broker rejection or live CPU leases')
    calls,created,released=map(int,match.groups())
    require([l for l in lines if l.startswith('BROKER_ROUTE')]==[starts[0],ends[0],'BROKER_ROUTE_REAP exit=0'],
            'Extra or failed broker lifecycle')
    stopped=once('LINUX_BRIDGE processExitViewsRetained=true pagingFences=0 hwQueueFences=0 syncFences=0 allocationViews=0 release=kernel-process-exit')
    completed=once('GPU_NATIVE_METADATA_TEST_COMPLETE verified=true rounds=2')
    unloaded=once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true')
    require(contracts[-1]<lines.index(starts[0])<completed<stopped<lines.index(ends[0])<once('BROKER_ROUTE_REAP exit=0')<unloaded<once('BRIDGE_RUNTIME_EXIT=0'),
            'Wrong graphics/process/kernel cleanup order')
    leases=re.findall(r'^LINUX_BRIDGE kernelApertureLease=true kind=([12]) bytes=(\d+) cookie=(\d+)$',guest,re.M)
    require(len(leases)==guest.count('LINUX_BRIDGE kernelApertureLease=') and
            [int(row[2]) for row in leases]==sorted({int(row[2]) for row in leases}) and
            all(int(cookie)>0 and (int(size)==4096 if kind=='1' else 0<int(size)<=64*1024*1024 and int(size)%4096==0) for kind,size,cookie in leases),
            'Malformed, reused or oversized kernel CPU lease')
    vm=report['ownedQemuControl']
    require(0<created==released==len(leases)==lines.count('LINUX_BRIDGE kernelApertureRevoked=true') and
            sum(row[0]=='1' for row in leases)==vm['fenceMappingsCreated'] and
            sum(row[0]=='2' for row in leases)==vm['allocationMappingsCreated'] and
            0<report['disconnectCleanup']['completedNativeSubmissions']<=calls<=10000,
            'Broker/kernel/native mapping or request counts disagree')
    require([l for l in lines if l.startswith('LINUX_BRIDGE transport=')]==['LINUX_BRIDGE transport=authenticated-broker nativeWorkerClients=1'],
            'Wrong or mixed native transport')
    require([l for l in lines if l.startswith('BROKER_OBJECT')]==[
        'BROKER_OBJECT_DRAIN producerExited=true retained=0',
        'BROKER_OBJECT_SUMMARY exported=0 published=0 resolved=0 reaped=0 closed=0 retained=0'],
        'Unexpected guest capability-store workload')
    require('APERTURE_CONTRACT_FAIL' not in guest and 'cleanup=false' not in guest,'Kernel cleanup failed')
    workers=[r for r in records if any(k.startswith('nativeGpuWorker') for k in r)]
    kinds=('nativeGpuWorkerStarted','nativeGpuWorkerInitialized','nativeGpuWorkerConsumed','nativeGpuWorkerConsumed',
           'nativeGpuWorkerShutdown','nativeGpuWorkerExited')
    require(len(workers)==len(kinds) and all(r.get(k) is True for r,k in zip(workers,kinds)),
            'Missing, repeated, failed or reordered Windows worker lifecycle')
    worker=workers[0].get('pid'); coordinator=workers[0].get('coordinatorPid')
    require(type(worker) is int and type(coordinator) is int and worker>0 and coordinator>0 and worker!=coordinator and
            all(type(r.get('pid')) is int and r['pid']==worker for r in workers),
            'Windows GPU worker is not a distinct pinned process')
    require(exact(workers[0],privatePipes=True,explicitInheritance=True,guestChoosesHandle=False) and
            exact(workers[1],parentPidChosenByGuest=False,hresult=0,ntHandleClosedAfterOpen=True) and
            exact(workers[-2],resourcesReleased=True) and exact(workers[-1],exit=0,forced=False,failed=False),
            'Unsafe NT handle transfer, failed initialization or forced worker cleanup')
    copies=[r for r in records if r.get('nativeSharedTextureCopied') is True]
    handoffs=[r for r in records if r.get('nativeSharedTextureHandoff') is True]
    require(len(copies)==len(handoffs)==2,'Missing native frame copies/handoffs')
    opened=[r for r in records if r.get('nativeSharedTextureConsumerOpened') is True]
    released_consumers=[r for r in records if r.get('nativeSharedTextureConsumerReleased') is True]
    require(len(opened)==len(released_consumers)==1 and exact(opened[0],ntstatus=0,hresult=0,sharedNtHandleClosed=True) and
            records.index(workers[0])<records.index(opened[0])<records.index(workers[1]),
            'Missing child-owned GPU resource opening')
    for n,(reply,frame,copy,handoff) in enumerate(zip(workers[2:4],frames,copies,handoffs),2):
        require(exact(reply,sequence=n,hresult=0,fnv1a=frame['fnv1a']) and
                exact(copy,round=n-1,width=3840,height=2160,fnv1a=frame['fnv1a'],sourceReturnedToCommon=True,cpuUpload=False,presented=False) and
                records.index(workers[1])<records.index(copy)<records.index(reply)<records.index(handoff)<records.index(workers[-2]),
                'Wrong worker frame identity, pixels, GPU copy or handoff ordering')
    require(records.index(handoffs[-1])<records.index(workers[-2])<records.index(workers[-1])<records.index(released_consumers[0]),
            'Source consumer released before worker GPU context retired')
    cleanup=report['disconnectCleanup']
    require(exact(cleanup,startedNativeGpuWorkers=1,exitedNativeGpuWorkers=1,liveNativeGpuWorkers=0,
                  forcedNativeGpuWorkers=0,failedNativeGpuWorkers=0,transferredNativeTextureHandles=1,
                  vendorCpuMappedByteLimit=96*1024*1024,vendorCpuSlotLimit=128),
            'Unbalanced worker ownership or changed CPU quotas')
    presents=[r for r in records if 'nativeSharedTexturePresented' in r]
    if present:
        require(len(presents)==2 and all(exact(p,nativeSharedTexturePresented=True,presentProcessId=worker) for p in presents),
                'Presentation did not belong to the pinned GPU worker')
    else: require(not presents,'Presentation requires explicit worker audit selection')
    result=dict(pixel_audit)
    result.update(windowsGpuProcessTransferVerified=True,unprivileged4kRenderingVerified=True,
                  windowsGpuWorkerPid=worker,windowsCoordinatorPid=coordinator,guestClientUid=65534,
                  guestGraphicsClients=1,windowsTextureConsumers=1,brokerNativeCalls=calls,
                  kernelCpuViews=created,explicitCpuViewReleases=released,
                  independentGuestGpuClientsVerified=False,drmDeviceVerified=False,gbmVerified=False,
                  dmaBufVerified=False,desktopVerified=False,desktopStabilityVerified=False,
                  gpuSideCrossProcessWaitVerified=False,nearNativePerformanceVerified=False,
                  concurrentWindowsGuestWorkloadsVerified=False,requiredApiMatrixVerified=False,
                  allNvidiaFeaturesVerified=False,openGl4kVerified=False)
    result['separateWindowsPresentationVerified']=present
    return result


def verify(report_path,captures,oracle,*,present=False):
    pixel_audit=verify_4k(report_path,captures,oracle)
    report_path=Path(report_path)
    records=[json.loads(line) for line in report_path.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    return validate_worker_receipts(json.loads(report_path.read_text()),report_path.with_suffix('.log').read_text(),records,pixel_audit,present=present)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('report','capture-directory','oracle','output'): parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--present',action='store_true')
    args=parser.parse_args(); result=verify(args.report,args.capture_directory,args.oracle,present=args.present)
    with args.output.open('x',encoding='utf8') as stream: json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))


if __name__=='__main__': main()
