"""Audit actual guest D3D12 shared-fence lifetime and future GPU queue ordering."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require, validate_cleanup
from verify_opengl_capture import checksum


def validate_gpu_timeline(report, guest, records):
    validate_cleanup(report, records)
    require(exact(report, liveNvidiaLinuxUmdLoaded=True, realWslDxgForwarding=False,
                  capturedPrivateFixturesUsed=False, d3d12DeviceHresult='00000000',
                  d3d12CopyQueueHresult='00000000'), 'Not a genuine successful guest D3D12 runtime')
    lines = guest.splitlines()

    def once(line):
        require(lines.count(line) == 1, 'Missing or duplicate timeline receipt: ' + line)
        return lines.index(line)

    start = once('GPU_TIMELINE_BEGIN nativeLinuxRuntime=true cpuFenceWrites=false desktop=false')
    release = once('GPU_TIMELINE_SOURCE_RELEASED original=true exportedFd=true importedAliasLive=true')
    gated = once('GPU_TIMELINE_GATED source=4 acknowledgement=0 expectedSource=4 expectedAcknowledgement=0')
    ordered = once('GPU_TIMELINE_ORDERING_VERIFIED futureWait=5 gatedBeforeRelease=true dependentSignalCompleted=true cpuFenceWrites=false')
    end = once('GPU_TIMELINE_COMPLETE gpuSignals=6 gpuQueueWaits=1 originalAndFdReleased=true cleanup=true desktop=false')
    copied = once('GPU_COPY_TEST_COMPLETE verified=true bytes=65536 rounds=2')
    objects = once('KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true')
    module = once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true')
    exited = once('BRIDGE_RUNTIME_EXIT=0')
    require(start < release < gated < ordered < end < copied < objects < module < exited, 'Incorrect timeline lifetime/order')
    once('OBJECT_CONTRACT_PASS checks=561 held=0 active=0 forgedRejected=true ownerIsolation=true dupLifetime=true reapRollback=true')
    once('APERTURE_CONTRACT_PASS checks=581 activeLeases=0 activeCpuBytes=0')
    once('KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false')
    stages = [line for line in lines if line.startswith('GPU_TIMELINE_API ')]
    expected_stages = ['SharedFence','Export','Import'] + ['QueueSignal']*4 + [
        'ReleasingQueue','AcknowledgementFence','FutureQueueWait','DependentSignal','ReleaseFromSecondQueue']
    require(stages == [f'GPU_TIMELINE_API stage={stage} status=00000000' for stage in expected_stages], 'Failed/missing D3D12 API receipt')
    require(lines.index(stages[2]) < release < lines.index(stages[3]) and
            lines.index(stages[-3]) < lines.index(stages[-2]) < gated < lines.index(stages[-1]) < ordered,
            'Wait must return and gate subsequent work before second-queue release')
    observed = [line for line in lines if line.startswith('GPU_TIMELINE_OBSERVED ')]
    require(observed == [f'GPU_TIMELINE_OBSERVED target={v} observed={v}' for v in (1,2,3,4,5,1)], 'Wrong completed timeline values')
    require(not any(marker in guest for marker in ('GPU_TIMELINE_TIMEOUT','OBJECT_CONTRACT_FAIL','GPU_COPY_TIMEOUT','GPU_COPY_MISMATCH')), 'Guest diagnostic failure')
    exported = [line for line in lines if line.startswith('LINUX_BRIDGE kernelObjectExported=')]
    reaped = [line for line in lines if line.startswith('LINUX_BRIDGE kernelObjectReaped=')]
    require(len(exported) == len(reaped) == 1, 'Wrong guest FD ownership receipt count')
    export = re.fullmatch(r'LINUX_BRIDGE kernelObjectExported=true kind=sync ticket=(\d+) cookie=(\d+)', exported[0])
    require(export is not None and int(export[1]) > 0 and int(export[2]) > 0 and
            reaped[0] == 'LINUX_BRIDGE kernelObjectReaped=true ticket=' + export[1], 'Mismatched kernel object identity')
    imported = once('LINUX_BRIDGE nativeSyncFdImported=true gpuMapped=0')
    require(lines.index(exported[0]) < imported < lines.index(reaped[0]) < release, 'FD not imported and reaped before signals')
    once('LINUX_BRIDGE nativeSyncFdImportInput flags=1155 affinity=0 outputFieldsIgnored=true')
    once('LINUX_BRIDGE nativeGpuQueueWaitQueued=true objects=1 firstTarget=5 contextStream=1 cpuWait=false')

    def select(marker):
        return [r for r in records if marker in r]

    lifecycle = [r for r in records if any(k in r for k in ('nativeSyncSharingNegotiated','nativeSyncHandleExported',
        'nativeSyncResourceInfoQueried','nativeSyncHandleImported','nativeSyncHandleClosed'))]
    expected_lifecycle = [
        {'nativeSyncSharingNegotiated':True,'handleLimit':16},
        {'nativeSyncHandleExported':True,'rawHandleExposed':False},
        {'nativeSyncResourceInfoQueried':True,'ntstatus':-1073741788},
        {'nativeSyncHandleImported':True,'gpuMapped':False}, {'nativeSyncHandleClosed':True}]
    require(json.dumps(lifecycle,sort_keys=True) == json.dumps(expected_lifecycle,sort_keys=True), 'Native export/type query/import/close mismatch')
    signals = select('nativeAsyncContextSignalAccepted')
    retired = select('nativeAsyncContextSignalRetired')
    require(len(signals) == len(retired) == 7, 'Incomplete nonblocking signal retirement')
    for n, (accepted, done) in enumerate(zip(signals, retired), 1):
        target = (1,2,3,4,1,1,2)[n-1]
        require(exact(accepted,nativeAsyncContextSignalAccepted=True,index=n,target=target,cpuWait=False) and
                type(accepted.get('observed')) is int and 0 <= accepted['observed'] <= target and
                exact(done,nativeAsyncContextSignalRetired=True,index=n,target=target,noGpuAccess=True,cpuValueWrittenByBridge=False) and
                type(done.get('observed')) is int and target <= done['observed'] < 2**64-1 and
                records.index(accepted) < records.index(done), 'Invalid nonblocking signal completion')
    require(signals[4]['observed'] == 0, 'Dependent GPU signal did not remain pending')
    waits, satisfied = select('nativeGpuQueueWaitAccepted'), select('nativeGpuQueueWaitDependenciesSatisfied')
    require(len(waits) == len(satisfied) == 1 and
            exact(waits[0],nativeGpuQueueWaitAccepted=True,index=1,objects=1,contextStream=True,firstTarget=5,cpuWait=False) and
            type(waits[0].get('nativeCallMs')) is int and waits[0]['nativeCallMs'] >= 0 and
            exact(satisfied[0],nativeGpuQueueWaitDependenciesSatisfied=True,index=1,objects=1,contextStream=True,queueRetirementVerified=False),
            'Wrong native GPU wait evidence')
    hardware, hardware_done = select('nativeHwQueueSignalAccepted'), select('nativeHwQueueSignalRetired')
    require(len(hardware) == len(hardware_done) == 1 and
            exact(hardware[0],nativeHwQueueSignalAccepted=True,index=1,flags=4,queues=1,target=5,noGpuAccess=True,cpuValueWrittenByBridge=False) and
            exact(hardware_done[0],nativeHwQueueSignalRetired=True,index=1,flags=4,queues=1,target=5,observed=5,noGpuAccess=True,cpuValueWrittenByBridge=False),
            'Missing actual second-queue GPU release')
    require(records.index(lifecycle[-1]) < records.index(signals[0]) < records.index(retired[3]) <
            records.index(waits[0]) < records.index(signals[4]) < records.index(hardware[0]) <
            records.index(hardware_done[0]) < records.index(retired[4]) < records.index(satisfied[0]),
            'Native signal, wait, release and completion ordering disagree')
    submissions, completions = select('nativeAsyncSubmissionAccepted'), select('nativeAsyncSubmissionRetired')
    require(len(submissions) == len(completions) == 7, 'Missing actual GPU command retirement')
    for n, (accepted, done) in enumerate(zip(submissions,completions),1):
        require(exact(accepted,nativeAsyncSubmissionAccepted=True,index=n) and exact(done,nativeAsyncSubmissionRetired=True,index=n) and
                all(type(done.get(k)) is int and done[k] == accepted.get(k) for k in ('target','bytes','privateBytes')) and
                type(done.get('observed')) is int and done['target'] <= done['observed'] < 2**64-1,
                'Unmatched native command completion')
    copies = [line for line in lines if line.startswith('GPU_COPY_ROUND ')]
    require(len(copies) == 2, 'Missing copy readbacks')
    for n,line in enumerate(copies,1):
        expected = checksum(bytes((i*37+(i>>8)*11+n*73)&255 for i in range(65536)))
        require(line == f'GPU_COPY_ROUND round={n} verifiedBytes=65536 expectedHash={expected} observedHash={expected} fenceTarget={n} fenceObserved={n}',
                'GPU copy readback differs from the independent byte pattern')
    cleanup = report['disconnectCleanup']
    require(exact(cleanup,exportedSyncHandles=1,importedSyncObjects=1,closedSyncHandles=1,
                  acceptedAsyncContextSignals=7,retiredAsyncContextSignals=7,pendingAsyncContextSignals=0,
                  acceptedHwQueueSignals=1,completedHwQueueSignals=1,pendingHwQueueSignals=0,
                  acceptedGpuQueueWaits=1,satisfiedGpuQueueWaits=1,pendingGpuQueueWaits=0,
                  acceptedAsyncSubmissions=7,completedNativeSubmissions=7), 'Unbalanced native work counters')
    require(all(type(v) is int and v == 0 for k,v in cleanup.items() if k.startswith('failed') or k.endswith('Timeouts')), 'Native work failure')
    return {'schema':1,'guestGpuTimelineVerified':True,'futureGpuQueueOrderingVerified':True,
            'sourceAndFdClosedBeforeSignals':True,'helperGpuSignals':6,'totalRetiredGpuSignals':8,
            'futureQueueWaits':1,'retiredNativeCommands':7,'copyBytesPerRound':65536,'copyRounds':2,
            'kernelObjectContractChecks':561,'kernelMemoryContractChecks':581,'kernelModuleUnloaded':True,
            'gpuAccessibleSharedFenceVerified':False,'mesaSemaphoreFdVerified':False,'syncFileVerified':False,
            'desktopVerified':False,'nearNativePerformanceVerified':False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    result = validate_gpu_timeline(report,guest,records)
    with args.output.open('x',encoding='utf-8') as stream:
        json.dump(result,stream,indent=2); stream.write('\n')
    print(json.dumps(result,indent=2))


if __name__ == '__main__':
    main()
