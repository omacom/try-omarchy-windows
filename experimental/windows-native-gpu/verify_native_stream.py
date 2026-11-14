#!/usr/bin/env python3
"""Verify native 4K reference retirement, GPU timestamps and two audit captures."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import subprocess
from verify_4k_capture import validate_oracle_frames


def require(condition, message):
    if not condition:
        raise ValueError(message)


def integer(record, key, minimum=0):
    value = record.get(key)
    require(type(value) is int and value >= minimum, 'Invalid integer: ' + key)
    return value


def validate_records(report, host, oracle, exit_code):
    require(type(exit_code) is int and exit_code == 0, 'Native process did not exit normally')
    require(report.get('nativeStreamCompleted') is True and report.get('guestBridge') is False, 'Wrong experiment')
    require(integer(report, 'width') == 3840 and integer(report, 'height') == 2160, 'Wrong resolution')
    count = integer(report, 'measuredFrames', 60)
    require(count <= 3600 and integer(report, 'warmupFrames') == 60, 'Wrong frame budget')
    total = count + 62
    require(integer(report, 'submitted') == total and integer(report, 'retired') == total, 'Unretired GPU work')
    starts = [r for r in host if r.get('nativeStreamStarted') is True]
    cleanups = [r for r in host if r.get('nativeStreamCleanup') is True]
    binaries = [r for r in host if r.get('nativeBinaryCapture') is True]
    require(len(host) == 4 and len(starts) == 1 and len(cleanups) == 1 and len(binaries) == 2, 'Missing or duplicated host receipts')
    start, cleanup = starts[0], cleanups[0]
    for key, expected in (('frames', count), ('warmup', 60), ('width', 3840), ('height', 2160), ('vendor', 0x10de)):
        require(integer(start, key) == expected, 'Startup mismatch: ' + key)
    require(start.get('guestBridge') is False and cleanup.get('gpuRetired') is True and integer(cleanup, 'submitted') == total,
            'Cleanup mismatch')
    gpu_frequency = integer(report, 'gpuTimestampFrequency', 1)
    cpu_frequency = integer(report, 'qpcFrequency', 1)
    require(integer(start, 'gpuTimestampFrequency') == gpu_frequency and integer(start, 'qpcFrequency') == cpu_frequency,
            'Frequency mismatch')
    frames = report.get('frames')
    require(type(frames) is list and len(frames) == total, 'Incomplete frame records')
    previous_gpu = previous_qpc = 0
    durations, observed_counts = [], set()
    visible = 0
    validate_oracle_frames(oracle)
    for i, frame in enumerate(frames):
        audit = i >= total - 2
        require(integer(frame, 'frame') == i and frame.get('audit') is audit, 'Wrong frame sequence/audit')
        require(integer(frame, 'rotation') == (i - total + 2 if audit else (i // 60) % 2), 'Wrong shader parameters')
        require(integer(frame, 'backBuffer') in (0, 1), 'Wrong buffer index')
        require(type(frame.get('presentHresult')) is int and frame['presentHresult'] == 0 and
                type(frame.get('countHresult')) is int and frame['countHresult'] == 0 and
                integer(frame, 'presentCount') == i + 1, 'Failed or missing presentation submission')
        before, after = integer(frame, 'presentCallQpcBefore', 1), integer(frame, 'presentCallQpcAfter', 1)
        require(previous_qpc <= before <= after, 'Nonmonotonic Present timestamps')
        previous_qpc = after
        visibility = frame.get('visibility', {})
        for key in ('windowVisible', 'windowMinimized', 'centerOwned'):
            require(type(visibility.get(key)) is bool, 'Missing typed visibility: ' + key)
        visible += visibility['windowVisible'] and not visibility['windowMinimized'] and visibility['centerOwned']
        require(type(frame.get('statisticsHresult')) is int, 'Missing statistics result')
        stats = integer(frame, 'statisticsPresentCount')
        sync_qpc = integer(frame, 'syncQpc')
        require(stats <= i + 1, 'Statistics beyond submitted frame')
        if frame['statisticsHresult'] == 0 and stats > 0 and sync_qpc > 0:
            observed_counts.add(stats)
        if not audit:
            begin, end = integer(frame, 'gpuStartTicks', 1), integer(frame, 'gpuEndTicks', 1)
            require(previous_gpu <= begin < end, 'Invalid or nonmonotonic GPU timestamps')
            previous_gpu = end
            if i >= 60:
                durations.append((end - begin) * 1000.0 / gpu_frequency)
        else:
            index = i - total + 2
            receipt = binaries[index]
            require(integer(frame, 'fnv1a') == oracle[index]['fnv1a'] and
                    integer(receipt, 'fnv1a') == oracle[index]['fnv1a'] and integer(receipt, 'round') == index + 1 and
                    integer(receipt, 'bytes') == 33177600 and receipt.get('file') == f'native-backbuffer-{index+1}.rgba',
                    'Capture/oracle mismatch')
    ordered = sorted(durations)
    # The renderer records the upper middle sample; also expose the statistical median.
    median, p95 = ordered[count // 2], ordered[(count * 95 - 1) // 100]
    wall_begin, wall_end = integer(report, 'wallStartQpc', 1), integer(report, 'wallEndQpc', 1)
    require(wall_begin <= frames[60]['presentCallQpcBefore'] and wall_end >= frames[total-3]['presentCallQpcAfter'],
            'Timed loop does not cover measured frames')
    seconds = (wall_end - wall_begin) / cpu_frequency
    for key, expected in (('gpuClearDrawMedianMs', median), ('gpuClearDrawP95Ms', p95), ('pacedWallSeconds', seconds)):
        value = report.get(key)
        require(type(value) in (int, float) and math.isfinite(value) and math.isclose(value, expected, rel_tol=1e-12, abs_tol=1e-12),
                'Timing summary mismatch: ' + key)
    return {'nativeReferenceSubmissionVerified': True, 'auditPixelsVerified': True,
            'guestBridgeVerified': False, 'allFramesDisplayedVerified': False, 'nativePerformanceRatioVerified': False,
            'width': 3840, 'height': 2160, 'measuredFrames': count, 'warmupFrames': 60, 'submittedAndRetired': total,
            'gpuClearDrawUpperMedianMs': median, 'gpuClearDrawMedianMs': statistics.median(durations), 'gpuClearDrawP95Ms': p95,
            'pacedWallSeconds': seconds, 'unobscuredCenterSamples': visible, 'visibilitySamples': total,
            'distinctStatisticsCounts': len(observed_counts), 'maximumStatisticsCount': max(observed_counts, default=0),
            'auditFrames': oracle}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def records(path):
    return [json.loads(line, object_pairs_hook=unique_object) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    paths = [args.directory / f'native-backbuffer-{i}.rgba' for i in (1, 2)]
    reports = records(args.directory / 'stream.jsonl')
    require(len(reports) == 1, 'Missing or duplicate stream report')
    run = subprocess.run([str(args.oracle.resolve()), *[str(p.resolve()) for p in paths]],
                         check=True, capture_output=True, text=True, timeout=60)
    oracle = [json.loads(line, object_pairs_hook=unique_object) for line in run.stdout.splitlines()]
    result = validate_records(reports[0], records(args.directory / 'host.jsonl'), oracle,
                              int((args.directory / 'exit.txt').read_text(encoding='utf-8-sig').strip()))
    result['sourceFiles'] = {p.name: {'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                             for p in paths + [args.directory / name for name in ('stream.jsonl', 'host.jsonl', 'exit.txt')]}
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(result, output, indent=2); output.write('\n')
    print(json.dumps({key: value for key, value in result.items() if key not in ('auditFrames', 'sourceFiles')}))


if __name__ == '__main__':
    main()
