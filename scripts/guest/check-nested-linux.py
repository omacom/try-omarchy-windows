#!/usr/bin/env python3
"""Boot a diskless Linux acceptance fixture through nested KVM, never TCG.

Run as the ordinary Omarchy user. Supply QEMU, kernel, initramfs and firmware
paths. The initramfs must print the documented READY/EXIT markers from PID 1.
No disks or network devices are attached. Each boot is bounded to 60 seconds.
"""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import time


def boot(args, mode):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        address = reservation.getsockname()
    command = [str(args.qemu.resolve()), '-L', str(args.firmware.resolve()),
               '-machine', 'q35,accel=kvm', '-cpu', 'host', '-smp', '1', '-m', '256M',
               '-nodefaults', '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
               '-no-reboot', '-S', '-kernel', str(args.kernel.resolve()),
               '-initrd', str(args.initramfs.resolve()),
               '-append', f'console=ttyS0 rdinit=/init panic=1 tryomarchy.test={mode}',
               '-qmp', f'tcp:{address[0]}:{address[1]},server=on,wait=off']
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                connection = socket.create_connection(address, timeout=2)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Nested QEMU did not open QMP')
                time.sleep(.05)
        with connection:
            connection.settimeout(10)
            with connection.makefile('rb') as stream:
                if 'QMP' not in json.loads(stream.readline()):
                    raise RuntimeError('Missing QMP greeting')

                def qmp(name):
                    connection.sendall(json.dumps({'execute': name}).encode() + b'\n')
                    while True:
                        reply = json.loads(stream.readline())
                        if 'event' in reply:
                            continue
                        if 'error' in reply:
                            raise RuntimeError(reply['error'])
                        return reply['return']

                qmp('qmp_capabilities')
                kvm = qmp('query-kvm')
                if kvm != {'enabled': True, 'present': True}:
                    raise RuntimeError(f'KVM is not enabled: {kvm}')
                qmp('cont')
        output = process.communicate(timeout=60)[0].decode(errors='replace')
        ready = next((s for s in output.splitlines() if s.startswith('TRY_OMARCHY_NESTED_LINUX_READY ')), '')
        passed = process.returncode == 0 and bool(ready) and f'TRY_OMARCHY_NESTED_LINUX_EXIT {mode}' in output
        result = {'mode': mode, 'kvm': kvm, 'exitCode': process.returncode,
                  'ready': ready, 'passed': passed}
        if not passed:
            result['logTail'] = output[-8192:]
        return result
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate(timeout=5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('qemu', 'kernel', 'initramfs', 'firmware'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    results = [boot(args, mode) for mode in ('poweroff', 'reboot')]
    print(json.dumps(results, indent=2))
    return 0 if all(r['passed'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
