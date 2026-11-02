"""Check bounded relay realization/registration without opening a GPU window."""
import argparse
from pathlib import Path
import subprocess


def verify(qemu, firmware):
    def run(arguments):
        return subprocess.run([str(qemu.resolve()), *arguments], capture_output=True,
                              text=True, timeout=15)
    help_result = run(['-device', 'wddm-sdl-relay,help'])
    assert help_result.returncode == 0, help_result.stderr
    help_text = help_result.stdout + help_result.stderr
    for name in ('adapter-luid', 'audit-directory', 'source-format', 'session', 'generation', 'width', 'height', 'boot-display'):
        assert name in help_text, (name, help_text)
    for extra in (
        'session=x,adapter-luid=0011223344556677',
        'session=00112233445566778899aabbccddeeff,adapter-luid=x',
        'session=00112233445566778899aabbccddeeff,adapter-luid=0011223344556677,width=3841',
        'session=00112233445566778899aabbccddeeff,adapter-luid=0011223344556677,height=2161',
        'session=00112233445566778899aabbccddeeff,adapter-luid=0011223344556677,generation=0',
        'session=00112233445566778899aabbccddeeff,adapter-luid=0011223344556677,source-format=unknown',
    ):
        result = run(['-L', str(firmware.resolve()), '-machine', 'q35,accel=tcg', '-m', '16M',
                      '-display', 'none', '-nodefaults', '-vga', 'none', '-S',
                      '-device', 'wddm-sdl-relay,' + extra])
        assert result.returncode != 0 and 'SDL GPU relay needs' in result.stderr, result.stderr
    print('PASS: relay registration and invalid session/LUID/extent/generation rejected before GPU startup')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    args = parser.parse_args()
    verify(args.qemu, args.firmware)
