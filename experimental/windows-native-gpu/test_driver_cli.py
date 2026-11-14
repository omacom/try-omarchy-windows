"""Reject malformed aperture settings before native device or VM startup."""
import argparse
import pathlib
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--bridge', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if sys.platform != 'win32' or not args.bridge.is_file():
        parser.error('Run on Windows with the built bridge')
    cases = [(['--driver-cpu-slots', value], 'CPU aperture slots must be specified once')
             for value in ('0', '8', '17', '33', '65', '127', '129', '255', '257', '512', '1024', '-1', '4294967296', '32x')]
    cases += [(['--stdio','--driver-rootfs-readonly','C:/rootfs.ext4'],'Read-only root image requires')]
    cases += [(['--stdio','--driver-rootfs-readonly',''],'Read-only root image must be specified once')]
    cases += [(['--stdio','--driver-rootfs-readonly','C:/one.ext4','--driver-rootfs-readonly','C:/two.ext4'],
               'Read-only root image must be specified once')]
    cases += [(['--stdio','--driver-consume-shared-process'],'Shared GPU worker requires explicit consumption opt-in')]
    cases += [(['--driver-fence-slots', value], 'Fence aperture slots must be specified once')
              for value in ('0','32','65','127','129','255','257','513','-1','4294967296','128x')]
    cases += [(['--driver-fence-slots','512'],'512 fence slots require the explicit owned coordinator')]
    cases += [(['--driver-fence-slots','64','--driver-fence-slots','256'], 'specified once')]
    cases += [(['--stdio','--driver-fence-slots',value], 'Fence aperture capacity requires')
              for value in ('64','128','256')]
    cases += [(['--native-texture-worker',value,'0'],'worker handle')
              for value in ('0','-1','32x','18446744073709551615','18446744073709551616')]
    cases += [(['--driver-cpu-slots', '16', '--driver-cpu-slots', '32'], 'specified once'),
              (['--stdio', '--driver-cpu-slots', '32'], 'capacity requires'),
              (['--stdio', '--driver-cpu-slots', '256'], 'capacity requires')]
    cases += [(['--stdio', '--sync-eof-test'], 'Synchronization EOF control requires'),
              (['--run-qemu', 'qemu', 'firmware', 'kernel', 'image', 'log', '--driver-syncs',
                '--sync-eof-test', '--hwqueue-eof-test'], 'Synchronization EOF control requires')]
    owned = ['--run-qemu', 'qemu', 'firmware', 'kernel', 'image', 'log', '--driver-contexts',
             '--driver-queries', '--driver-allocations', '--driver-gpuva', '--driver-reservation']
    cases += [(['--stdio', '--driver-reservation'], 'GPU reservation requires'),
              (['--stdio', '--reservation-eof-test'], 'Reservation EOF control requires'),
              (['--stdio', '--driver-contexts', '--driver-queries', '--driver-allocations',
                '--driver-gpuva', '--driver-reservation'], 'GPU reservation requires'),
              (owned + ['--reservation-eof-test', '--sync-eof-test'], 'Reservation EOF control requires'),
              (owned + ['--reservation-eof-test', '--cpu-eof-test'], 'Reservation EOF control requires'),
              (owned + ['--reservation-eof-test', '--driver-submit'], 'Submission requires')]
    cases += [(['--stdio', '--driver-gpu-state'], 'GPU state mappings require'),
              (['--stdio', '--gpu-state-eof-test'], 'GPU state EOF control requires'),
              (owned + ['--gpu-state-eof-test'], 'GPU state EOF control requires')]
    cases += [(owned + ['--driver-gpu-state', '--gpu-state-eof-test', control], 'GPU state EOF control requires')
              for control in ('--reservation-eof-test', '--sync-eof-test', '--hwqueue-eof-test', '--cpu-eof-test', '--cpu-store-test')]
    cases += [(['--stdio', '--cpu-span-eof-test'], 'CPU span EOF control requires')]
    cases += [(owned + ['--driver-cpu', '--cpu-span-eof-test', control], 'CPU span EOF control requires')
              for control in ('--reservation-eof-test', '--sync-eof-test', '--hwqueue-eof-test', '--cpu-eof-test', '--cpu-store-test', '--gpu-state-eof-test')]
    cases += [(['--stdio', '--sync-no-max-eof-test'], 'NoSignalMaxValueOnTdr EOF control requires')]
    cases += [(owned + ['--driver-syncs', '--sync-no-max-eof-test', control], 'NoSignalMaxValueOnTdr EOF control requires')
              for control in ('--reservation-eof-test', '--sync-eof-test', '--hwqueue-eof-test', '--cpu-eof-test', '--cpu-store-test', '--gpu-state-eof-test', '--cpu-span-eof-test')]
    cases += [(owned + ['--driver-syncs', '--sync-no-max-eof-test', '--driver-submit'], 'Submission requires')]
    cases += [(['--stdio', '--hwqueue-no-broadcast-eof-test'], 'NoBroadcastSignal queue EOF control requires')]
    no_broadcast = owned + ['--driver-translation','--driver-hwqueues','--driver-syncs','--driver-cpu','--driver-residency',
                            '--driver-submit','--driver-retirement','--hwqueue-no-broadcast-eof-test']
    for missing in ('--driver-submit', '--driver-retirement'):
        cases.append(([o for o in no_broadcast if o != missing], 'NoBroadcastSignal queue EOF control requires'))
    cases += [(no_broadcast + [control], 'requires') for control in
              ('--hwqueue-eof-test','--cpu-eof-test','--cpu-store-test','--cpu-span-eof-test',
               '--sync-eof-test','--sync-no-max-eof-test','--reservation-eof-test','--gpu-state-eof-test')]
    wait_control = '--hwqueue-no-broadcast-wait-eof-test'
    cases += [(['--stdio', wait_control], 'NoBroadcastWait queue EOF control requires')]
    no_broadcast_wait = [wait_control if o == '--hwqueue-no-broadcast-eof-test' else o for o in no_broadcast]
    for missing in ('--driver-submit', '--driver-retirement'):
        cases.append(([o for o in no_broadcast_wait if o != missing], 'NoBroadcastWait queue EOF control requires'))
    cases += [(no_broadcast_wait + [control], 'requires') for control in
              ('--hwqueue-eof-test','--cpu-eof-test','--cpu-store-test','--cpu-span-eof-test',
               '--sync-eof-test','--sync-no-max-eof-test','--reservation-eof-test','--gpu-state-eof-test',
               '--hwqueue-no-broadcast-eof-test')]
    cases += [(['--stdio','--driver-async-submit'], 'Asynchronous submission requires')]
    for missing in ('--driver-submit','--driver-retirement'):
        cases.append(([o for o in no_broadcast if o not in (missing,'--hwqueue-no-broadcast-eof-test')] + ['--driver-async-submit'], 'Asynchronous submission requires'))
    cases += [(['--stdio','--driver-native-metadata'], 'Native texture metadata requires explicit shared resource opt-in')]
    cases += [(['--stdio','--driver-consume-shared'], 'Shared texture consumption requires shared resources, asynchronous submission and sync opt-ins')]
    cases += [(['--stdio','--driver-present-shared'], 'Shared texture presentation requires explicit consumption opt-in')]
    cases += [(['--stdio','--driver-shared-resources','--driver-present-shared'], 'Shared texture presentation requires explicit consumption opt-in')]
    cases += [(['--stdio','--driver-shared-resources','--driver-consume-shared'], 'Shared texture consumption requires shared resources, asynchronous submission and sync opt-ins')]
    cases += [(['--stdio','--driver-shared-resources'], 'Shared resources require explicit allocation opt-in')]
    cases += [(['--stdio','--driver-contexts','--driver-queries','--driver-allocations','--driver-shared-resources'],
               'Shared resources require an owned QEMU runtime')]
    for options, message in cases:
        result = subprocess.run([str(args.bridge.resolve()), *options], capture_output=True, text=True,
                                timeout=5, creationflags=subprocess.CREATE_NO_WINDOW)
        if result.returncode != 1 or message not in result.stderr or result.stdout:
            raise RuntimeError(f'Invalid capacity reached startup: {options}: {result.returncode}')
    print(f'PASS: {len(cases)} native capacity/control rejection cases before runtime startup')


if __name__ == '__main__':
    main()
