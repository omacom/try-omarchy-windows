"""Independently audit the bounded OpenGL probe, never desktop/API conformance."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from triangle_evidence import WIDTH, HEIGHT, TOLERANCE, checksum, triangle_reference
from test_owned_runtime_qemu import async_submissions_complete
from verify_4k_capture import LIVE_COUNTERS, exact, require


def validate_opengl_cleanup(report, records, *, allocation_slots=128):
    require(type(allocation_slots) is int and allocation_slots in (128, 256), 'Unsupported explicit allocation layout')
    cleanup, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(exact(cleanup, driverCleanupVerified=True, cleanupFailures=0, pendingNativeSubmissions=0,
            pendingHwQueueSignals=0, **{'live'+k: 0 for k in LIVE_COUNTERS}), 'Native cleanup not verified')
    require(all(type(v) is int and v == 0 for k, v in cleanup.items() if k.startswith('live')), 'Invalid live counter')
    require(exact(vm, ownedQemuExited=True, qemuExit=0, qemuForcedStop=False, fenceControlFailed=False,
            liveFenceMappings=0, liveAllocationMappings=0, liveAllocationMappedBytes=0, liveAllocationSlotMappings=0,
            fenceApertureSlots=128, allocationApertureSlots=allocation_slots), 'Owned VM cleanup not verified')
    for created, acknowledged, after_exit in (
            ('fenceMappingsCreated', 'fenceUnmapAcknowledgements', 'fenceMappingsReleasedAfterVmExit'),
            ('allocationMappingsCreated', 'allocationUnmapAcknowledgements', 'allocationMappingsReleasedAfterVmExit'),
            ('allocationSlotMappingsCreated', 'allocationSlotUnmapAcknowledgements', 'allocationSlotMappingsReleasedAfterVmExit')):
        require(all(type(vm.get(k)) is int and vm[k] >= 0 for k in (created, acknowledged, after_exit)) and
                0 < vm[created] == vm[acknowledged] + vm[after_exit], 'Unaccounted mapping retirement')
    for key, marker in (('disconnectCleanup', 'driverCleanupVerified'), ('ownedQemuControl', 'ownedQemuExited')):
        actual = [r for r in records if marker in r]
        require(len(actual) == 1 and json.dumps(report[key], sort_keys=True) == json.dumps(actual[0], sort_keys=True),
                'Report/cleanup receipt mismatch')


def validate_opengl_rendering(report, guest, records, *, allocation_slots=128, transport='virtio-port'):
    """Audit the actual draws and native/VM cleanup independently of guest exit.

    A caller testing an intentional process crash must validate that signal and
    exit separately. Normal success must use validate_opengl below.
    """
    require(exact(report, runtimeWorkload='opengl', hostBridgeExit=0), 'Wrong workload or host exit')
    validate_opengl_cleanup(report, records, allocation_slots=allocation_slots)
    profile = [r for r in records if 'openGlProfileNegotiated' in r]
    require(len(profile) == 1 and (exact(profile[0], version=1) or
            exact(profile[0], version=2, objectLimit=384, hwQueueLimit=16, vendorObjectLimit=192)) and
            exact(profile[0], openGlProfileNegotiated=True, frames=2,
            commandLimit=64, pendingCommands=16, reservationLimit=16, reservedByteLimit=17179869184,
            requestLimit=10000), 'Missing bounded OpenGL profile')
    for key, marker in (('nativeAsyncSubmissionAcceptances', 'nativeAsyncSubmissionAccepted'),
                        ('nativeAsyncSubmissionRetirements', 'nativeAsyncSubmissionRetired')):
        actual = [r for r in records if marker in r]
        require(json.dumps(report[key], sort_keys=True) == json.dumps(actual, sort_keys=True),
                'Report/native command receipt mismatch')
        for record in actual:
            require(all(type(record.get(k)) is int for k in ('index', 'bytes', 'privateBytes', 'target', 'observed')),
                    'Invalid native command counter type')
    for record in report['guestCommandEnqueues']:
        require(all(type(record.get(k)) is int for k in ('bytes', 'privateBytes', 'target', 'observedAtReturn')),
                'Invalid guest command counter type')
    require(async_submissions_complete(report['guestCommandEnqueues'], report['nativeAsyncSubmissionAcceptances'],
            report['nativeAsyncSubmissionRetirements'], report['disconnectCleanup'], maximum=64),
            'Unretired or unmatched native command')
    transports = {'virtio-port': 'LINUX_BRIDGE transport=virtio-port',
                  'authenticated-broker': 'LINUX_BRIDGE transport=authenticated-broker nativeWorkerClients=1'}
    require(transport in transports and
            [l for l in guest.splitlines() if l.startswith('LINUX_BRIDGE transport=')] == [transports[transport]],
            'Missing or mixed explicit guest transport')
    require('realDxgForwarding=false' in guest and
            'LINUX_BRIDGE nvidiaUmdPresentDuringPrivateQuery=true' in guest and
            'LINUX_BRIDGE openGlProfileNegotiated=true frames=2 commandLimit=64 reservationLimit=16 reservedByteLimit=17179869184' in guest,
            'Missing genuine runtime/transport provenance')
    require(re.search(r'^LINUX_BRIDGE contextInput node=\d+ engine=1 flags=16 clientHint=16 privateBytes=[1-9]\d*[ \t\r]*$', guest, re.M),
            'Missing real GLON12 context request')
    require(any(exact(r, nativeSynchronizationCreated=True, type=5, flags=flags, globalSharedHandle=False)
            for r in records for flags in (131, 1155)), 'Missing native NT-shareable fence creation')
    def matches(pattern):
        return re.findall(r'^' + pattern + r'[ \t\r]*$', guest, re.M)
    require(guest.splitlines().count('OPENGL_TEST_BEGIN api=OpenGL requested=3.3 width=130 height=73 frames=2 display=false') == 1,
            'Wrong OpenGL probe profile')
    require(guest.splitlines().count('OPENGL_TEST_COMPLETE frames=2 independentPixelAudit=pending desktop=false') == 1,
            'Incomplete guest probe')
    require('OPENGL_FAILURE' not in guest and 'OPENGL_SHADER_FAILURE' not in guest and 'Removing Device' not in guest,
            'Guest OpenGL failure')
    renderer = matches(r'OPENGL_IDENTITY renderer=(.*)')
    version = matches(r'OPENGL_VERSION (.*)')
    require(len(renderer) == 1 and 'D3D12' in renderer[0] and 'NVIDIA' in renderer[0] and
            not any(x in renderer[0].lower() for x in ('llvmpipe', 'softpipe', 'software')), 'Software or wrong renderer')
    require(len(version) == 1 and re.fullmatch(r'(?:3\.[3-9]|[4-9]\.\d+) \(Core Profile\) Mesa [0-9.]+', version[0]),
            'Wrong desktop OpenGL version/profile')
    require(matches(r'OPENGL_FRAME frame=(\d+) vertices=(\d+) error=([0-9a-f]{4})') == [('1', '3', '0000'), ('2', '3', '0000')],
            'Missing draw or GL error')
    rows = matches(r'OPENGL_PIXEL_ROW frame=(\d+) y=(\d+) rgba=([0-9a-f]+)')
    require(len(rows) == 2 * HEIGHT, 'Missing or duplicate GPU pixel row')
    audited, previous = [], None
    for frame in (1, 2):
        data = bytearray()
        for y, (f, row, rgba) in enumerate(rows[(frame - 1) * HEIGHT:frame * HEIGHT]):
            require((f, row) == (str(frame), str(y)) and len(rgba) == WIDTH * 8, 'Reordered or malformed GPU pixel row')
            data.extend(bytes.fromhex(rgba))
        reference, coverage = triangle_reference(frame)
        maximum = 0
        for index, (observed, expected) in enumerate(zip(data, reference)):
            error = abs(observed - expected)
            require(error <= (TOLERANCE if coverage[index // 4] and index % 4 < 3 else 0), 'Independent GPU pixel mismatch')
            maximum = max(maximum, error)
        require(previous != data, 'Repeated OpenGL frame')
        previous = data
        draw = guest.index(f'OPENGL_FRAME frame={frame} ')
        first = guest.index(f'OPENGL_PIXEL_ROW frame={frame} y=0 ')
        last = guest.index(f'OPENGL_PIXEL_ROW frame={frame} y=72 ')
        end = guest.index('OPENGL_FRAME frame=2 ') if frame == 1 else guest.index('OPENGL_TEST_COMPLETE ')
        require(draw < first < last < end, 'Incorrect frame/readback ordering')
        audited.append({'frame': frame, 'width': WIDTH, 'height': HEIGHT, 'verifiedPixels': WIDTH * HEIGHT,
                        'trianglePixels': sum(coverage), 'maximumChannelError': maximum,
                        'sha256': hashlib.sha256(data).hexdigest(), 'fnv1a': checksum(data),
                        'referenceFnv1a': checksum(reference)})
    return {'schema': 1, 'boundedOpenGlShaderVerified': True, 'desktopVerified': False,
            'openGlConformanceVerified': False, 'openGl4kVerified': False, 'nearNativePerformanceVerified': False,
            'renderer': renderer[0], 'version': version[0], 'frames': audited,
            'retiredNativeCommands': len(report['nativeAsyncSubmissionRetirements'])}


def validate_opengl(report, guest, records, *, allocation_slots=128, transport='virtio-port'):
    require(re.findall(r'^BRIDGE_RUNTIME_EXIT=(\d+)[ \t\r]*$', guest, re.M) == ['0'],
            'Guest runtime did not finish normally')
    require('BRIDGE_RUNTIME_SIGNAL' not in guest, 'Unexpected guest runtime signal')
    return validate_opengl_rendering(report, guest, records, allocation_slots=allocation_slots, transport=transport)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    result = validate_opengl(report, guest, records)
    with args.output.open('x', encoding='utf-8') as destination:
        json.dump(result, destination, indent=2); destination.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
