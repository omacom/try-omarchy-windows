"""Reject invalid lifetime options before creating a VM or native GPU device."""
import argparse
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--bridge', type=Path, required=True)
args = p.parse_args()
if sys.platform != 'win32' or not args.bridge.is_file(): p.error('Requires a built Windows bridge')
flag = '--driver-launcher-cancel-handle'
cases = [(['--stdio', flag, value], 'Invalid launcher cancellation handle') for value in
    ('0', '-1', '+1', '0x10', '1x', '18446744073709551615', '18446744073709551614', '18446744073709551616')]
cases += [(['--stdio', flag, ''], 'Missing launcher cancellation handle'),
    (['--stdio', flag, '1'], 'Launcher lifetime requires the owned QEMU runtime'),
    (['--listen', '0', flag, '1'], 'Launcher lifetime requires the owned QEMU runtime'),
    (['--stdio', flag, '1', flag, '2'], 'Launcher cancellation handle must be specified once'),
    (['--run-qemu', 'missing-qemu', 'firmware', 'kernel', 'initrd', 'fresh-log', flag, '1'], 'requires explicit query/context opt-ins')]
cases += [(['--stdio','--driver-launcher-session'], 'Launcher session requires an original profile'),
    (['--listen','0','--driver-launcher-session'], 'Launcher session requires an original profile'),
    (['--run-qemu','missing-qemu','firmware','kernel','image','log','--driver-launcher-session'], 'Launcher session requires an original profile'),
    (['--stdio','--driver-launcher-session','--driver-launcher-session'], 'Launcher session requires an original profile')]
cases += [(command, 'Boot console requires the persistent original launcher session') for command in (
    ['--stdio','--driver-boot-console'],
    ['--listen','0','--driver-boot-console'],
    ['--run-qemu','missing-qemu','firmware','kernel','image','log','--driver-boot-console'],
    ['--stdio','--driver-launcher-session','--driver-boot-console'])]
cases += [(['--stdio','--driver-boot-console','--driver-boot-console'], 'Boot console must be specified once')]
for command, reason in cases:
    result = subprocess.run([str(args.bridge.resolve()), *command], capture_output=True, text=True, timeout=5,
        creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode == 0 or reason not in result.stderr or 'ownedQemuPid' in result.stdout:
        raise ValueError('Wrong lifetime rejection: ' + repr(command) + '\n' + result.stdout + result.stderr)
print(f'PASS {len(cases)} launcher lifetime CLI controls; no VM/GPU/window/input')
