"""Audit native fence CPU views in a late ELF finalizer, never desktop stability."""
import argparse
import json
from pathlib import Path
import re
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture
from verify_opengl_capture import validate_opengl_rendering


ARMED = 'EXIT_LIFETIME_ARMED nativeFence=true observed=57 lateLibraryFinalizer=true'
BEGIN = 'EXIT_LIFETIME_FINALIZER_BEGIN processExit=true'
COMPLETE = 'EXIT_LIFETIME_FINALIZER_COMPLETE observed=57 mappingLive=true'
GL_COMPLETE = 'OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false'
OBJECT_EXIT = 'KERNEL_OBJECT_EXIT held=0 active=0 cleanup=true'
APERTURE_EXIT = 'KERNEL_APERTURE_EXIT activeLeases=0 activeCpuBytes=0 mmioReads=0 mmioWrites=0 unloaded=true'


def _lifecycle(report, guest, records, control):
    require(exact(report, liveNvidiaLinuxUmdLoaded=True, realWslDxgForwarding=False,
                  capturedPrivateFixturesUsed=False), 'Wrong native runtime provenance')
    require(any(exact(r, nativeSynchronizationCreated=True, type=5, flags=128,
                      globalSharedHandle=False) for r in records), 'Missing native monitored fence')
    require(all(type(v) is int and v == 0 for k, v in report['disconnectCleanup'].items()
                if k.startswith('failed') or k.endswith('Timeouts')), 'Native failure or timeout')
    lines = guest.splitlines()

    def once(line):
        require(lines.count(line) == 1, 'Missing or duplicate exit receipt: ' + line)
        return lines.index(line)

    require([line for line in lines if line.startswith('EXIT_LIFETIME_')] ==
            ([ARMED, BEGIN] if control else [ARMED, BEGIN, COMPLETE]), 'Wrong finalizer results')
    summary = [i for i, line in enumerate(lines) if line.startswith('LINUX_BRIDGE summary ')]
    require(len(summary) == 1, 'Missing bridge stop summary')
    require(once(ARMED) < once(GL_COMPLETE) < summary[0] < once(BEGIN) <
            once(OBJECT_EXIT) < once(APERTURE_EXIT), 'Wrong workload/stop/finalizer/kernel ordering')
    return lines, once


def validate_exit_lifetime(report, guest, records, *, allocation_slots=128, transport='virtio-port'):
    result = validate_kernel_aperture(report, guest, records, allocation_slots=allocation_slots, transport=transport)
    lines, once = _lifecycle(report, guest, records, control=False)
    retained = [line for line in lines if line.startswith('LINUX_BRIDGE processExitViewsRetained=')]
    require(len(retained) == 1, 'Missing or duplicate process-owned mapping receipt')
    match = re.fullmatch(r'LINUX_BRIDGE processExitViewsRetained=true pagingFences=(\d+) '
                         r'hwQueueFences=(\d+) syncFences=(\d+) allocationViews=(\d+) '
                         r'release=kernel-process-exit', retained[0])
    require(match is not None and all(int(n) > 0 for n in match.groups()), 'Invalid retained mapping counts')
    require(once(GL_COMPLETE) < once(retained[0]) < once(BEGIN) < once(COMPLETE) <
            once(OBJECT_EXIT) < once(APERTURE_EXIT) < once('BRIDGE_RUNTIME_EXIT=0'),
            'Mappings not retained through the late library finalizer')
    require('BRIDGE_RUNTIME_SIGNAL' not in guest, 'Unexpected guest signal')
    counts = dict(zip(('pagingFences', 'hwQueueFences', 'syncFences', 'allocationViews'), map(int, match.groups())))
    vm, cleanup = report['ownedQemuControl'], report['disconnectCleanup']
    require(vm['fenceMappingsReleasedAfterVmExit'] == sum(counts[k] for k in
                ('pagingFences', 'hwQueueFences', 'syncFences')) and
            vm['allocationMappingsReleasedAfterVmExit'] == counts['allocationViews'] and
            cleanup['cpuLocksReleasedAfterVmExit'] == counts['allocationViews'],
            'Process-owned views disagree with native post-VM retirement')
    result.update(lateNativeFenceFinalizerVerified=True, retainedProcessExitViews=counts,
                  nativeFenceValue=57, processExitMappingLifetimeVerified=True,
                  generalDesktopStabilityVerified=False)
    return result


def validate_exit_lifetime_control(report, guest, records):
    result = validate_opengl_rendering(report, guest, records)
    lines, once = _lifecycle(report, guest, records, control=True)
    require(not any(line.startswith('LINUX_BRIDGE processExitViewsRetained=') for line in lines),
            'Negative control unexpectedly retains mappings')
    require([line for line in lines if line.startswith('BRIDGE_RUNTIME_SIGNAL')] ==
            ['BRIDGE_RUNTIME_SIGNAL number=11 coreDumped=false'] and
            [line for line in lines if line.startswith('BRIDGE_RUNTIME_EXIT=')] == ['BRIDGE_RUNTIME_EXIT=128'],
            'Control did not reproduce the expected segmentation fault')
    require(once(BEGIN) < once('BRIDGE_RUNTIME_SIGNAL number=11 coreDumped=false') <
            once(OBJECT_EXIT) < once(APERTURE_EXIT) < once('BRIDGE_RUNTIME_EXIT=128'),
            'Control signal occurred outside the late finalizer')
    result.update(expectedLateFinalizerSegfaultVerified=True, signal=11, guestExit=128,
                  processExitMappingLifetimeVerified=False, generalDesktopStabilityVerified=False)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expected-old-mapping-failure', action='store_true')
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines()
               if line.startswith('{')]
    validator = validate_exit_lifetime_control if args.expected_old_mapping_failure else validate_exit_lifetime
    result = validator(report, guest, records)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
