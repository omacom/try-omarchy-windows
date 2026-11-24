"""Audit actual, opt-in EGL callbacks and native GPU ordering; never a desktop."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_egl_native_sync(report, guest, records, *, enabled=True):
    require(type(enabled) is bool, 'Explicit EGL feature profile required')
    result = validate_kernel_aperture(report, guest, records,
        transport='authenticated-broker', contract_checks=583 if enabled else 581)
    require(exact(report, liveNvidiaLinuxUmdLoaded=True, realWslDxgForwarding=False,
        capturedPrivateFixturesUsed=False), 'Wrong runtime provenance')
    lines = guest.splitlines()

    def once(text):
        require(lines.count(text) == 1, 'Missing or duplicate receipt: ' + text)
        return lines.index(text)

    def select(marker):
        return [r for r in records if marker in r]

    def position(record):
        # Several native import/close receipts are identical dictionaries.
        return next(i for i, r in enumerate(records) if r is record)

    client = once('BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    starts = [l for l in lines if l.startswith('BROKER_ROUTE_START ')]
    require(len(starts) == 1 and re.fullmatch(r'BROKER_ROUTE_START protocol=3 clientPid=[1-9]\d* clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false', starts[0]), 'Wrong broker client')
    require(client < lines.index(starts[0]), 'Broker started before privilege drop')
    require(not any(s in guest for s in ('EGL_NATIVE_SYNC_FAILURE', 'SYNC_LINEAGE_CONTRACT_FAIL',
        'SYNC_FILE_CONTRACT_FAIL', 'Oops:', 'BUG:', 'cleanup=false')), 'EGL or kernel failure')
    clean = report['disconnectCleanup']
    require(all(type(v) is int and v == 0 for k, v in clean.items()
        if k.startswith('failed') or k.endswith('Timeouts')), 'Native failure or timeout')
    egl = [l for l in lines if l.startswith('EGL_NATIVE_SYNC_')]
    if not enabled:
        require(egl == ['EGL_NATIVE_SYNC_DISABLED extensionAbsent=true desktop=false'], 'Disabled extension advertised')
        kernel = once('KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=0 signaled=0 errors=0 released=0 cleanup=true')
        require(not select('nativeSyncHandleExported') and not select('nativeSyncHandleImported'), 'Disabled native FD path used')
    else:
        once('SYNC_LINEAGE_CONTRACT_UNPRIVILEGED deniedCreate=true deniedResolve=true')
        once('SYNC_FILE_CONTRACT_UNPRIVILEGED_CREATE denied=true')
        contracts = [l for l in lines if l.startswith('SYNC_LINEAGE_CONTRACT_PASS ')]
        require(len(contracts) == 1 and re.fullmatch(r'SYNC_LINEAGE_CONTRACT_PASS checks=(?:[3-9]\d|[1-9]\d{2,3}) enabled=true rollback=true leaseOwnerIsolation=true foreignRejected=true gpu=false', contracts[0]), 'Missing lineage isolation contract')
        target = 4294967387
        expected = [
            'EGL_NATIVE_SYNC_BEGIN callbacks=true nativeGpu=true kernel=true desktop=false',
            'EGL_NATIVE_SYNC_BASELINE created=4 signaled=4 released=4 held=0 pending=0',
            'EGL_NATIVE_SYNC_EXPORT create=true duplicateFds=true cloexec=true inputOwnership=EGL sourceSyncDestroyed=true importedComplete=true',
            f'EGL_NATIVE_SYNC_GPU target={target} pendingClientWait=timeout pendingStatus=unsignaled gpuWait=true dependentEglExportBefore=pending dependentEglExportAfter=signaled acknowledgementBefore=0 acknowledgementAfter=1 independentGpuRelease=true completedClientWait=satisfied completedStatus=signaled cpuFenceWrites=false',
            'EGL_NATIVE_SYNC_LIFETIME originalNativeFdClosed=true originalSourceReleased=true firstEglSyncDestroyed=true completedEglReimport=true zeroTargetImport=true',
            'EGL_NATIVE_SYNC_RESIDENCY evict=true makeResident=true nativeResource=true forcedPhysicalPageOut=false',
            'EGL_NATIVE_SYNC_REJECT foreignError=badAttribute failedInputFdRetained=true canceledStatus=-19 canceledClientWait=timeout canceledEglStatus=unsignaled canceledReimport=badAttribute',
            'EGL_NATIVE_SYNC_COUNTS created=9 signaled=8 errors=1 released=9 held=0 pending=0 mesaBatchFencesIncluded=true',
            'EGL_NATIVE_SYNC_COMPLETE externalCases=5 callbacksVerified=true generalInterop=false desktop=false']
        require(egl == expected, 'Incomplete EGL callback workload')
        indices = [once(l) for l in expected]
        require(indices == sorted(indices), 'Reordered EGL workload')
        kernel = once('KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=10 signaled=9 errors=1 released=10 cleanup=true')
        waits, satisfied = select('nativeGpuQueueWaitAccepted'), select('nativeGpuQueueWaitDependenciesSatisfied')
        require(len(waits) == len(satisfied) == 9, 'Missing native GPU dependencies')
        for n, (accepted, done) in enumerate(zip(waits, satisfied), 1):
            value = 1 if n <= 3 else 2 if n <= 6 else target
            require(exact(accepted, nativeGpuQueueWaitAccepted=True, index=n, objects=1,
                contextStream=False, firstTarget=value, cpuWait=False) and
                type(accepted.get('nativeCallMs')) is int and accepted['nativeCallMs'] >= 0 and
                exact(done, nativeGpuQueueWaitDependenciesSatisfied=True, index=n, objects=1,
                contextStream=False, queueRetirementVerified=False) and position(accepted) < position(done),
                'Wrong GPU dependency or CPU wait')
        require(lines.count(f'LINUX_BRIDGE nativeGpuQueueWaitQueued=true objects=1 firstTarget={target} contextStream=0 cpuWait=false') == 3, 'Missing guest future dependency')
        statuses = select('nativeContextSignalStatus')
        accepted, retired = select('nativeAsyncContextSignalAccepted'), select('nativeAsyncContextSignalRetired')
        require(len(statuses) == len(accepted) == len(retired) == 3, 'Missing residency/release signal')
        for n, (state, begin, end) in enumerate(zip(statuses, accepted, retired), 1):
            value = n if n <= 2 else target
            require(exact(state, nativeContextSignalStatus=True, flags=4, target=value, ntstatus=0,
                contextFlags=8 if n <= 2 else 16, synchronizationOnly=n <= 2) and
                exact(begin, nativeAsyncContextSignalAccepted=True, index=n, target=value, cpuWait=False) and
                type(begin.get('observed')) is int and 0 <= begin['observed'] <= value and
                exact(end, nativeAsyncContextSignalRetired=True, index=n, target=value, observed=value,
                noGpuAccess=True, cpuValueWrittenByBridge=False) and
                position(state) < position(begin) < position(end), 'Wrong native signal ownership or completion')
        hardware = []
        for key, marker in (('nativeHwQueueSignalReturns', 'nativeHwQueueSignalReturned'),
                            ('nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalAccepted'),
                            ('nativeHwQueueSignalRetirements', 'nativeHwQueueSignalRetired')):
            actual = select(marker)
            require(actual == report[key] and len(actual) == 8, 'Unmatched hardware queue signal')
            for n, record in enumerate(actual, 1):
                value = n if n <= 6 else 1 if n == 7 else 7
                require(exact(record, **{marker: True}, flags=4, queues=1, target=value) and
                    type(record.get('observed')) is int and 0 <= record['observed'] <= value,
                    'Wrong hardware signal')
                require(exact(record, ntstatus=0) if marker.endswith('Returned') else
                    exact(record, index=n, noGpuAccess=True, cpuValueWrittenByBridge=False), 'CPU-written signal')
                if marker.endswith('Retired'):
                    require(record['observed'] == value, 'Signal did not retire')
            hardware.append(actual)
        for n in (5, 6):  # Mesa's exported EGL fence, then the independent SDK acknowledgement.
            begin, end = hardware[1][n], hardware[2][n]
            require(begin['observed'] < begin['target'] and
                position(waits[-1]) < position(begin) < position(accepted[-1]) < position(end),
                'Dependent EGL/SDK signal escaped the future GPU wait')
        require(position(accepted[-1]) < position(retired[-1]) < position(satisfied[-3]), 'Release did not precede satisfied dependency')
        for begin, end in zip(hardware[1], hardware[2]):
            require(position(begin) < position(end), 'Signal retired before acceptance')
        for marker, count in (('nativeSyncHandleExported', 3), ('nativeSyncHandleImported', 5), ('nativeSyncHandleClosed', 3)):
            actual = select(marker)
            require(len(actual) == count and all(exact(r, **{marker: True}) for r in actual), 'Unbalanced native capability lifetime')
            if marker.endswith('Exported'):
                require(all(exact(r, rawHandleExposed=False) for r in actual), 'Raw handle exposed')
            if marker.endswith('Imported'):
                require(all(exact(r, gpuMapped=False) for r in actual), 'Wrong import path')
        imports, closes = select('nativeSyncHandleImported'), select('nativeSyncHandleClosed')
        require(position(imports[3]) < position(closes[0]) < position(imports[4]) < position(closes[1]) < position(closes[2]), 'Completed source lifetime not retained')
        budget = select('nativeMemoryBudgetQueried')
        require(len(budget) == 20 and {r.get('group') for r in budget} == {0, 1}, 'Missing native memory budgets')
        for r in budget:
            require(exact(r, nativeMemoryBudgetQueried=True, currentProcessOnly=True, physicalAdapter=0) and
                all(type(r.get(k)) is int and r[k] >= 0 for k in ('group', 'nativeBudget', 'budget', 'usage', 'reservation', 'available')) and
                0 < r['budget'] <= min(r['nativeBudget'], 268435456) and r['available'] <= r['budget'], 'Invalid budget ownership or diagnostic clamp')
        eviction = select('nativeVendorEviction')
        require(len(eviction) == 1 and exact(eviction[0], nativeVendorEviction=True, count=1, flags=1, ntstatus=0,
            bytesToTrim=0, pendingCommands=0, pendingSignals=0, pendingWaits=0, nativeCall=True), 'Missing native eviction or work still pending')
        evicted = once('LINUX_BRIDGE nativeVendorEviction=true count=1 flags=1 bytesToTrim=0')
        cycle = lines[evicted + 1:indices[5]]
        resident = 'LINUX_BRIDGE nativeVendorResident=true count=1 status=259 fence=7010 bytesToTrim=0'
        resident_wait = 'LINUX_BRIDGE cpuFenceWaitRetired=true direct=true target=7010 observed=7010 owner=paging'
        # Paging values are scoped to each owned queue and may occur elsewhere.
        require(cycle.count(resident) == cycle.count(resident_wait) == 1 and
            cycle.index(resident) < cycle.index(resident_wait), 'Eviction/remake-resident cycle did not finish')
        require(exact(clean, completedMemoryBudgetQueries=20, attemptedVendorEvictions=1, completedVendorEvictions=1,
            completedVendorResidencyRequests=155, completedVendorResidencyWaits=155,
            acceptedAsyncContextSignals=3, retiredAsyncContextSignals=3, pendingAsyncContextSignals=0,
            acceptedGpuQueueWaits=9, satisfiedGpuQueueWaits=9, pendingGpuQueueWaits=0,
            acceptedHwQueueSignals=8, completedHwQueueSignals=8, pendingHwQueueSignals=0,
            exportedSyncHandles=3, importedSyncObjects=5, closedSyncHandles=3, liveSharedSyncHandles=0,
            hardVendorAllocationByteQuotaImplemented=False), 'Unbalanced native operations')
        result.update(nativeGpuSignalTarget=target, syncFilesCreated=10, syncFilesReleased=10,
            nativeMemoryBudgetQueries=20, nativeEvictionCycleVerified=True,
            forcedPhysicalPageOutVerified=False, synchronizationOnlyResidencySignalsVerified=True)
    require(lines.index(egl[0]) < once('OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false') < kernel <
        once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true') < once('BRIDGE_RUNTIME_EXIT=0'), 'Wrong workload/teardown order')
    routes = [l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')]
    require(len(routes) == 1, 'Missing broker summary')
    match = re.fullmatch(r'BROKER_ROUTE_EXIT processExited=true retainedViews=(\d+) nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false', routes[0])
    require(match is not None, 'Broker cleanup failed')
    retained, calls, created, released = map(int, match.groups())
    require(created == retained + released and created == result['kernelFenceLeases'] + result['kernelAllocationLeases'] and
        released == lines.count('LINUX_BRIDGE kernelApertureRevoked=true') and
        retained == report['ownedQemuControl']['fenceMappingsReleasedAfterVmExit'] + report['ownedQemuControl']['allocationMappingsReleasedAfterVmExit'] and
        result['retiredNativeCommands'] <= calls <= 10000,
        'Unbalanced broker leases')
    require([l for l in lines if l.startswith('BROKER_ROUTE')] == [starts[0], routes[0], 'BROKER_ROUTE_REAP exit=0'], 'Wrong broker lifecycle')
    expected_objects = ['BROKER_OBJECT_DRAIN producerExited=true retained=0',
        'BROKER_OBJECT_SUMMARY exported=3 published=3 resolved=45 reaped=3 closed=3 retained=0',
        'BROKER_OBJECT recoverableErrors=7'] if enabled else [
        'BROKER_OBJECT_DRAIN producerExited=true retained=0',
        'BROKER_OBJECT_SUMMARY exported=0 published=0 resolved=0 reaped=0 closed=0 retained=0']
    require([l for l in lines if l.startswith('BROKER_OBJECT')] == expected_objects, 'Wrong capability cleanup')
    once('KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true')
    result.update(eglNativeFenceVerified=enabled, eglNativeFenceDisabledVerified=not enabled,
        nativeGpuWaitThroughEglVerified=enabled, completedEglNativeLifetimeVerified=enabled,
        zeroTargetEglImportVerified=enabled, canceledEglImportRejected=enabled,
        erroredFenceReportedComplete=False, unprivilegedGraphicsClientVerified=True,
        brokerNativeCalls=calls, brokerProtocol=3, generalEglConformanceVerified=False,
        foreignSyncFileImportVerified=False, mergedSyncFileImportVerified=False,
        gbmVerified=False, dmaBufVerified=False, desktopVerified=False, desktopStabilityVerified=False,
        multiClientGpuRoutingVerified=False, requiredApiMatrixVerified=False,
        concurrentWindowsGuestWorkloadsVerified=False, nearNativePerformanceVerified=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--disabled', action='store_true')
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(l) for l in args.report.with_suffix('.host.log').read_text().splitlines() if l.startswith('{')]
    result = validate_egl_native_sync(report, guest, records, enabled=not args.disabled)
    with args.output.open('x', encoding='utf8') as stream:
        json.dump(result, stream, indent=2); stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
