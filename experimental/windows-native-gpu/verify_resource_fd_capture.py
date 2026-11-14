"""Audit the bounded native-resource FD diagnostic, never dma-buf or desktop acceptance."""
import argparse
from functools import lru_cache
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture
from verify_opengl_capture import validate_opengl_cleanup
from test_owned_runtime_qemu import async_submissions_complete


@lru_cache(maxsize=2)
def uniform_digest(rgba):
    # Independent reference for exactly 3840x2160 tightly packed RGBA8 pixels.
    value = 14695981039346656037
    pixel = bytes.fromhex(rgba)
    for _ in range(3840 * 2160):
        for byte in pixel:
            value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:016x}'


def validate_resource_fds(report, guest, records, *, full_readback=False, transport='virtio-port', retained_recipient=False, native_buffers=False):
    require(type(full_readback) is bool, 'Full-readback audit selection must be explicit')
    require(type(retained_recipient) is bool and (not retained_recipient or transport=='authenticated-broker'),
            'Retained-recipient audit requires the explicit broker transport')
    require(type(native_buffers) is bool and (not native_buffers or
            (transport=='authenticated-broker' and not retained_recipient and not full_readback)),
            'Native buffer audit requires the explicit bounded broker profile')
    allocation_slots = 256 if full_readback else 128
    bands, readback_bytes = (1, 33177600) if full_readback else (5, 7864320)
    result = validate_kernel_aperture(report, guest, records, allocation_slots=allocation_slots, transport=transport,
        contract_checks=583 if native_buffers else 581)
    lines = guest.splitlines()
    def exactly(line):
        require(lines.count(line) == 1, 'Missing or duplicated resource-FD receipt: ' + line)
        return lines.index(line)
    buffer_label = 'true' if native_buffers else 'false'
    start = exactly(f'MESA_RESOURCE_FD_BEGIN width=3840 height=2160 format=28 native=true dmaBuf={buffer_label} desktop=false')
    created = exactly('MESA_RESOURCE_FD_TEXTURE_CREATED width=3840 height=2160 shared=true')
    exported = exactly('MESA_RESOURCE_FD_EXPORTED native=true rawHostHandle=false')
    imported = exactly('MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true')
    complete = exactly(f'MESA_RESOURCE_FD_COMPLETE rounds=2 pixelsPerRound=8294400 native=true dmaBuf={buffer_label} desktop=false')
    exactly('RESOURCE_FD_CONTRACT_PASS checks=15 held=0 active=0 typeIsolation=true ownerIsolation=true dupLifetime=true dmaBuf=false')
    if full_readback:
        exactly('MESA_RESOURCE_FD_READBACK_PROFILE fullBuffer=true bandsPerRound=1 bytes=33177600')
    else:
        require('MESA_RESOURCE_FD_READBACK_PROFILE' not in guest, 'Full-buffer run requires explicit audit selection')
    require(not any(token in guest for token in ('MESA_RESOURCE_FD_FAILURE', 'MESA_RESOURCE_FD_NEGATIVE_CONTROL', 'RESOURCE_FD_CONTRACT_FAIL', 'D3D12: Removing Device.')),
            'Resource-FD or device failure')
    pattern = (r'^MESA_RESOURCE_FD_SHARED_STORAGE round=(\d+) writer=(source|imported) importedRead=true '
               r'sourceReleased=(true|false) fdClosed=true width=3840 height=2160 pixels=8294400 '
               rf'rgba=([0-9a-f]{{8}}) bands={bands} readbackBytes={readback_bytes} fnv1a=([0-9a-f]{{16}})$')
    receipts = [line for line in lines if line.startswith('MESA_RESOURCE_FD_SHARED_STORAGE')]
    require(len(receipts) == 2, 'Missing or duplicated shared-storage round')
    rounds = []
    for line, expected in zip(receipts, ((1, 'source', 'false', 'ff0000ff'), (2, 'imported', 'true', '00ff00ff'))):
        match = re.fullmatch(pattern, line)
        require(match is not None, 'Malformed shared-storage receipt')
        number, writer, released, rgba, digest = match.groups()
        require((int(number), writer, released, rgba) == expected, 'Wrong storage ownership phase or pixels')
        require(digest == uniform_digest(rgba), 'Shared-storage digest differs from the independent full-image reference')
        rounds.append(dict(round=int(number), writer=writer, sourceReleased=released == 'true',
                           verifiedPixels=3840 * 2160, rgba=rgba, fnv1a=digest))
    require(start < created < exported < imported < lines.index(receipts[0]) < lines.index(receipts[1]) < complete,
            'Invalid export/import/storage ordering')
    block = '\n'.join(lines[imported:complete])
    targets = re.findall(r'^LINUX_BRIDGE nativeHwQueueSignalQueued=true flags=4 queues=1 target=(\d+) observedAtReturn=\d+ noGpuAccess=true$', block, re.M)
    require([int(n) for n in targets] == list(range(1, 2 * bands + 1)), 'Missing or reordered band retirement signals')
    tickets = re.findall(r'^LINUX_BRIDGE kernelObjectExported=true kind=resource ticket=(\d+) cookie=(\d+)$', guest, re.M)
    export_count=2 if retained_recipient else 1
    require(len(tickets) == export_count and all(int(n)>0 for row in tickets for n in row) and
            len({row[0] for row in tickets})==len({row[1] for row in tickets})==export_count,
            'Missing or repeated typed resource FD export')
    ticket, _ = tickets[0]
    reaped = exactly('LINUX_BRIDGE kernelObjectReaped=true ticket=' + ticket)
    require(imported < reaped < lines.index(receipts[0]), 'FD reference was not reaped after import and before storage proof')
    def one(key):
        events = [event for event in records if event.get(key) is True]
        require(len(events) == 1, 'Missing or duplicated native event: ' + key)
        return events[0]
    negotiated = one('nativeResourceSharingNegotiated')
    native_exports=[r for r in records if r.get('nativeResourceHandleExported') is True]
    require(len(native_exports)==export_count and all(r.get('rawHandleExposed') is False for r in native_exports),
            'Wrong native export count or native HANDLE exposed')
    native_export=native_exports[0]
    open_begin = one('nativeResourceFdOpenBegin')
    native_open = one('nativeResourceFdOpenReturned')
    require(native_open.get('ntstatus') == 0 and native_open.get('hasResource') is True and native_open.get('hasAllocation') is True and
            native_open.get('hasKeyedMutex') is False and native_open.get('hasSync') is False, 'Invalid native resource-open result')
    native_import = one('nativeResourceFdImported')
    require(native_import.get('allocations') == 1 and native_import.get('rawHandleExposed') is False, 'Invalid native resource import')
    queries = [event for event in records if event.get('nativeResourceInfoQueried') is True]
    require(len(queries) == 2 and queries[0] == queries[1], 'Inconsistent native metadata queries')
    info = queries[0]
    require(0 < info.get('guestRuntimeBytes', 0) <= 1024 and 0 < info.get('hostRuntimeBytes', 0) <= 4000 and
            0 < info.get('driverBytes', 0) <= 4000 and info.get('resourceDriverBytes') == 0 and info.get('allocations') == 1,
            'Unbounded or unsupported native resource metadata')
    require(native_open.get('runtimeBytes') == info['hostRuntimeBytes'] and native_open.get('driverBytes') == info['driverBytes'] and
            native_open.get('allocationDriverBytes') == info['driverBytes'] and native_open.get('resourceDriverBytes') == 0 and
            native_import.get('guestRuntimeBytes') == info['guestRuntimeBytes'] and native_import.get('driverBytes') == info['driverBytes'],
            'Native/Linux resource metadata substitution')
    native_closes=[r for r in records if r.get('nativeResourceHandleClosed') is True]
    require(len(native_closes)==export_count, 'Wrong native NT-reference close count')
    native_closed=native_closes[0]
    require(records.index(negotiated) < records.index(native_export) < records.index(queries[0]) <
            records.index(open_begin) < records.index(native_open) < records.index(native_import) < records.index(native_closed),
            'Invalid native NT-reference lifetime or import ordering')
    cleanup = report['disconnectCleanup']
    require(cleanup.get('exportedResourceHandles') == export_count and cleanup.get('closedResourceHandles') == export_count and
            cleanup.get('liveSharedResourceHandles') == 0 and cleanup.get('vendorCpuSlotQuotaRejections') == 0,
            'Resource handle leak or CPU aperture exhaustion')
    if retained_recipient:
        armed=exactly('MESA_RESOURCE_FD_RECIPIENT_ARMED producerFdClosed=true recipientFdInherited=true gpuWork=false')
        drain=exactly('BROKER_OBJECT_DRAIN producerExited=true retained=1')
        alive=exactly('MESA_RESOURCE_FD_RECIPIENT producerExited=true capabilityOpen=true gpuWork=false')
        closing=exactly('MESA_RESOURCE_FD_RECIPIENT closingCapability=true producerExited=true gpuWork=false')
        reference=exactly('BROKER_OBJECT_CLOSE producerExited=true kind=2 ticket='+tickets[1][0]+' cookie='+tickets[1][1])
        require(complete<armed<drain<alive<closing<reference, 'Wrong recipient/native cleanup lifetime')
        export_positions=[i for i,r in enumerate(records) if r.get('nativeResourceHandleExported') is True]
        close_positions=[i for i,r in enumerate(records) if r.get('nativeResourceHandleClosed') is True]
        require(close_positions[0]<export_positions[1]<close_positions[1],
                'Wrong second native export/close ordering')
    else:
        require('MESA_RESOURCE_FD_RECIPIENT' not in guest, 'Recipient proof requires explicit audit selection')
    result.update(nativeResourceFdVerified=True, sharedStorageVerified=True, width=3840, height=2160,
                  rounds=rounds, readbackBandsPerRound=bands, diagnosticReadbackBytes=readback_bytes,
                  sourceAndFdReleaseVerified=True, resourceFdContractChecks=15,
                  drmDeviceVerified=False, dmaBufVerified=False, desktopVerified=False,
                  openGl4kVerified=False, nearNativePerformanceVerified=False)
    if retained_recipient:
        result.update(recipientFdBeyondProducerExitVerified=True,crossProcessGpuResourceImportVerified=False)
    if full_readback:
        require(report.get('configuredCpuApertureSlots') == cleanup.get('vendorCpuSlotLimit') == 256 and
                cleanup.get('vendorCpuMappedByteLimit') == 96 * 1024 * 1024 and
                33177600 <= cleanup.get('peakVendorCpuBytes', 0) <= 96 * 1024 * 1024,
                'Wrong virtual capacity or increased physical memory budget')
        # Lock2 exposes the owned native allocation extent, including driver
        # padding, rather than the tightly packed pixel count. Match all three
        # guest receipts and the native committed region; do not guess padding.
        mappings = re.findall(
            r'^LINUX_BRIDGE kernelApertureLease=true kind=2 bytes=(\d+) cookie=[1-9]\d*\n'
            r'LINUX_BRIDGE allocationCpuMapped=true direct=true loads=10000 bytes=(\d+) offset=(\d+) generation=([1-9]\d*)\n'
            r'LINUX_BRIDGE nativeVendorCpuLocked=true bytes=(\d+)$',
            '\n'.join(lines[imported:lines.index(receipts[0])]), re.M)
        full_mappings = [m for m in mappings if int(m[0]) >= readback_bytes]
        require(len(full_mappings) == 1, 'Missing or ambiguous full-buffer kernel CPU lease')
        lease_bytes, map_bytes, offset, _, lock_bytes = map(int, full_mappings[0])
        require(readback_bytes <= lease_bytes <= 64 * 1024 * 1024 and lease_bytes % 4096 == 0 and
                lease_bytes == map_bytes == lock_bytes and offset % (1024 * 1024) == 0 and
                offset + lease_bytes <= allocation_slots * 1024 * 1024,
                'Full-buffer lease differs from its bounded owned mapping')
        native_regions = [r for r in records if r.get('vendorCpuRegionCandidate') is True and
                          r.get('expectedBytes') == lease_bytes]
        require(len(native_regions) == 1 and exact(native_regions[0],
                     status=0, regionBytes=lease_bytes, type=131072, state=4096, protection=4,
                     baseMatches=True, allocationBaseMatches=True, regionBaseOffsetBytes=0,
                     exportedRangeContained=True), 'Missing bounded committed native full-buffer region')
        result.update(fullBufferReadbackVerified=True, cpuVirtualSlots=256, cpuMappedByteLimit=96 * 1024 * 1024,
                      nativeFullBufferMappingBytes=lease_bytes,
                      generalApertureFragmentationSolved=False, desktopStabilityVerified=False)
    return result


def validate_resource_fd_control(report, guest, records):
    """An explicit actual-GPU wrong-color run must fail, not become a milestone."""
    validate_opengl_cleanup(report, records)
    lines = guest.splitlines()
    require(report.get('runtimeWorkload') == 'opengl' and report.get('hostBridgeExit') == 0, 'Wrong control workload or native exit')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)$', guest, re.M) == ['1'] and 'BRIDGE_RUNTIME_SIGNAL' not in guest,
            'Wrong negative-control guest exit')
    require(lines.count('MESA_RESOURCE_FD_NEGATIVE_CONTROL kind=sourceColor expectedFailure=SharedPixels') == 1 and
            len(re.findall(r'^MESA_RESOURCE_FD_FAILURE stage=SharedPixels status=80004005 errno=\d+$', guest, re.M)) == 1 and
            lines.count('OPENGL_FAILURE stage=SharedResourceFd eglError=3000') == 1,
            'Missing actual GPU color-mismatch failure')
    require('MESA_RESOURCE_FD_COMPLETE' not in guest and 'MESA_RESOURCE_FD_SHARED_STORAGE' not in guest and 'Removing Device' not in guest,
            'Control reported a success or unrelated device failure')
    require(lines.count('MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true') == 1 and
            lines.count('KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true') == 1,
            'Missing real import or kernel cleanup in control')
    imported = [r for r in records if r.get('nativeResourceFdImported') is True]
    require(len(imported) == 1 and imported[0].get('rawHandleExposed') is False, 'Missing native resource import')
    for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
                        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired')):
        actual = [r for r in records if marker in r]
        require(report[key] == actual, 'Control/native command receipt mismatch')
    require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
            report['nativeAsyncSubmissionRetirements'], report['disconnectCleanup'], maximum=64), 'Unretired control GPU commands')
    cleanup = report['disconnectCleanup']
    require(cleanup.get('exportedResourceHandles') == cleanup.get('closedResourceHandles') == 1 and
            cleanup.get('liveSharedResourceHandles') == cleanup.get('vendorCpuSlotQuotaRejections') == 0,
            'Control resource leak or aperture exhaustion')
    return dict(schema=1, intentionalWrongColorRejected=True, guestExit=1, nativeCleanupVerified=True,
                nativeResourceFdImported=True, sharedStorageVerified=False, desktopVerified=False,
                dmaBufVerified=False, nearNativePerformanceVerified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--wrong-color-control', action='store_true')
    parser.add_argument('--full-readback', action='store_true')
    args = parser.parse_args()
    if args.wrong_color_control and args.full_readback:
        parser.error('Select one explicit audit profile')
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    result = (validate_resource_fd_control(report, guest, records) if args.wrong_color_control else
              validate_resource_fds(report, guest, records, full_readback=args.full_readback))
    with args.output.open('x', encoding='utf-8') as destination:
        json.dump(result, destination, indent=2); destination.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
