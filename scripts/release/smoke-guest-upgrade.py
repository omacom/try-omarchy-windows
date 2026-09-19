#!/usr/bin/env python3
"""Upgrade a disposable copy of an older image and verify it across reboots."""
import argparse
import json
import os
from pathlib import Path
import selectors
import shlex
import subprocess
import time


FIXTURES = Path(__file__).resolve().parent / 'guest-upgrade'


def boot(artifacts, disk, phase, log, environment, timeout):
    spec = json.loads((artifacts / 'build-spec.json').read_text())
    cmdline = spec['runtime']['kernelCommandLine'].replace('console=tty0 ', '').replace('console=hvc0', 'console=ttyS0')
    cmdline += ' tryomarchy.instant=1 systemd.unit=multi-user.target'
    command = [
        'qemu-system-x86_64', '-nodefaults', '-no-reboot', '-accel', 'kvm',
        '-machine', 'q35', '-cpu', 'host', '-smp', '4', '-m', '4096',
        '-display', 'none', '-monitor', 'none', '-serial', 'stdio',
        '-drive', f'file={disk},format=raw,if=virtio',
        '-kernel', str(artifacts / 'vmlinuz-linux'),
        '-initrd', str(artifacts / 'initramfs-linux.img'), '-append', cmdline,
        '-device', 'virtio-rng-pci', '-netdev', 'user,id=net0',
        '-device', 'virtio-net-pci,netdev=net0',
        '-fsdev', f'local,id=tests,path={FIXTURES},security_model=none,readonly=on',
        '-device', 'virtio-9p-pci,fsdev=tests,mount_tag=hostshare',
    ]
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    transcript = bytearray()
    login = password = -1
    sent = False
    password_at = None
    deadline = time.monotonic() + timeout
    try:
        with log.open('wb') as output:
            while time.monotonic() < deadline:
                for key, _ in selector.select(timeout=1):
                    data = os.read(key.fileobj.fileno(), 65536)
                    if not data:
                        continue
                    output.write(data)
                    output.flush()
                    transcript.extend(data)
                    pos = transcript.rfind(b'login:')
                    if not sent and pos > login:
                        process.stdin.write(b'omarchy\n')
                        process.stdin.flush()
                        login = pos
                    pos = transcript.rfind(b'Password:')
                    if not sent and pos > password:
                        process.stdin.write(b'omarchy\n')
                        process.stdin.flush()
                        password = pos
                        password_at = time.monotonic()
                if password_at is not None and not sent and time.monotonic() - password_at > 3:
                    # One short command avoids terminal input limits and sudo
                    # discarding queued lines. Test scripts mount read-only.
                    assignments = ' '.join(shlex.quote(f'{k}={v}') for k, v in environment.items())
                    script = f'env {assignments} bash /mnt/host/{phase}.sh; result=$?; '
                    script += f"printf 'UPGRADE_%s:%s:%s\\n' RESULT {phase} $result; sudo systemctl poweroff\n"
                    process.stdin.write(script.encode())
                    process.stdin.flush()
                    sent = True
                if process.poll() is not None:
                    break
            else:
                raise RuntimeError(f'guest test timed out; see {log}')
        if process.returncode != 0 or f'UPGRADE_RESULT:{phase}:0'.encode() not in transcript:
            raise RuntimeError(f'guest test failed; see {log}')
    finally:
        selector.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        process.stdin.close()
        process.stdout.close()
    print(f'PASS {log.stem}', flush=True)


def runtime(spec):
    upstream = spec['upstream']
    return f"{upstream['version']}-{upstream.get('packageRelease', 1)}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path, help='verified older release artifacts, including decompressed rootfs.ext4')
    parser.add_argument('candidate', type=Path, help='newly built candidate artifacts')
    parser.add_argument('work', type=Path, help='new directory for the disposable disk and logs; must not exist')
    parser.add_argument('--timeout', type=int, default=1800, help='seconds per boot')
    args = parser.parse_args()
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        parser.error('accessible /dev/kvm is required')
    baseline, candidate, work = (p.resolve() for p in (args.baseline, args.candidate, args.work))
    for directory in (baseline, candidate):
        # Only the baseline factory is copied. Candidate boots reuse that disk.
        required = ('vmlinuz-linux', 'initramfs-linux.img', 'build-spec.json')
        if directory == baseline:
            required += ('rootfs.ext4',)
        for filename in required:
            if not (directory / filename).is_file():
                parser.error(f'missing artifact: {directory / filename}')
        if ',' in str(directory):
            parser.error('QEMU paths must not contain commas')
    if ',' in str(work) or ',' in str(FIXTURES):
        parser.error('QEMU paths must not contain commas')
    specs = [json.loads((p / 'build-spec.json').read_text()) for p in (baseline, candidate)]
    environment = {'BASELINE_RUNTIME': runtime(specs[0]), 'CANDIDATE_RUNTIME': runtime(specs[1]),
                   'CANDIDATE_VERSION': specs[1]['upstream']['version']}
    work.mkdir(parents=True, exist_ok=False)
    disk = work / 'persistent.ext4'
    subprocess.run(['cp', '--reflink=auto', '--sparse=always', str(baseline / 'rootfs.ext4'), str(disk)], check=True)
    with disk.open('r+b') as stream:
        stream.truncate(max(disk.stat().st_size, 24 * 1024**3))
    for artifacts, phase, name in (
        (baseline, 'seed', '01-seed'), (candidate, 'upgrade', '02-upgrade'),
        (candidate, 'reboot', '03-reboot'), (baseline, 'reboot', '04-old-image'),
        (candidate, 'reboot', '05-return-to-candidate'),
    ):
        boot(artifacts, disk, phase, work / f'{name}.log', environment, args.timeout)
    print(f'Upgrade and preservation checks passed. Evidence: {work}')


if __name__ == '__main__':
    main()
