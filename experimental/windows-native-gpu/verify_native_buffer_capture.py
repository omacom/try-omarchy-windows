"""Audit opaque native DMA-BUF ownership and real 4K storage; not EGL image/GBM support."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture
from verify_resource_fd_capture import validate_resource_fds


def validate_native_buffers(report, guest, records, *, enabled=True):
    require(type(enabled) is bool, 'Explicit native buffer profile required')
    result = (validate_resource_fds(report, guest, records, transport='authenticated-broker', native_buffers=True)
              if enabled else validate_kernel_aperture(report, guest, records,
                  transport='authenticated-broker', contract_checks=583))
    require(exact(report, liveNvidiaLinuxUmdLoaded=True, realWslDxgForwarding=False,
                  capturedPrivateFixturesUsed=False), 'Wrong runtime provenance')
    lines = guest.splitlines()
    def once(line):
        require(lines.count(line) == 1, 'Missing or duplicated receipt: ' + line)
        return lines.index(line)
    def selected(marker):
        return [r for r in records if marker in r]
    def position(record):
        return next(i for i, r in enumerate(records) if r is record)

    require(not any(token in guest for token in ('NATIVE_BUFFER_CONTRACT_FAIL', 'MESA_RESOURCE_FD_FAILURE',
        'MESA_RESOURCE_FD_NEGATIVE_CONTROL', 'Oops:', 'BUG:', 'cleanup=false')), 'Buffer, GPU or kernel failure')
    clean, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(all(type(v) is int and v == 0 for k, v in clean.items()
                if k.startswith('failed') or k.endswith('Timeouts')), 'Native failure or timeout')
    client = once('BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    starts = [l for l in lines if l.startswith('BROKER_ROUTE_START ')]
    require(len(starts) == 1 and re.fullmatch(r'BROKER_ROUTE_START protocol=3 clientPid=[1-9]\d* clientUid=65534 nativeWorkerClients=1 kernelControllersClient=false', starts[0]), 'Wrong broker identity')
    require(client < lines.index(starts[0]), 'Broker started before privilege drop')
    begin = once('MESA_RESOURCE_FD_BEGIN width=3840 height=2160 format=28 native=true dmaBuf=true desktop=false')
    texture = once('MESA_RESOURCE_FD_TEXTURE_CREATED width=3840 height=2160 shared=true')
    require(begin < texture, 'Texture predates workload')
    if enabled:
        contract = once('NATIVE_BUFFER_CONTRACT_PASS checks=200 dmaBuf=true opaque=true cpuAccess=false foreignDma=false gpu=false')
        export = once('NATIVE_BUFFER_GPU_EXPORT dmaBuf=true nativeFdClosed=true duplicateLifetime=true cloexec=true logicalBytes=33177600 cpuAccess=false eglImage=false gbm=false')
        imported = once('MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true')
        lifetime = once('NATIVE_BUFFER_GPU_LIFETIME originalNativeFdClosed=true dmaBufFdClosed=true importedGpuReference=true baselineCreated=71 created=72 released=72 held=0 bytes=0')
        complete = once('NATIVE_BUFFER_GPU_COMPLETE sharedStorage=true sourceReleased=true eglImage=false gbm=false drm=false desktop=false')
        buffers = once('KERNEL_NATIVE_BUFFER_EXIT held=0 bytes=0 created=72 released=72 cleanup=true')
        require(contract < begin < texture < export < imported < lifetime < complete < buffers, 'Wrong native buffer lifetime')
        require('NATIVE_BUFFER_GPU_DISABLED' not in guest, 'Mixed buffer profiles')
    else:
        contract = once('NATIVE_BUFFER_CONTRACT disabled=true gpu=false')
        complete = once('NATIVE_BUFFER_GPU_DISABLED exportUnsupported=true infoUnsupported=true inputFdRetained=true dmaBufsCreated=0 gpuImport=false desktop=false')
        require(contract < begin < texture < complete, 'Wrong disabled control order')
        require(not any(s in guest for s in ('NATIVE_BUFFER_GPU_EXPORT', 'NATIVE_BUFFER_GPU_LIFETIME',
            'NATIVE_BUFFER_GPU_COMPLETE', 'MESA_RESOURCE_FD_IMPORTED', 'MESA_RESOURCE_FD_COMPLETE',
            'MESA_RESOURCE_FD_SHARED_STORAGE', 'KERNEL_NATIVE_BUFFER_EXIT')), 'Disabled buffers used')
        require(not selected('nativeResourceFdImported') and not selected('nativeResourceFdOpenBegin'), 'Disabled resource imported')
        for marker in ('nativeResourceHandleExported', 'nativeResourceHandleClosed'):
            events = selected(marker)
            require(len(events) == 1 and events[0][marker] is True, 'Unbalanced disabled source handle')
        require(selected('nativeResourceHandleExported')[0].get('rawHandleExposed') is False,
                'Raw native handle exposed')
        # The host's separate creation-time metadata probe may import an NT
        # resource. This counter records actual guest FD imports specifically.
        require(exact(clean, exportedResourceHandles=1, closedResourceHandles=1, liveSharedResourceHandles=0,
            importedResourceFdObjects=0), 'Disabled control leaked or imported a guest resource')
    # Mesa batch-fence integration is independently enabled for both profiles.
    sync = once('KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=5 signaled=5 errors=0 released=5 cleanup=true')
    require(complete < once('OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false') < sync <
        once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true') <
        once('BRIDGE_RUNTIME_EXIT=0'), 'Wrong runtime teardown')
    waits, done = selected('nativeGpuQueueWaitAccepted'), selected('nativeGpuQueueWaitDependenciesSatisfied')
    require(len(waits) == len(done) == 6, 'Missing Mesa GPU dependencies')
    for n, (accepted, retired) in enumerate(zip(waits, done), 1):
        require(exact(accepted, nativeGpuQueueWaitAccepted=True, index=n, objects=1, contextStream=False,
            firstTarget=1 if n <= 3 else 2, cpuWait=False) and
            exact(retired, nativeGpuQueueWaitDependenciesSatisfied=True, index=n, objects=1,
                contextStream=False, queueRetirementVerified=False) and position(accepted) < position(retired),
            'Wrong Mesa GPU wait')
    signals = 15 if enabled else 5
    for key, marker in (('nativeHwQueueSignalReturns', 'nativeHwQueueSignalReturned'),
                        ('nativeHwQueueSignalAcceptances', 'nativeHwQueueSignalAccepted'),
                        ('nativeHwQueueSignalRetirements', 'nativeHwQueueSignalRetired')):
        events = selected(marker)
        require(events == report[key] and len(events) == signals, 'Unmatched native signal evidence')
        for event in events:
            require(event.get(marker) is True and type(event.get('observed')) is int and
                type(event.get('target')) is int and 0 <= event['observed'] <= event['target'], 'Invalid signal counters')
            require(event.get('ntstatus') == 0 if marker.endswith('Returned') else
                exact(event, noGpuAccess=True, cpuValueWrittenByBridge=False), 'Failed or CPU-written signal')
            if marker.endswith('Retired'):
                require(event['observed'] == event['target'], 'Signal did not retire')
    require(exact(clean, acceptedGpuQueueWaits=6, satisfiedGpuQueueWaits=6, pendingGpuQueueWaits=0,
        acceptedHwQueueSignals=signals, completedHwQueueSignals=signals, pendingHwQueueSignals=0,
        exportedSyncHandles=1, closedSyncHandles=1, importedSyncObjects=0, liveSharedSyncHandles=0), 'Unbalanced native synchronization')
    routes = [l for l in lines if l.startswith('BROKER_ROUTE_EXIT ')]
    require(len(routes) == 1, 'Missing broker summary')
    match = re.fullmatch(r'BROKER_ROUTE_EXIT processExited=true retainedViews=0 nativeCalls=(\d+) viewsCreated=(\d+) viewsReleased=(\d+) rejected=0 multiClient=false', routes[0])
    require(match is not None, 'Broker retained mappings or rejected a request')
    calls, created, released = map(int, match.groups())
    require(created == released == result['kernelFenceLeases'] + result['kernelAllocationLeases'] ==
        lines.count('LINUX_BRIDGE kernelApertureRevoked=true') and result['retiredNativeCommands'] <= calls <= 10000 and
        vm['fenceMappingsReleasedAfterVmExit'] == vm['allocationMappingsReleasedAfterVmExit'] ==
        vm['allocationSlotMappingsReleasedAfterVmExit'] == 0, 'Unbalanced broker leases')
    require([l for l in lines if l.startswith('BROKER_ROUTE')] == [starts[0], routes[0], 'BROKER_ROUTE_REAP exit=0'], 'Wrong broker lifecycle')
    summaries = [l for l in lines if l.startswith('BROKER_OBJECT_SUMMARY ')]
    require(len(summaries) == 1 and re.fullmatch(r'BROKER_OBJECT_SUMMARY exported=2 published=2 resolved=[1-9]\d* reaped=2 closed=2 retained=0', summaries[0]), 'Capability reference leak')
    require([l for l in lines if l.startswith('BROKER_OBJECT')] == [
        'BROKER_OBJECT_DRAIN producerExited=true retained=0', summaries[0],
        f'BROKER_OBJECT recoverableErrors={1 if enabled else 2}'], 'Wrong recoverable errors or capability teardown')
    once('KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true')
    result.update(nativeDmaBufLifetimeVerified=enabled, nativeBufferDisabledVerified=not enabled,
        nativeBufferContractChecks=200 if enabled else 0, nativeBuffersCreated=72 if enabled else 0,
        nativeBuffersReleased=72 if enabled else 0, dmaBufVerified=enabled,
        generalDmaBufInteropVerified=False, dmaBufCpuAccessVerified=False, foreignDmaAttachmentVerified=False,
        implicitBufferSynchronizationVerified=False, eglImageImportVerified=False, gbmVerified=False,
        drmDeviceVerified=False, desktopVerified=False, desktopStabilityVerified=False,
        nearNativePerformanceVerified=False, unprivilegedGraphicsClientVerified=True,
        brokerNativeCalls=calls, brokerProtocol=3, syncFilesCreated=5, syncFilesReleased=5)
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
    result = validate_native_buffers(report, guest, records, enabled=not args.disabled)
    with args.output.open('x', encoding='utf8') as stream:
        json.dump(result, stream, indent=2); stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
