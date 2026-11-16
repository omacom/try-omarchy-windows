"""Audit genuine Mesa native-timeline FD imports and future GPU queue ordering.

This private D3D12 FD contract does not prove binary Vulkan interoperability,
Linux sync_file support, GL conformance, compositor synchronization or a desktop.
"""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_disabled_mesa(report, guest, records):
    result = validate_kernel_aperture(report, guest, records)
    profiles = [r for r in records if 'openGlProfileNegotiated' in r]
    require(len(profiles) == 1 and exact(profiles[0], version=1), 'Wrong non-opt-in profile')
    extensions = [line for line in guest.splitlines() if line.startswith('OPENGL_INTEROP_EXTENSION ')]
    require(extensions and not any('semaphore' in line for line in extensions) and
            'MESA_FENCE_FD_' not in guest, 'Experimental semaphore path enabled without opt-in')
    result.update(experimentalSemaphoreExtensionsAdvertised=False, mesaNativeTimelineFdVerified=False)
    return result


def validate_mesa_fence_fds(report, guest, records, *, allocation_slots=128, transport='virtio-port'):
    require(type(allocation_slots) is int and allocation_slots in (128,256), 'Explicit supported virtual aperture layout required')
    result = validate_kernel_aperture(report, guest, records, allocation_slots=allocation_slots, transport=transport)
    require(exact(report, liveNvidiaLinuxUmdLoaded=True, realWslDxgForwarding=False,
                  capturedPrivateFixturesUsed=False), 'Wrong native runtime provenance')
    profile = [r for r in records if 'openGlProfileNegotiated' in r]
    require(len(profile) == 1 and exact(profile[0], version=2, objectLimit=384,
                  hwQueueLimit=16, vendorObjectLimit=192), 'Missing explicit fence profile')
    lines = guest.splitlines()

    def once(line):
        require(lines.count(line) == 1, 'Missing or duplicate Mesa fence receipt: ' + line)
        return lines.index(line)

    expected = [
        'MESA_FENCE_FD_BEGIN timeline=true binary=false syncFile=false desktop=false',
        'MESA_FENCE_FD_REJECT kind=binary error=0502 fdRetained=true',
        'MESA_FENCE_FD_REJECT kind=unimported-wait-signal error=0502',
        'MESA_FENCE_FD_IMPORT timeline=true sourceReleased=true fdOwnership=GL',
        'MESA_FENCE_FD_REJECT kind=foreign error=0502 fdRetained=true oldFenceRetained=true',
        'MESA_FENCE_FD_QUEUE type=COPY status=00000000',
        'MESA_FENCE_FD_SIGNAL api=GL target=1 observed=1',
        'MESA_FENCE_FD_WAIT target=2 dependentSignal=3 observedBeforeRelease=1',
        'MESA_FENCE_FD_RELEASE api=D3D12 target=2 dependentObserved=3',
        'MESA_FENCE_FD_COMPLETE glSignals=2 gpuReleaseSignals=1 futureWaits=1 desktop=false']
    require([line for line in lines if line.startswith('MESA_FENCE_FD_')] == expected,
            'Incorrect Mesa API results, ownership or ordering')
    start, imported, signal, end = (once(expected[i]) for i in (0, 3, 6, 9))
    require(once('OPENGL_FRAME frame=2 vertices=3 error=0000') < start < end <
            once('OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false') <
            once('KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true') <
            once('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true') <
            once('BRIDGE_RUNTIME_EXIT=0'), 'Wrong GL/FD/kernel teardown order')
    once('OBJECT_CONTRACT_PASS checks=561 held=0 active=0 forgedRejected=true ownerIsolation=true dupLifetime=true reapRollback=true')
    for extension in ('GL_EXT_semaphore', 'GL_EXT_semaphore_fd', 'GL_NV_timeline_semaphore'):
        once('OPENGL_INTEROP_EXTENSION ' + extension)
    require('OBJECT_CONTRACT_FAIL' not in guest and 'cleanup=false' not in guest, 'Kernel object failure')

    exported = [line for line in lines if line.startswith('LINUX_BRIDGE kernelObjectExported=')]
    reaped = [line for line in lines if line.startswith('LINUX_BRIDGE kernelObjectReaped=')]
    require(len(exported) == len(reaped) == 2, 'Wrong guest FD ownership count')
    identities = []
    for n, (creation, destruction) in enumerate(zip(exported, reaped)):
        match = re.fullmatch(r'LINUX_BRIDGE kernelObjectExported=true kind=sync ticket=(\d+) cookie=(\d+)', creation)
        require(match is not None and int(match[1]) > 0 and int(match[2]) > 0 and
                destruction == 'LINUX_BRIDGE kernelObjectReaped=true ticket=' + match[1], 'Mismatched FD identity')
        identities.append((int(match[1]), int(match[2])))
        require(start < lines.index(creation) < lines.index(destruction) < imported,
                'FD not consumed before original fence release')
        if n:
            require(lines.index(reaped[n-1]) < lines.index(creation), 'Control import must precede GL export')
    require(len(set(identities)) == 2 and identities[0][0] < identities[1][0] and
            identities[0][1] < identities[1][1], 'Reused kernel FD identity')
    for line in ('LINUX_BRIDGE nativeSyncFdImported=true gpuMapped=0',
                 'LINUX_BRIDGE nativeSyncFdImportInput flags=1155 affinity=0 outputFieldsIgnored=true'):
        require(lines.count(line) == 2 and all(start < i < imported for i, text in enumerate(lines) if text == line),
                'Missing native FD import evidence')
    require(lines.count('LINUX_BRIDGE nativeGpuQueueWaitQueued=true objects=1 firstTarget=2 contextStream=0 cpuWait=false') == 3,
            'Missing nonblocking guest GPU queue waits')

    def select(marker):
        return [r for r in records if marker in r]

    lifecycle = [r for r in records if any(k in r for k in ('nativeSyncSharingNegotiated',
        'nativeSyncHandleExported', 'nativeSyncResourceInfoQueried', 'nativeSyncHandleImported', 'nativeSyncHandleClosed'))]
    expected_lifecycle = [{'nativeSyncSharingNegotiated': True, 'handleLimit': 16}] + 2 * [
        {'nativeSyncHandleExported': True, 'rawHandleExposed': False},
        {'nativeSyncResourceInfoQueried': True, 'ntstatus': -1073741788},
        {'nativeSyncHandleImported': True, 'gpuMapped': False}, {'nativeSyncHandleClosed': True}]
    require(json.dumps(lifecycle, sort_keys=True) == json.dumps(expected_lifecycle, sort_keys=True),
            'Native export/type-query/import/close mismatch')
    hardware, retired = select('nativeHwQueueSignalAccepted'), select('nativeHwQueueSignalRetired')
    require(len(hardware) == len(retired) == 2, 'Wrong GL GPU signal count')
    for n, (accepted, done) in enumerate(zip(hardware, retired), 1):
        target = (1, 3)[n-1]
        require(exact(accepted, nativeHwQueueSignalAccepted=True, index=n, flags=4, queues=1,
                      target=target, observed=1, noGpuAccess=True, cpuValueWrittenByBridge=False) and
                exact(done, nativeHwQueueSignalRetired=True, index=n, flags=4, queues=1,
                      target=target, observed=target, noGpuAccess=True, cpuValueWrittenByBridge=False) and
                records.index(accepted) < records.index(done), 'Invalid GL GPU signal completion')
    waits, satisfied = select('nativeGpuQueueWaitAccepted'), select('nativeGpuQueueWaitDependenciesSatisfied')
    require(len(waits) == len(satisfied) == 3, 'Wrong native queue wait count')
    for n, (wait, done) in enumerate(zip(waits, satisfied), 1):
        require(exact(wait, nativeGpuQueueWaitAccepted=True, index=n, objects=1, contextStream=False,
                      firstTarget=2, cpuWait=False) and type(wait.get('nativeCallMs')) is int and wait['nativeCallMs'] >= 0 and
                exact(done, nativeGpuQueueWaitDependenciesSatisfied=True, index=n, objects=1,
                      contextStream=False, queueRetirementVerified=False) and
                records.index(retired[0]) < records.index(wait) < records.index(hardware[1]) < records.index(done),
                'Future wait not queued before the dependent GL signal')
    releases, release_done = select('nativeAsyncContextSignalAccepted'), select('nativeAsyncContextSignalRetired')
    require(len(releases) == len(release_done) == 1, 'Wrong independent release count')
    require(exact(releases[0], nativeAsyncContextSignalAccepted=True, index=1, target=2, cpuWait=False) and
            exact(release_done[0], nativeAsyncContextSignalRetired=True, index=1, target=2,
                  noGpuAccess=True, cpuValueWrittenByBridge=False) and
            all(type(r.get('observed')) is int and 2 <= r['observed'] <= 3 for r in (releases[0], release_done[0])),
            'Invalid native D3D12 release completion')
    require(records.index(lifecycle[-1]) < records.index(hardware[0]) < records.index(retired[0]) <
            records.index(hardware[1]) < records.index(releases[0]) < records.index(retired[1]) and
            records.index(releases[0]) < records.index(release_done[0]), 'Native GL wait/signal/release ordering disagrees')
    require(all(records.index(releases[0]) < records.index(done) for done in satisfied), 'Wait satisfied before release')
    cleanup = report['disconnectCleanup']
    require(exact(cleanup, exportedSyncHandles=2, importedSyncObjects=2, closedSyncHandles=2,
                  acceptedAsyncContextSignals=1, retiredAsyncContextSignals=1, pendingAsyncContextSignals=0,
                  acceptedHwQueueSignals=2, completedHwQueueSignals=2, pendingHwQueueSignals=0,
                  acceptedGpuQueueWaits=3, satisfiedGpuQueueWaits=3, pendingGpuQueueWaits=0,
                  vendorAllocationObjectLimit=192, vendorCpuSlotLimit=allocation_slots, vendorCpuSlotQuotaRejections=0),
            'Unbalanced native work or changed allocation budgets')
    require(all(type(v) is int and v == 0 for k, v in cleanup.items() if k.startswith('failed') or k.endswith('Timeouts')),
            'Native failure or timeout')
    result.update(mesaNativeTimelineFdVerified=True, mesaFutureGpuQueueOrderingVerified=True,
                  successfulFdImports=2, glGpuSignals=2, independentD3d12ReleaseSignals=1,
                  nativeQueueWaits=3, kernelObjectContractChecks=561, importedFdOwnershipVerified=True,
                  binarySemaphoreInteroperabilityVerified=False, syncFileVerified=False,
                  compositorSynchronizationVerified=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    result = validate_mesa_fence_fds(report, guest, records)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
