"""Audit owned native sync-file import/GPU ordering; never full EGL/desktop support."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_native_sync_lineage(report, guest, records):
    result=validate_kernel_aperture(report,guest,records,transport='authenticated-broker',contract_checks=583)
    require(exact(report,liveNvidiaLinuxUmdLoaded=True,realWslDxgForwarding=False,capturedPrivateFixturesUsed=False),
            'Wrong native runtime provenance')
    lines=guest.splitlines()
    def once(text):
        require(lines.count(text)==1,'Missing or duplicate sync-file lineage receipt: '+text)
        return lines.index(text)
    contracts=[l for l in lines if l.startswith('SYNC_LINEAGE_CONTRACT_PASS ')]
    require(len(contracts)==1,'Missing lineage kernel rejection/rollback contract')
    match=re.fullmatch(r'SYNC_LINEAGE_CONTRACT_PASS checks=(\d+) enabled=true rollback=true leaseOwnerIsolation=true foreignRejected=true gpu=false',contracts[0])
    require(match is not None and 33<=int(match[1])<=2048,'Incomplete lineage kernel contract')
    once('SYNC_LINEAGE_CONTRACT_UNPRIVILEGED deniedCreate=true deniedResolve=true')
    once('SYNC_FILE_CONTRACT_UNPRIVILEGED_CREATE denied=true')
    client=once('BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    starts=[l for l in lines if l.startswith('BROKER_ROUTE_START ')]
    require(len(starts)==1 and re.fullmatch(r'BROKER_ROUTE_START protocol=3 clientPid=[1-9]\d* clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false',starts[0]),
            'Wrong authenticated broker client')
    target=4294967367
    expected=[
        'SYNC_LINEAGE_BEGIN gpu=true kernel=true eglNativeFence=false desktop=false',
        f'SYNC_LINEAGE_GPU target={target} before=0 acknowledgementBefore=0 acknowledgementAfter=1 nativeWait=true independentGpuRelease=true cpuFenceWrites=false',
        'SYNC_LINEAGE_LIFETIME originalSourceReleased=true originalNativeFdClosed=true firstImportedAliasReleased=true completedSyncFileRetainsNative=true',
        'SYNC_LINEAGE_REJECT canceledStatus=-19 canceledImportErrno=19 foreignRejected=true nullOutputRejected=true',
        'SYNC_LINEAGE_COMPLETE created=3 signaled=2 errors=1 released=3 held=0 pending=0 gpuQueueWaits=1 gpuReleaseSignals=1 dependentGpuSignals=1 eglNativeFence=false desktop=false']
    require([l for l in lines if l.startswith('SYNC_LINEAGE_') and not l.startswith('SYNC_LINEAGE_CONTRACT')]==expected,
            'Incomplete or failed sync-file lineage workload')
    indices=[once(l) for l in expected]
    kernel=once('KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=3 signaled=2 errors=1 released=3 cleanup=true')
    require(client<lines.index(starts[0])<indices[0]<indices[1]<indices[2]<indices[3]<indices[4]<
            once('OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false')<kernel<
            once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true')<once('BRIDGE_RUNTIME_EXIT=0'),
            'Incorrect sync-file GPU/lifetime/cleanup order')
    require(not any(s in guest for s in ('SYNC_LINEAGE_CONTRACT_FAIL','SYNC_FILE_CONTRACT_FAIL','cleanup=false','Oops:','BUG:')), 'Kernel sync-file failure')
    def select(marker): return [r for r in records if marker in r]
    waits,satisfied=select('nativeGpuQueueWaitAccepted'),select('nativeGpuQueueWaitDependenciesSatisfied')
    require(len(waits)==len(satisfied)==3,'Missing native GPU wait dependencies')
    for n,(accepted,done) in enumerate(zip(waits,satisfied),1):
        require(exact(accepted,nativeGpuQueueWaitAccepted=True,index=n,objects=1,contextStream=False,firstTarget=target,cpuWait=False) and
                type(accepted.get('nativeCallMs')) is int and accepted['nativeCallMs']>=0 and
                exact(done,nativeGpuQueueWaitDependenciesSatisfied=True,index=n,objects=1,contextStream=False,queueRetirementVerified=False) and records.index(accepted)<records.index(done),
                'CPU wait or incorrect native GPU dependency')
    require(lines.count(f'LINUX_BRIDGE nativeGpuQueueWaitQueued=true objects=1 firstTarget={target} contextStream=0 cpuWait=false')==3,
            'Missing matching guest GPU wait calls')
    hardware=[]
    for key,marker in (('nativeHwQueueSignalReturns','nativeHwQueueSignalReturned'),
                       ('nativeHwQueueSignalAcceptances','nativeHwQueueSignalAccepted'),
                       ('nativeHwQueueSignalRetirements','nativeHwQueueSignalRetired')):
        actual=select(marker)
        require(actual==report[key] and len(actual)==1,'Unmatched dependent GPU signal')
        record=actual[0]; hardware.append(record)
        require(exact(record,**{marker:True}) and exact(record,flags=4,queues=1,target=1,observed=1 if marker.endswith('Retired') else 0),
                'Dependent GPU signal was not gated before release')
        require(exact(record,ntstatus=0) if marker.endswith('Returned') else
                exact(record,index=1,noGpuAccess=True,cpuValueWrittenByBridge=False),'CPU-written or rejected dependent signal')
    released,retired=select('nativeAsyncContextSignalAccepted'),select('nativeAsyncContextSignalRetired')
    require(len(released)==len(retired)==1,'Missing independent GPU release')
    require(exact(released[0],nativeAsyncContextSignalAccepted=True,index=1,target=target,cpuWait=False) and type(released[0].get('observed')) is int and
            0<=released[0]['observed']<=target and
            exact(retired[0],nativeAsyncContextSignalRetired=True,index=1,target=target,observed=target,noGpuAccess=True,cpuValueWrittenByBridge=False),
            'Wrong independent native GPU release')
    require(records.index(waits[-1])<records.index(hardware[1])<records.index(released[0])<records.index(hardware[2]) and
            records.index(released[0])<records.index(retired[0])<records.index(satisfied[0]),
            'Native GPU wait/signal/release order disagrees')
    lifecycle=[r for r in records if any(k in r for k in ('nativeSyncSharingNegotiated','nativeSyncHandleExported',
        'nativeSyncResourceInfoQueried','nativeSyncHandleImported','nativeSyncHandleClosed'))]
    expected_lifecycle=[{'nativeSyncSharingNegotiated':True,'handleLimit':16},
        {'nativeSyncHandleExported':True,'rawHandleExposed':False},
        {'nativeSyncResourceInfoQueried':True,'ntstatus':-1073741788},
        {'nativeSyncHandleImported':True,'gpuMapped':False},
        {'nativeSyncResourceInfoQueried':True,'ntstatus':-1073741788},
        {'nativeSyncHandleImported':True,'gpuMapped':False},
        {'nativeSyncHandleClosed':True},
        {'nativeSyncHandleExported':True,'rawHandleExposed':False},
        {'nativeSyncHandleClosed':True}]
    require(json.dumps(lifecycle,sort_keys=True)==json.dumps(expected_lifecycle,sort_keys=True),
            'Native reference not retained through completed import')
    clean=report['disconnectCleanup']
    require(exact(clean,acceptedHwQueueSignals=1,completedHwQueueSignals=1,pendingHwQueueSignals=0,
                  acceptedAsyncContextSignals=1,retiredAsyncContextSignals=1,pendingAsyncContextSignals=0,
                  acceptedGpuQueueWaits=3,satisfiedGpuQueueWaits=3,pendingGpuQueueWaits=0,
                  exportedSyncHandles=2,importedSyncObjects=2,closedSyncHandles=2,liveSharedSyncHandles=0,
                  exportedResourceHandles=0,liveSharedResourceHandles=0), 'Unbalanced native GPU/FD lifetimes')
    require(all(type(v) is int and v==0 for k,v in clean.items() if k.startswith('failed') or k.endswith('Timeouts')),
            'Native GPU failure or timeout')
    routes=[l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')]
    require(len(routes)==1,'Missing broker summary')
    match=re.fullmatch(r'BROKER_ROUTE_EXIT processExited=true retainedViews=(\d+) nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false',routes[0])
    require(match is not None,'Failed broker route')
    retained,calls,created,released_count=map(int,match.groups()); vm=report['ownedQemuControl']
    require(created==result['kernelFenceLeases']+result['kernelAllocationLeases'] and
            created==retained+released_count and released_count==lines.count('LINUX_BRIDGE kernelApertureRevoked=true') and
            retained==vm['fenceMappingsReleasedAfterVmExit']+vm['allocationMappingsReleasedAfterVmExit'] and
            result['retiredNativeCommands']<=calls<=10000,'Unbalanced broker lease lifetimes')
    require([l for l in lines if l.startswith('BROKER_ROUTE')]==[starts[0],routes[0],'BROKER_ROUTE_REAP exit=0'],'Unexpected broker lifecycle')
    require([l for l in lines if l.startswith('BROKER_OBJECT')]==[
        'BROKER_OBJECT_DRAIN producerExited=true retained=0',
        'BROKER_OBJECT_SUMMARY exported=2 published=2 resolved=18 reaped=2 closed=2 retained=0',
        'BROKER_OBJECT recoverableErrors=5'],'Incorrect native capability cleanup/errors')
    result.update(nativeSyncFileLineageVerified=True,syncFileImportVerified=True,nativeGpuWaitThroughSyncFileVerified=True,
                  completedSyncFileNativeLifetimeVerified=True,zeroTargetSyncFileVerified=True,canceledSyncFileImportRejected=True,
                  nativeGpuSignalTarget=target,syncFilesCreated=3,syncFilesReleased=3,unprivilegedGraphicsClientVerified=True,
                  clientUid=65534,brokerNativeCalls=calls,brokerProtocol=3,eglNativeFenceVerified=False,
                  gbmVerified=False,dmaBufVerified=False,desktopVerified=False,desktopStabilityVerified=False,
                  foreignSyncFileImportVerified=False,mergedSyncFileImportVerified=False,
                  multiClientGpuRoutingVerified=False,gpuSideCrossProcessWaitVerified=False,
                  requiredApiMatrixVerified=False,allNvidiaFeaturesVerified=False,
                  concurrentWindowsGuestWorkloadsVerified=False,nearNativePerformanceVerified=False)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True); parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); report=json.loads(args.report.read_text()); guest=args.report.with_suffix('.log').read_text()
    records=[json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    result=validate_native_sync_lineage(report,guest,records)
    with args.output.open('x',encoding='utf8') as stream: json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__': main()
