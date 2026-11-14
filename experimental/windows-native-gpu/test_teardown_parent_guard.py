"""Linux GitHub CI: compare mock teardown faults against the previous header."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess


def run(command, cwd, prefix):
    process = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=180)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        stdout, stderr = process.communicate()
        prefix.with_suffix('.stdout.txt').write_bytes(stdout)
        prefix.with_suffix('.stderr.txt').write_bytes(stderr)
        raise RuntimeError('Owned CPU subprocess timed out: ' + command[0])
    prefix.with_suffix('.stdout.txt').write_bytes(stdout)
    prefix.with_suffix('.stderr.txt').write_bytes(stderr)
    return process.returncode, stdout.decode('utf-8', errors='replace'), stderr.decode('utf-8', errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-header', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_OS') != 'Linux':
        parser.error('Requires the Linux GitHub Actions CPU runner')
    # The expected baseline abort must not leave a core dump on the runner.
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    root = Path(os.environ['RUNNER_TEMP']).resolve(strict=True)
    output = args.output.resolve()
    if not output.is_relative_to(root) or output == root:
        parser.error('Output must be an owned child of RUNNER_TEMP')
    output.mkdir()
    baseline = args.baseline_header.resolve(strict=True)
    source = Path(__file__).resolve().parent
    results = {}
    for name in ('baseline', 'candidate'):
        fixture = output / name
        fixture.mkdir()
        for header in source.rglob('*.h'):
            if header.is_symlink():
                raise RuntimeError('Source header symlink is not permitted')
            destination = fixture / header.relative_to(source)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(header, destination)
        shutil.copy2(source / 'driver_wire_test.cpp', fixture / 'driver_wire_test.cpp')
        if name == 'baseline':
            shutil.copy2(baseline, fixture / 'driver_wire.h')
        executable = fixture / 'wire-test'
        code, _, _ = run(['c++', '-std=c++17', '-O0', '-Wall', '-Wextra', '-Werror',
                          str(fixture / 'driver_wire_test.cpp'), '-o', str(executable)],
                         fixture, output / (name + '-build'))
        if code:
            raise RuntimeError(name + ' CPU fixture failed to compile; this is not a regression result')
        code, stdout, stderr = run([str(executable)], fixture, output / (name + '-run'))
        if name == 'baseline':
            if not code or 'TEARDOWN_PARENT_GUARD_REPRO:' not in stderr:
                raise RuntimeError('Previous header did not reproduce the targeted parent-destruction fault')
        elif code or 'TEARDOWN_PARENT_GUARD_PASS cases=26 ' not in stdout or 'PASS: WDDM wire ownership' not in stdout:
            raise RuntimeError('Candidate CPU controls failed')
        results[name] = dict(exitCode=code, headerSha256=hashlib.sha256(
            (fixture / 'driver_wire.h').read_bytes()).hexdigest())
    result = dict(schema=1, sourceCommit=os.environ.get('SOURCE_COMMIT'),
                  headCommit=os.environ.get('SOURCE_HEAD_COMMIT'),
                  baselineCommit='0a2dea044fdbb429055077c939f88d99eee39547',
                  fixtureSha256=hashlib.sha256((source / 'driver_wire_test.cpp').read_bytes()).hexdigest(),
                  results=results, baselineFaultReproduced=True, candidateControlsPassed=26,
                  mockDriverOnly=True, nativeKmtCallsExecuted=False,
                  gpuWorkloadRun=False, vmLaunched=False, hardwareRecoveryVerified=False,
                  dailyUseAccepted=False)
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8', newline='\n')
    print('TEARDOWN_PARENT_COMPARISON_PASS baselineFaultReproduced=true candidateCases=26 mock=true gpu=false')


if __name__ == '__main__':
    main()
