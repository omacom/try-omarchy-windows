"""Compare the retained 128-slot failure with two bounded full-buffer GPU runs."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_opengl_capture import validate_opengl_cleanup
from verify_resource_fd_capture import validate_resource_fds


def unpack(proof):
    return proof['report'], '\n'.join(proof['guestLines']), proof['records']


def validate_capacity_control(proof):
    report, guest, records = unpack(proof)
    validate_opengl_cleanup(report, records)
    require(exact(report, runtimeWorkload='opengl', hostBridgeExit=0, configuredCpuApertureSlots=128),
            'Wrong capacity-control workload, layout or host exit')
    lines = guest.splitlines()
    receipts = (
        'RESOURCE_FD_CONTRACT_PASS checks=15 held=0 active=0 typeIsolation=true ownerIsolation=true dupLifetime=true dmaBuf=false',
        'APERTURE_CONTRACT_PASS checks=581 activeLeases=0 activeCpuBytes=0',
        'KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false',
        'MESA_RESOURCE_FD_IMPORTED fdClosed=true descriptionMatches=true',
        'MESA_RESOURCE_FD_READBACK_PROFILE fullBuffer=true bandsPerRound=1 bytes=33177600',
        'MESA_RESOURCE_FD_FAILURE stage=ReadbackMap status=887a0005 errno=38',
        'OPENGL_FAILURE stage=SharedResourceFd eglError=3000',
        'KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true',
        'KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true',
        'BRIDGE_RUNTIME_EXIT=1')
    require(all(lines.count(line) == 1 for line in receipts), 'Missing or duplicated old-layout failure/cleanup receipt')
    require([lines.index(line) for line in receipts] == sorted(lines.index(line) for line in receipts),
            'Reordered capacity-control receipts')
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)$', guest, re.M) == ['1'] and not any(token in guest for token in
            ('BRIDGE_RUNTIME_SIGNAL', 'MESA_RESOURCE_FD_COMPLETE', 'MESA_RESOURCE_FD_SHARED_STORAGE',
             'APERTURE_CONTRACT_FAIL', 'RESOURCE_FD_CONTRACT_FAIL')), 'Control hid a success or unrelated failure')
    native_imports = [r for r in records if r.get('nativeResourceFdImported') is True]
    require(len(native_imports) == 1 and exact(native_imports[0], allocations=1, rawHandleExposed=False),
            'Missing real native import before capacity failure')
    cleanup = report['disconnectCleanup']
    require(exact(cleanup, vendorCpuSlotLimit=128, vendorCpuMappedByteLimit=96*1024*1024,
            vendorCpuSlotQuotaRejections=1, peakVendorCpuBytes=31526912,
            exportedResourceHandles=1, closedResourceHandles=1), 'Wrong old-layout capacity failure')
    return dict(reproducedVirtualSlotFailure=True, workloadAccepted=False, guestExit=1,
                nativeAndKernelCleanupVerified=True, cpuVirtualSlots=128,
                peakCpuMappedBytes=31526912, cpuMappedByteLimit=96*1024*1024)


def validate_cpu_capacity(evidence):
    control = evidence['oldLayoutControl']
    control_audit = validate_capacity_control(control['proof'])
    require(control_audit == control['audit'], 'Control audit differs from retained proof')
    runs = evidence['runs']
    require(len(runs) == 2 and len({r['label'] for r in runs}) == 2, 'Need two distinct actual GPU captures')
    reports = [control['proof']['report']]
    for run in runs:
        report, guest, records = unpack(run['proof'])
        audit = validate_resource_fds(report, guest, records, full_readback=True)
        require(audit == run['audit'], 'Full-buffer audit differs from retained proof')
        reports.append(report)
    for key in ('driverBridgeSha256', 'initramfsSha256', 'kernelSha256'):
        hashes = [r.get(key) for r in reports]
        require(all(isinstance(h, str) and re.fullmatch('[0-9a-f]{64}', h) for h in hashes) and
                len(set(hashes)) == 1, 'Comparison changed native bridge or private guest image: ' + key)
    old_qemu, *new_qemu = [r.get('qemuSha256') for r in reports]
    require(all(isinstance(h, str) and re.fullmatch('[0-9a-f]{64}', h) for h in (old_qemu, *new_qemu)) and
            new_qemu[0] == new_qemu[1] != old_qemu, 'Wrong QEMU comparison binaries')
    for report in reports:
        require(exact(report['disconnectCleanup'], vendorCpuMappedByteLimit=96*1024*1024,
                vendorGpuMappedByteLimit=256*1024*1024, syncObjectLimit=96, vendorAllocationObjectLimit=192),
                'Comparison raised a physical memory or object budget')
    return dict(schema=1, fullBufferCapacityComparisonVerified=True, fullReadbackBytes=33177600,
                positiveGuests=2, pixelsPerRound=8294400, roundsPerGuest=2,
                oldVirtualSlots=128, newVirtualSlots=256, cpuMappedByteLimit=96*1024*1024,
                oldFailureRetained=True, generalApertureFragmentationSolved=False,
                desktopVerified=False, desktopStabilityVerified=False, nearNativePerformanceVerified=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(validate_cpu_capacity(json.loads(args.evidence.read_text())), indent=2))


if __name__ == '__main__':
    main()
