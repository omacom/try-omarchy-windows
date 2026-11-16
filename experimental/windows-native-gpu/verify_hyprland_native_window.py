"""Verify bounded actual-compositor native-window display, not launcher/product acceptance."""
import argparse
import json
from pathlib import Path
import re
from verify_hyprland_compositor_capture import _validate_readback, require


def validate(report, guest, records, frames, expected_rgb):
    readback = _validate_readback(report, guest, records, frames, expected_rgb, retained_presenter=True)
    opened = [row for row in records if 'nativeSharedTexturePresenterOpened' in row]
    presented = [row for row in records if 'nativeSharedTexturePresented' in row]
    released = [row for row in records if 'nativeSharedTexturePresenterReleased' in row]
    copied = [row for row in records if row.get('nativeCompositorCaptured') is True]
    capture_release = [row for row in records if row.get('nativeCompositorCaptureReleased') is True]
    require(len(opened) == len(released) == 1 and len(presented) == 2, 'Missing/duplicate retained native presenter')
    start = opened[0]
    require(all(start.get(key) is True for key in ('nativeSharedTexturePresenterOpened', 'compositorOutput',
            'flipModel', 'windowVisible', 'sameAdapter')) and start.get('cpuUpload') is False and
            all(type(start.get(key)) is int and start[key] == value for key, value in
                dict(width=640, height=360, clientWidth=640, clientHeight=360, bufferCount=2).items()),
            'Wrong native compositor window')
    require(all(released[0].get(key) is True for key in ('nativeSharedTexturePresenterReleased', 'gpuWorkRetired',
            'windowDestroyed', 'classUnregistered')), 'Native window release failure')
    order = [opened[0], copied[0], presented[0], copied[1], presented[1], released[0], capture_release[0]]
    require(all(records.index(left) < records.index(right) for left, right in zip(order, order[1:])),
            'Presenter/resource lifecycle order mismatch')
    previous_sync = previous_after = 0
    identity = None
    for index, (row, frame) in enumerate(zip(presented, readback['frames']), 1):
        require(all(row.get(key) is True for key in ('nativeSharedTexturePresented', 'compositorOutput',
                'dxgiStatisticsComplete', 'windowVisible')) and row.get('cpuUpload') is False and
                row.get('presentationResult') == 'dxgi-statistics-verified' and row.get('backBufferState') == 'PRESENT',
                'Unverified native compositor display')
        require(all(type(row.get(key)) is int and row[key] == value for key, value in
                dict(round=index, width=640, height=360, presentHresult=0, lastPresentCount=index,
                    statisticsHresult=0, statisticsPresentCount=index, dwmFlushHresult=0,
                    presentQueueFenceTarget=index).items()), 'Stale/failed display statistics')
        fields = ('presentProcessId', 'presentSwapChain', 'presentCallQpcBefore', 'presentCallQpcAfter',
                  'qpcFrequency', 'syncQpc', 'presentQueueFenceObserved', 'backBufferIndex', 'backBufferFnv1a')
        require(all(type(row.get(key)) is int for key in fields), 'Invalid display identity/timing types')
        require(0 < row['presentProcessId'] < 1 << 32 and 0 < row['presentSwapChain'] < 1 << 64 and
                0 < row['qpcFrequency'] < 1 << 63 and previous_after < row['presentCallQpcBefore'] <= row['presentCallQpcAfter'] < 1 << 63 and
                max(previous_sync, row['presentCallQpcBefore'] - 1) < row['syncQpc'] < 1 << 63 and
                index <= row['presentQueueFenceObserved'] < (1 << 64) - 1 and row['backBufferIndex'] in (0, 1) and
                row['backBufferFnv1a'] == int(frame['rawFnv1a'], 16), 'Unretired/stale/mismatched display evidence')
        current = tuple(row[key] for key in ('presentProcessId', 'presentSwapChain', 'qpcFrequency'))
        require(identity is None or current == identity, 'Native presenter identity changed')
        identity = current
        previous_sync, previous_after = row['syncQpc'], row['presentCallQpcAfter']
    require(presented[0]['backBufferIndex'] != presented[1]['backBufferIndex'], 'Backbuffer did not advance')
    return dict(schema=1, actualCompositorNativeWindowPresentationVerified=True,
        normalLauncherIntegrationVerified=False, displayStabilityVerified=False,
        fullScenePixelCorrectnessVerified=False, desktopVerified=False, nearNativePerformanceVerified=False,
        backBufferReadback=readback, presentProcessId=identity[0], presentSwapChain=identity[1],
        statisticsPresentCounts=[row['statisticsPresentCount'] for row in presented],
        syncQpc=[row['syncQpc'] for row in presented])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--expected-background-rgb', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(re.fullmatch('[0-9a-fA-F]{6}', args.expected_background_rgb) is not None, 'Expected six RGB hex digits')
    report = json.loads(args.report.read_text())
    guest = args.report.with_suffix('.log').read_text()
    records = [json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    frames = {f'compositor-copy-{index}.raw': (args.report.parent / f'compositor-copy-{index}.raw').read_bytes() for index in (1, 2)}
    result = validate(report, guest, records, frames, tuple(bytes.fromhex(args.expected_background_rgb)))
    with args.output.open('x', newline='\n') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps(result, indent=2))
