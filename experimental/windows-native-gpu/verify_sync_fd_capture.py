"""Audit opaque guest fence FDs; distinguish native GPU signals from guest proof."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_sync_fds(report, guest, records):
    result = validate_kernel_aperture(report, guest, records)
    lines = guest.splitlines()
    contract = 'OBJECT_CONTRACT_PASS checks=561 held=0 active=0 forgedRejected=true ownerIsolation=true dupLifetime=true reapRollback=true'
    completion = ('SYNC_FD_TEST_COMPLETE nativeExports=2 nativeImports=1 sourceDestroyedBeforeImport=true '
                  'unprivilegedTransfer=true reexport=true initialValue=57 gpuSignals=false cleanup=true')
    cleanup = 'KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true'
    negotiation = 'LINUX_BRIDGE nativeSyncSharingNegotiated=true kernelObjectFds=true'
    for line in (contract, completion, cleanup, negotiation,
                 'OBJECT_CONTRACT_UNPRIVILEGED_CREATE denied=true',
                 'LINUX_BRIDGE nativeSyncFdImported=true gpuMapped=0'):
        require(lines.count(line) == 1, 'Missing or duplicate object FD receipt')
    require('OBJECT_CONTRACT_FAIL' not in guest and 'SYNC_FD_FAILURE' not in guest,
            'Guest object FD failure')
    require(guest.index(contract) < guest.index('KERNEL_APERTURE_START loaded=') <
            guest.index('OPENGL_FRAME frame=2 ') < guest.index(negotiation) <
            guest.index(completion) < guest.index('OPENGL_TEST_COMPLETE ') <
            guest.index(cleanup) < guest.index('KERNEL_APERTURE_EXIT ') <
            guest.index('BRIDGE_RUNTIME_EXIT=0'), 'Incorrect object FD lifecycle ordering')
    events = [line for line in lines if line.startswith(('LINUX_BRIDGE kernelObjectExported=',
                                                       'LINUX_BRIDGE kernelObjectReaped='))]
    require(len(events) == 4, 'Wrong kernel object lifetime receipt count')
    exports, reaps = [], []
    for index, line in enumerate(events):
        if index % 2 == 0:
            match = re.fullmatch(r'LINUX_BRIDGE kernelObjectExported=true kind=sync ticket=(\d+) cookie=(\d+)', line)
            require(match is not None, 'Malformed or reordered object export')
            exports.append(tuple(map(int, match.groups())))
        else:
            match = re.fullmatch(r'LINUX_BRIDGE kernelObjectReaped=true ticket=(\d+)', line)
            require(match is not None, 'Malformed or reordered object reap')
            reaps.append(int(match[1]))
    require(all(0 < ticket < 2**32-1 and cookie > 0 for ticket, cookie in exports) and
            exports[0][0] < exports[1][0] and exports[0][1] < exports[1][1] and
            reaps == [ticket for ticket, _ in exports], 'Reused, unmatched or forged object identity')
    require(guest.index(events[0]) < guest.index('LINUX_BRIDGE nativeSyncFdImported=') <
            guest.index(events[1]) < guest.index(events[2]) < guest.index(events[3]) <
            guest.index(completion), 'Incorrect export/import/reap ordering')
    markers = ('nativeSyncSharingNegotiated', 'nativeSyncHandleExported',
               'nativeSyncHandleImported', 'nativeSyncHandleClosed')
    native = [record for record in records if any(marker in record for marker in markers)]
    expected_native = [
        {'nativeSyncSharingNegotiated': True, 'handleLimit': 16},
        {'nativeSyncHandleExported': True, 'rawHandleExposed': False},
        {'nativeSyncHandleImported': True, 'gpuMapped': False},
        {'nativeSyncHandleClosed': True},
        {'nativeSyncHandleExported': True, 'rawHandleExposed': False},
        {'nativeSyncHandleClosed': True}]
    require(json.dumps(native, sort_keys=True) == json.dumps(expected_native, sort_keys=True),
            'Native fence export/import/close receipts disagree')
    require(exact(report['disconnectCleanup'], liveSharedSyncHandles=0,
                  exportedSyncHandles=2, importedSyncObjects=1, closedSyncHandles=2),
            'Native shared-fence references not balanced')
    require(result['kernelFenceLeases'] == 73 and result['kernelAllocationLeases'] == 91,
            'Wrong rendering/import mapping count')
    result.update(kernelObjectFdsVerified=True, kernelObjectContractChecks=561,
                  nativeFenceExports=2, nativeFenceImports=1, nativeFenceHandleCloses=2,
                  unprivilegedFdTransferVerified=True, sourceDestroyedBeforeImportVerified=True,
                  importedFenceReexportVerified=True, guestFenceInitialValue=57,
                  guestGpuTimelineVerified=False, crossProcessNativeImportVerified=False,
                  mesaSemaphoreFdVerified=False, syncFileVerified=False)
    return result


def validate_native_sync(log):
    lines = log.splitlines()
    begin = 'NATIVE_SYNC_BEGIN vendor=4318 device=11352 cpuWritesFence=false guest=false desktop=false'
    release = 'NATIVE_SYNC_SOURCE_RELEASED originalDestroyed=true ntHandleClosed=true aliasesLive=true'
    complete = ('NATIVE_SYNC_COMPLETE gpuSignals=16 originalReleasedAfter=8 ntClosedBeforeLast=8 '
                'aliasesReleased=true cleanup=true guest=false desktop=false')
    for line in (begin, release, complete):
        require(lines.count(line) == 1, 'Missing native GPU fence lifecycle receipt')
    require('NATIVE_SYNC_FAILURE' not in log, 'Native GPU fence failure')
    statuses = [line for line in lines if line.startswith(('NATIVE_SYNC_NT ', 'NATIVE_SYNC_HR ', 'NATIVE_SYNC_CLEANUP '))]
    require(statuses and all(re.fullmatch(r'NATIVE_SYNC_(?:NT|HR|CLEANUP) (?:stage|kind)=\w+ status=00000000', line)
                             for line in statuses), 'Failed native API or cleanup')
    signals = [line for line in lines if line.startswith('NATIVE_SYNC_GPU_RETIRED ')]
    expected = [f'NATIVE_SYNC_GPU_RETIRED target={i} source={i} recipient={i} d3d12={i} sourceAlive={int(i <= 8)}'
                for i in range(1, 17)]
    require(signals == expected and log.index(begin) < log.index(signals[0]) < log.index(signals[7]) <
            log.index(release) < log.index(signals[8]) < log.index(signals[-1]) < log.index(complete),
            'Missing or reordered native GPU timeline observation')
    require(sum('stage=QueueSignal ' in line for line in statuses) == 8 and
            sum('stage=QueueSignalAfterClose ' in line for line in statuses) == 8 and
            sum('kind=fence ' in line for line in statuses) == 1 and
            sum('kind=device ' in line for line in statuses) == 2 and
            sum('kind=adapter ' in line for line in statuses) == 1,
            'Unbalanced native GPU signal or object cleanup')
    for stage in ('CreateSource', 'ExportNt', 'ImportKmt', 'ImportD3D12', 'DestroySource',
                  'ReexportRecipient', 'ReimportD3D12'):
        require(sum(f'stage={stage} ' in line for line in statuses) == 1, 'Missing native sharing API receipt')
    return {'nativeGpuSharedFenceVerified': True, 'gpuSignals': 16, 'signalsAfterSourceAndNtClose': 8,
            'guestGpuTimelineVerified': False, 'desktopVerified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines()
               if line.startswith('{')]
    result = validate_sync_fds(report, guest, records)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2); stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
