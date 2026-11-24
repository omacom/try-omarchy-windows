"""Audit actual compositor readback and known background regions, not display/full scene correctness."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from verify_hyprland_lifecycle_capture import require, validate as validate_lifecycle


def fnv(data):
    result = 14695981039346656037
    for value in data:
        result = ((result ^ value) * 1099511628211) & ((1 << 64) - 1)
    return result


def _validate_readback(report, guest, records, frames, expected_rgb, *, retained_presenter=False):
    lifecycle = validate_lifecycle(report, guest)
    require(type(expected_rgb) is tuple and len(expected_rgb) == 3 and
            all(type(value) is int and 0 <= value <= 255 for value in expected_rgb), 'Invalid independent background reference')
    require('HYPRLAND_HOST_CAPTURE_FAILURE' not in guest, 'Guest capture failure')
    require(guest.count('HYPRLAND_HOST_CAPTURE_BEGIN outputBuffer=true nativeFence=true guestCpuMap=false windowsPresentation=false') == 2,
            'Missing/duplicate native capture handoff')
    replies = re.findall(r'^HYPRLAND_HOST_CAPTURE_RESULT result=0 errno=0 fnv1a=([0-9a-f]{16}) syncFileClosed=true eglSyncDestroyed=true windowsPresentation=false$', guest, re.M)
    require(len(replies) == 2 and guest.count('HYPRLAND_HOST_CAPTURE_RESULT ') == 2, 'Capture reply or source-FD cleanup mismatch')
    copied = [row for row in records if row.get('nativeCompositorCaptured') is True]
    binary = [row for row in records if row.get('nativeCompositorBinaryCapture') is True]
    released = [row for row in records if row.get('nativeCompositorCaptureReleased') is True]
    require(len(copied) == len(binary) == 2 and len(released) == (1 if retained_presenter else 2),
            'Incomplete native capture/copy/release sequence')
    require(all(row.get('submitted') is True and row.get('gpuWorkRetired') is True for row in released), 'Native capture resources released before retirement')
    clean = report['disconnectCleanup']
    require(clean.get('compositorCaptureAttempts') == clean.get('completedCompositorCaptures') == 2 and
            clean.get('failedCompositorCaptures') == 0, 'Native capture outcome mismatch')
    require(set(frames) == {'compositor-copy-1.raw', 'compositor-copy-2.raw'}, 'Unexpected capture files')
    accepted = []
    for index, (gpu, file, reply) in enumerate(zip(copied, binary, replies), 1):
        name = f'compositor-copy-{index}.raw'
        require(file.get('file') == name and file.get('frame') == gpu.get('frame') == index, 'Mixed/reordered native frames')
        for row in (gpu, file):
            require(all(type(row.get(key)) is int and row[key] == value for key, value in
                    dict(width=640, height=360, fourcc=875713112).items()), 'Unexpected compositor storage layout')
        require(gpu.get('dxgiFormat') == 87 and gpu.get('sourceReturnedToCommon') is True and
                gpu.get('cpuUpload') is False and gpu.get('windowsPresentation') is False, 'Wrong GPU copy/ownership route')
        require(gpu.get('backBufferReadback') is (True if retained_presenter else None), 'Wrong source/backbuffer readback route')
        target, observed, before = (gpu.get(key) for key in ('producerFenceTarget', 'producerFenceObserved', 'producerFenceBefore'))
        require(all(type(value) is int and 0 <= value < (1 << 64) - 1 for value in (target, observed, before)) and
                0 < target <= observed and gpu.get('copyFenceObserved') == (index if retained_presenter else 1),
                'Unretired producer/copy GPU dependency')
        pixels = frames[name]
        require(type(pixels) is bytes and len(pixels) == file.get('bytes') == 640 * 360 * 4, 'Wrong/truncated capture byte count')
        checksum = fnv(pixels)
        require(checksum == gpu.get('fnv1a') == file.get('fnv1a') == int(reply, 16), 'Native/guest/file pixel digest mismatch')
        known = 0
        # Two disjoint background regions avoid the startup banner and cursor.
        # The expected RGB comes from the pinned test configuration, never from
        # observed pixels. XRGB's unused X byte is retained but not treated as alpha.
        for low, high in ((32, 160), (280, 360)):
            for y in range(low, high):
                row = pixels[y * 640 * 4:(y + 1) * 640 * 4]
                for x in range(640):
                    require((row[x*4+2], row[x*4+1], row[x*4]) == expected_rgb, 'Independent compositor background mismatch')
                    known += 1
        accepted.append(dict(frame=index, file=name, width=640, height=360, byteCount=len(pixels),
            rawSha256=hashlib.sha256(pixels).hexdigest(), rawFnv1a=f'{checksum:016x}',
            knownBackgroundPixelsVerified=known, producerFenceTarget=target, producerFenceObserved=observed))
    return dict(schema=1, actualCompositorReadbackVerified=True, knownBackgroundRGB=list(expected_rgb),
        fullScenePixelCorrectnessVerified=False, windowsPresentationVerified=False, desktopVerified=False,
        nearNativePerformanceVerified=False, guestCpuMapUsed=False, cpuUploadUsed=False,
        lifecycle=lifecycle, frames=accepted)


def validate(report, guest, records, frames, expected_rgb):
    return _validate_readback(report, guest, records, frames, expected_rgb)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--expected-background-rgb', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(re.fullmatch('[0-9a-fA-F]{6}', args.expected_background_rgb) is not None, 'Expected six RGB hex digits')
    rgb = tuple(bytes.fromhex(args.expected_background_rgb))
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    frames = {f'compositor-copy-{index}.raw': (args.report.parent / f'compositor-copy-{index}.raw').read_bytes() for index in (1, 2)}
    result = validate(report, guest, records, frames, rgb)
    with args.output.open('x', newline='\n') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(result, indent=2))
