"""Ungated coordinator invocations must fail before native GPU/VM work."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', required=True)
    args = parser.parse_args()
    cases = [
        ['--stdio','--driver-application-objects'],
        ['--stdio','--driver-application-objects','--driver-application-objects'],
        ['--stdio','--driver-application-memory','--driver-application-objects'],
        ['--stdio','--driver-application-memory'],
        ['--stdio','--driver-application-memory','--driver-application-memory'],
        ['--stdio','--driver-actor-slots','8','--driver-resource-reference-slots','32','--driver-application-memory'],
        ['--stdio','--driver-mapped-residency-admission'],
        ['--stdio','--driver-mapped-residency-admission','--driver-mapped-residency-admission'],
        ['--stdio','--driver-actor-slots','8','--driver-resource-reference-slots','32','--driver-mapped-residency-admission'],
        ['--stdio','--driver-resource-reference-slots','32'],
        ['--stdio','--driver-resource-reference-slots','16'],
        ['--stdio','--driver-resource-reference-slots','33'],
        ['--stdio','--driver-resource-reference-slots','32','--driver-resource-reference-slots','32'],
        ['--stdio','--driver-actor-slots','8','--driver-resource-reference-slots','32'],
        ['--stdio','--driver-actor-slots','8'],
        ['--stdio','--driver-actor-slots','4'],
        ['--stdio','--driver-actor-slots','9'],
        ['--stdio','--driver-actor-slots','8','--driver-actor-slots','8'],
        ['--stdio', '--driver-window-input'],
        ['--stdio', '--driver-input-self-test'],
        ['--stdio', '--driver-window-input', '--driver-window-input'],
        ['--stdio', '--driver-input-self-test', '--driver-input-self-test'],
        ['--listen', '0', '--driver-window-input'],
        ['--stdio', '--driver-coordinator'],
        ['--listen', '0', '--driver-coordinator'],
        ['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'image', 'log', '--driver-coordinator'],
        ['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'image', 'log',
         '--driver-coordinator', '--driver-native-metadata', '--driver-present-shared'],
        ['--stdio', '--driver-coordinator', '--driver-coordinator'],
        ['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'image', 'log', '--driver-fence-slots', '512'],
        ['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'image', 'log', '--driver-coordinator', '--driver-fence-slots', '512'],
        ['--stdio', '--driver-cpu-slots', '1024'],
        ['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'image', 'log', '--driver-cpu-slots', '1024', '--driver-cpu'],
        ['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'image', 'log', '--driver-cpu-slots', '1024', '--driver-cpu', '--driver-coordinator', '--driver-fence-slots', '256'],
    ]
    for command in cases:
        result = subprocess.run([args.bridge, *command], input=b'', capture_output=True, timeout=10)
        if result.returncode == 0 or result.stdout or b'ownedQemuPid' in result.stderr or b'nativeTextureMetadata' in result.stderr:
            raise SystemExit(f'FAIL: coordinator startup guard {command}: {result.returncode}, {result.stderr!r}')
    print(f'PASS: {len(cases)} coordinator opt-in/duplicate guards reject before GPU/VM work')


if __name__ == '__main__':
    main()
