"""Audit GPU-driven Linux sync-file export; EGL import/desktop remain separate."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_native_sync_files(report, guest, records):
    result=validate_kernel_aperture(report,guest,records,transport='authenticated-broker',contract_checks=583)
    require(exact(report,liveNvidiaLinuxUmdLoaded=True,realWslDxgForwarding=False,capturedPrivateFixturesUsed=False),
            'Wrong native runtime provenance')
    lines=guest.splitlines()
    def once(text):
        require(lines.count(text)==1,'Missing or duplicate sync-file receipt: '+text)
        return lines.index(text)
    contract=[l for l in lines if l.startswith('SYNC_FILE_CONTRACT_PASS ')]
    require(len(contract)==1,'Missing sync-file rejection/rollback contract')
    match=re.fullmatch(r'SYNC_FILE_CONTRACT_PASS checks=(\d+) enabled=true rollback=true ownerIsolation=true gpu=false',contract[0])
    require(match is not None and 20<=int(match[1])<=2048,'Incomplete kernel sync-file contract')
    once('SYNC_FILE_CONTRACT_UNPRIVILEGED_CREATE denied=true')
    client=once('BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    starts=[l for l in lines if l.startswith('BROKER_ROUTE_START ')]
    require(len(starts)==1 and re.fullmatch(r'BROKER_ROUTE_START protocol=3 clientPid=[1-9]\d* clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false',starts[0]),
            'Wrong authenticated broker client')
    target=4294967354
    begin=once('NATIVE_SYNC_FILE_BEGIN gpu=true kernel=true eglNativeFence=false desktop=false')
    info=[l for l in lines if l.startswith('NATIVE_SYNC_FILE_INFO ')]
    require(len(info)==1 and re.fullmatch(r'NATIVE_SYNC_FILE_INFO fd=\d+ flags=1 ready=1 ioctl=0 fences=1 status=1 fenceStatus=1 driver=detached-driver timeline=signaled-timeline',info[0]),
            'Invalid standard sync-file poll/ioctl results')
    gpu=once(f'NATIVE_SYNC_FILE_GPU target={target} observed={target} pendingBeforeRelease=true recipientUid=65534 parentFdClosed=true childExit=0')
    revoke=once('NATIVE_SYNC_FILE_REVOKE status=-19 sourceRevoked=true abandonedFdClosed=true')
    end=once('NATIVE_SYNC_FILE_COMPLETE created=5 signaled=2 errors=3 released=5 held=0 pending=0 timeoutStatus=-110 eglNativeFence=false desktop=false')
    kernel=once('KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=5 signaled=2 errors=3 released=5 cleanup=true')
    require(client<lines.index(starts[0])<begin<lines.index(info[0])<gpu<revoke<end<
            once('OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false')<kernel<
            once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true')<once('BRIDGE_RUNTIME_EXIT=0'),
            'Invalid GPU/sync-file/process teardown order')
    require([l for l in lines if l.startswith('NATIVE_SYNC_FILE_')]==[lines[begin],info[0],lines[gpu],lines[revoke],lines[end]],
            'Unexpected or failed sync-file workload')
    require(not any(s in guest for s in ('SYNC_FILE_CONTRACT_FAIL','cleanup=false','Oops:','BUG:')), 'Kernel sync-file failure')
    clean=report['disconnectCleanup']
    for key,marker in (('nativeHwQueueSignalReturns','nativeHwQueueSignalReturned'),
                       ('nativeHwQueueSignalAcceptances','nativeHwQueueSignalAccepted'),
                       ('nativeHwQueueSignalRetirements','nativeHwQueueSignalRetired')):
        actual=[r for r in records if marker in r]
        require(actual==report[key] and len(actual)==1,'Missing or inconsistent native GPU signal')
        require(exact(actual[0],flags=4,queues=1,target=target) and type(actual[0].get('observed')) is int and
                0<=actual[0]['observed']<=target,'Wrong native signal target/value')
        if marker.endswith('Retired'): require(actual[0]['observed']==target,'GPU signal did not retire')
        if marker.endswith('Returned'): require(exact(actual[0],ntstatus=0),'Native signal rejected')
        else: require(exact(actual[0],index=1,noGpuAccess=True,cpuValueWrittenByBridge=False),'CPU-written or misidentified native signal')
    require(exact(clean,acceptedHwQueueSignals=1,completedHwQueueSignals=1,failedHwQueueSignals=0,pendingHwQueueSignals=0,
                  exportedSyncHandles=2,importedSyncObjects=1,closedSyncHandles=2,liveSharedSyncHandles=0,
                  exportedResourceHandles=0,liveSharedResourceHandles=0), 'Unexpected native signal/FD lifetime counts')
    route=[l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')]
    require(len(route)==1,'Missing broker lifetime summary')
    match=re.fullmatch(r'BROKER_ROUTE_EXIT processExited=true retainedViews=(\d+) nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false',route[0])
    require(match is not None,'Failed broker route')
    retained,calls,created,released=map(int,match.groups())
    vm=report['ownedQemuControl']
    require((retained,created,released)==(96,165,69) and result['retiredNativeCommands']<=calls<=10000 and
            created==result['kernelFenceLeases']+result['kernelAllocationLeases'] and
            released==lines.count('LINUX_BRIDGE kernelApertureRevoked=true') and
            retained==vm['fenceMappingsReleasedAfterVmExit']+vm['allocationMappingsReleasedAfterVmExit'],
            'Unbalanced broker/native kernel leases')
    require([l for l in lines if l.startswith('BROKER_ROUTE')]==[starts[0],route[0],'BROKER_ROUTE_REAP exit=0'],
            'Unexpected broker process lifecycle')
    require([l for l in lines if l.startswith('BROKER_OBJECT')]==[
        'BROKER_OBJECT_DRAIN producerExited=true retained=0',
        'BROKER_OBJECT_SUMMARY exported=2 published=2 resolved=6 reaped=2 closed=2 retained=0',
        'BROKER_OBJECT recoverableErrors=1'], 'Native capability lifetime mismatch')
    result.update(nativeGpuSyncFileExportVerified=True,standardSyncFilePollVerified=True,
                  crossProcessSyncFileTransferVerified=True,nativeGpuSignalTarget=target,
                  sourceRevocationErrorVerified=True,timeoutErrorVerified=True,boundedSyncFileCleanupVerified=True,
                  syncFilesCreated=5,syncFilesReleased=5,unprivilegedGraphicsClientVerified=True,
                  clientUid=65534,brokerNativeCalls=calls,brokerProtocol=3,
                  eglNativeFenceVerified=False,syncFileImportVerified=False,gbmVerified=False,
                  dmaBufVerified=False,desktopVerified=False,desktopStabilityVerified=False,
                  multiClientGpuRoutingVerified=False,gpuSideCrossProcessWaitVerified=False,
                  requiredApiMatrixVerified=False,allNvidiaFeaturesVerified=False,
                  concurrentWindowsGuestWorkloadsVerified=False,nearNativePerformanceVerified=False)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True); parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args(); report=json.loads(args.report.read_text()); guest=args.report.with_suffix('.log').read_text()
    records=[json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    result=validate_native_sync_files(report,guest,records)
    with args.output.open('x',encoding='utf8') as stream: json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))
if __name__=='__main__': main()
