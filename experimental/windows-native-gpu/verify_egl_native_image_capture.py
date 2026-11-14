"""Audit owned EGL DMA-BUF image storage and shader pixels; never a desktop gate."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from triangle_evidence import WIDTH, HEIGHT, TOLERANCE, checksum, triangle_reference
from verify_4k_capture import exact, require
from verify_kernel_aperture_capture import validate_kernel_aperture


def validate_egl_native_images(report, guest, records, *, enabled=True):
    require(type(enabled) is bool, 'Explicit native image profile required')
    result = validate_kernel_aperture(report, guest, records,
        transport='authenticated-broker', contract_checks=583)
    require(exact(report, liveNvidiaLinuxUmdLoaded=True, realWslDxgForwarding=False,
        capturedPrivateFixturesUsed=False), 'Wrong live GPU provenance')
    lines = guest.splitlines()

    def once(line):
        require(lines.count(line) == 1, 'Missing or duplicated receipt: ' + line)
        return lines.index(line)

    def match(pattern):
        found = re.findall(r'^' + pattern + r'[ \t\r]*$', guest, re.M)
        require(len(found) == 1, 'Missing, duplicate or malformed receipt: ' + pattern)
        return found[0]

    require(not any(s in guest for s in ('EGL_NATIVE_IMAGE_FAILURE', 'EGL_NATIVE_IMAGE_PIXEL_MISMATCH',
        'EGL_NATIVE_IMAGE_COLOR_ERROR', 'NATIVE_BUFFER_CONTRACT_FAIL', 'Oops:', 'BUG:', 'cleanup=false')),
        'Image, storage or kernel failure')
    clean, vm = report['disconnectCleanup'], report['ownedQemuControl']
    require(all(type(v) is int and v == 0 for k, v in clean.items()
        if k.startswith('failed') or k.endswith('Timeouts')), 'Native failure or timeout')
    require(all(vm[k] == 0 for k in ('fenceMappingsReleasedAfterVmExit',
        'allocationMappingsReleasedAfterVmExit', 'allocationSlotMappingsReleasedAfterVmExit')),
        'Mappings survived until forced process/VM teardown')
    once('BROKER_RENDER_CLIENT uid=65534 gid=65534 effectiveCapabilities=0 noNewPrivs=true controllerOpen=denied nativePortOpen=denied')
    once(f'NATIVE_BUFFER_CAPS_CONTRACT_PASS checks=9 enabled={str(enabled).lower()} readOnly=true gpu=false')
    require('EGL_NATIVE_IMAGE_CLEANUP failed=' not in guest, 'In-process cleanup failed')

    if not enabled:
        once('NATIVE_BUFFER_CONTRACT disabled=true gpu=false')
        once('EGL_NATIVE_IMAGE_DISABLED kernelEnabled=false importAbsent=true exportAbsent=true desktop=false')
        once('EGL_NATIVE_IMAGE_CLEANUP enabled=false created=0 released=0 held=0 bytes=0 contextsDestroyed=true beforeProcessExit=true')
        require(not any(s in guest for s in ('EGL_NATIVE_IMAGE_BEGIN ', 'EGL_NATIVE_IMAGE_COMPLETE ',
            'EGL_NATIVE_IMAGE_PIXEL_ROW ', 'KERNEL_NATIVE_BUFFER_EXIT ')), 'Disabled image path used')
        result.update(eglNativeImageVerified=False, disabledImageControlVerified=True,
            gbmVerified=False, desktopVerified=False)
        return result

    contract = once('NATIVE_BUFFER_CONTRACT_PASS checks=200 dmaBuf=true opaque=true cpuAccess=false foreignDma=false gpu=false')
    width, height, dxgi, fourcc = match(r'EGL_NATIVE_IMAGE_BEGIN width=(\d+) height=(\d+) dxgi=(\d+) fourcc=([0-9a-f]{8}) implicitOpaque=true desktop=false')
    width, height, dxgi = int(width), int(height), int(dxgi)
    require(WIDTH <= width <= 4096 and HEIGHT <= height <= 4096 and
        (dxgi, fourcc) in ((28, '34324241'), (28, '34324258'), (87, '34325241'), (87, '34325258')),
        'Unsupported or mismatched native texture metadata')
    begin = once(f'EGL_NATIVE_IMAGE_BEGIN width={width} height={height} dxgi={dxgi} fourcc={fourcc} implicitOpaque=true desktop=false')
    once('EGL_NATIVE_IMAGE_FORMATS rgba=true rgbx=true bgra=true bgrx=true explicitModifiers=0 linearAdvertised=false')
    once('EGL_NATIVE_IMAGE_PREPARATION invalidRejected=5 duplicateRejected=true perThread=true')
    once('LINUX_BRIDGE nativeInteropCaps=true formats=7 maxWidth=4096 maxHeight=4096 maxLogicalBytes=67108864 implicitOpaque=true')
    once(f'LINUX_BRIDGE nativeInteropResource=true threadOwned=true width={width} height={height} format={dxgi}')
    producer = once('EGL_NATIVE_IMAGE_PRODUCER dxgiClear=red transitionCommon=true gpuRetired=true implicitSync=false')
    rejects = ['rawCapability', 'foreignFd', 'stride', 'offset', 'dimensions', 'format', 'linearModifier']
    for kind in rejects:
        once(f'EGL_NATIVE_IMAGE_REJECT kind={kind} error=3003 inputFdRetained=true')
    require(sum(l.startswith('EGL_NATIVE_IMAGE_REJECT ') for l in lines) == len(rejects), 'Unexpected rejection case')
    imported = once('EGL_NATIVE_IMAGE_IMPORTED nativeFdClosed=true inputDmaBufFdClosed=true retainedDmaBufReference=true')
    exported = once(f'EGL_NATIVE_IMAGE_EXPORT query=1 fourcc={fourcc} planes=1 modifier=00ffffffffffffff export=1 validFd=true stride={width*4} offset=0')
    phases = []
    for phase, rgba in (('nativeProducerRead', bytes((255, 0, 0, 255))), ('reimportedRead', bytes((0, 255, 0, 255)))):
        digest = checksum(rgba * (width * height))
        receipt = f'EGL_NATIVE_IMAGE_PIXELS phase={phase} rgba={rgba.hex()} pixels={width * height} bands={(height+511)//512} fnv1a={digest} all=true'
        phases.append(once(receipt))
    lifetime = once('EGL_NATIVE_IMAGE_LIFETIME sourceReleased=true firstImageDestroyed=true importedTextureSiblingAlive=true')
    shader = once(f'EGL_NATIVE_IMAGE_SHADER width=130 height=73 viewport=130x73 texture={width}x{height} referenceFrame=2 topOrigin=true independentPixelAudit=pending')
    rows = re.findall(r'^EGL_NATIVE_IMAGE_PIXEL_ROW y=(\d+) rgba=([0-9a-f]+)[ \t\r]*$', guest, re.M)
    require(len(rows) == HEIGHT and guest.count('EGL_NATIVE_IMAGE_PIXEL_ROW ') == HEIGHT, 'Missing or duplicated image pixel row')
    data = bytearray()
    for y, (row, rgba) in enumerate(rows):
        require(row == str(y) and len(rgba) == WIDTH * 8, 'Malformed or reordered image pixels')
        data.extend(bytes.fromhex(rgba))
    reference, coverage = triangle_reference(2)
    maximum = 0
    for index, (observed, expected) in enumerate(zip(data, reference)):
        error = abs(observed-expected)
        require(error <= (TOLERANCE if coverage[index//4] and index % 4 < 3 else 0), 'Independent image shader pixel mismatch')
        maximum = max(maximum, error)
    complete = once('EGL_NATIVE_IMAGE_COMPLETE import=true export=true reimport=true nativeProducerPixels=true sharedShaderPixels=pending desktop=false gbm=false')
    released = once('EGL_NATIVE_IMAGE_CLEANUP enabled=true created=73 released=73 held=0 bytes=0 contextsDestroyed=true beforeProcessExit=true')
    kernel = once('KERNEL_NATIVE_BUFFER_EXIT held=0 bytes=0 created=73 released=73 cleanup=true')
    require(contract < begin < producer < imported < phases[0] < exported < phases[1] < lifetime < shader < complete < released < kernel,
        'Invalid allocation/import/storage/cleanup ordering')
    require(shader < lines.index('EGL_NATIVE_IMAGE_PIXEL_ROW y=0 rgba='+rows[0][1]) <
        lines.index('EGL_NATIVE_IMAGE_PIXEL_ROW y=72 rgba='+rows[-1][1]) < complete, 'Pixels outside shader readback')
    sync = tuple(map(int, match(r'KERNEL_SYNC_FILE_EXIT held=0 pending=0 created=(\d+) signaled=(\d+) errors=0 released=(\d+) cleanup=true')))
    require(sync[0] == sync[1] == sync[2] > 16, 'Missing live signal-budget regression/cleanup')
    require(exact(clean, acceptedHwQueueSignals=sync[0]+1, completedHwQueueSignals=sync[0]+1,
        pendingHwQueueSignals=0, sharedResourceProbes=1, importedSharedResources=1, failedSharedResourceProbes=0),
        'Native signal or texture metadata mismatch')
    descriptions = [r for r in records if r.get('nativeSharedResourceImportProbe') is True]
    require(len(descriptions) == 1 and exact(descriptions[0], verified=True, width=width, height=height,
        format=dxgi, dimension=3, flags=1, sharedNtHandleClosed=True, importedResourceReleased=True),
        'Actual Windows resource differs from declared texture')
    result.update(eglNativeImageVerified=True, dmaBufVerified=True, nativeImageWidth=width,
        nativeImageHeight=height, dxgiFormat=dxgi, drmFourcc=fourcc, implicitOpaqueLayout=True,
        explicitLinearLayoutAdvertised=False, rejectedImports=rejects, verifiedStoragePixels=width*height*2,
        shader={'width': WIDTH, 'height': HEIGHT, 'verifiedPixels': WIDTH*HEIGHT,
            'maximumChannelError': maximum, 'sha256': hashlib.sha256(data).hexdigest(), 'fnv1a': checksum(data)},
        nativeBufferReferencesReleasedBeforeProcessExit=True, gbmVerified=False,
        drmDeviceVerified=False, desktopVerified=False, openGl4kVerified=False,
        nearNativePerformanceVerified=False)
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
    result = validate_egl_native_images(report, guest, records, enabled=not args.disabled)
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, indent=2); stream.write('\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
