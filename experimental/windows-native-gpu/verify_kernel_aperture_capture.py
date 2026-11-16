"""Audit kernel memory leases plus genuine OpenGL pixels; never DRM/desktop support."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import require
from verify_opengl_capture import validate_opengl


def validate_kernel_aperture(report, guest, records, *, allocation_slots=128, transport='virtio-port', contract_checks=581):
    # The opt-in sync-file contract adds two calls to the existing checked
    # lease-revocation helper. Older/default captures retain exactly 581.
    require(type(contract_checks) is int and contract_checks in (581,583), 'Explicit kernel contract profile required')
    result = validate_opengl(report, guest, records, allocation_slots=allocation_slots, transport=transport)
    lines = guest.splitlines()
    for line in (
            'APERTURE_CONTRACT fdTransfer=true unprivilegedMap=true readOnly=true revokeSigbus=true',
            f'APERTURE_CONTRACT_PASS checks={contract_checks} activeLeases=0 activeCpuBytes=0',
            'KERNEL_APERTURE_START loaded=true contracts=true rawSysfsFallback=false',
            'KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true'):
        require(lines.count(line) == 1, 'Missing kernel isolation or cleanup receipt')
    require('APERTURE_CONTRACT_FAIL' not in guest and 'Kernel aperture ' not in guest and
            'KERNEL_APERTURE_START failed=' not in guest and 'cleanup=false' not in guest,
            'Guest kernel lease failure')
    require(guest.index('APERTURE_CONTRACT_PASS ') < guest.index('KERNEL_APERTURE_START loaded=') <
            guest.index('OPENGL_TEST_BEGIN ') < guest.index('OPENGL_TEST_COMPLETE ') <
            guest.index('KERNEL_APERTURE_EXIT ') < guest.index('BRIDGE_RUNTIME_EXIT=0'),
            'Invalid kernel/probe/cleanup ordering')
    layouts = [line for line in lines if line.startswith(('LINUX_BRIDGE fenceHubLayout=', 'LINUX_BRIDGE allocationCpuLayout='))]
    require(layouts and all(line.endswith(' kernel=true') for line in layouts), 'Raw sysfs aperture path used')
    for prefix, expected in (
            ('LINUX_BRIDGE fenceHubLayout=', 'LINUX_BRIDGE fenceHubLayout=true slots=128 stride=4096 readonly=true kernel=true'),
            ('LINUX_BRIDGE allocationCpuLayout=', f'LINUX_BRIDGE allocationCpuLayout=true slots={allocation_slots} stride=1048576 kernel=true')):
        observed = [line for line in layouts if line.startswith(prefix)]
        require(observed and all(line == expected for line in observed), 'Missing or inconsistent kernel hub layout')
    leases = re.findall(r'^LINUX_BRIDGE kernelApertureLease=true kind=([12]) bytes=(\d+) cookie=(\d+)[ \t\r]*$', guest, re.M)
    require(len(leases) == guest.count('LINUX_BRIDGE kernelApertureLease='), 'Malformed kernel lease receipt')
    cookies = [int(cookie) for _, _, cookie in leases]
    require(cookies and cookies == sorted(set(cookies)) and all(c > 0 for c in cookies), 'Reused kernel lease identity')
    require(all(int(size) == 4096 if kind == '1' else 0 < int(size) <= 96 * 1024 * 1024 and not int(size) % 4096
                for kind, size, _ in leases), 'Invalid kernel lease size')
    vm = report['ownedQemuControl']
    fence_count = sum(kind == '1' for kind, _, _ in leases)
    allocation_count = sum(kind == '2' for kind, _, _ in leases)
    require(fence_count == vm['fenceMappingsCreated'] and allocation_count == vm['allocationMappingsCreated'],
            'Native mappings were not all backed by kernel leases')
    result.update(kernelApertureVerified=True, kernelContractChecks=contract_checks,
                  kernelFenceLeases=fence_count, kernelAllocationLeases=allocation_count,
                  transferredFdRevocationVerified=True, kernelModuleUnloaded=True,
                  drmDeviceVerified=False, dmaBufVerified=False, guestGraphicsIoctlKernelImplemented=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    result = validate_kernel_aperture(report, guest, records)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2); stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
