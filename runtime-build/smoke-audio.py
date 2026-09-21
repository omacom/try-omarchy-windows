#!/usr/bin/env python3
"""Exercise selected/unavailable SDL routes against a built Windows QEMU.

Creates an isolated, paused, diskless VM. No guest code runs and no samples
are generated. The microphone-off case must not attempt a recording route.
"""
import argparse
import os
from pathlib import Path
import runpy

parser = argparse.ArgumentParser()
parser.add_argument('qemu', type=Path)
parser.add_argument('--output', required=True, help='SDL playback device name')
parser.add_argument('--input', required=True, help='SDL recording device name')
args = parser.parse_args()
# Windows QEMU does not reliably consume redirected stdio commands. Reuse the
# bounded socket transport already exercised by memory/Unicode acceptance.
VM = runpy.run_path(str(Path(__file__).with_name('smoke-memory.py')))['VM']

for label, output, input_, muted, expected in [
    ('selected devices', args.output, args.input, False, []),
    ('missing playback', 'Try Omarchy nonexistent playback endpoint', args.input,
     False, ['playback']),
    ('missing recording', args.output, 'Try Omarchy nonexistent recording endpoint',
     False, ['recording']),
    ('microphone disabled', args.output, 'Try Omarchy nonexistent recording endpoint',
     True, []),
]:
    env = {k: v for k, v in os.environ.items() if k.upper() not in {
        'SDL_AUDIO_DEVICE_NAME', 'OMARCHY_SDL_OUTPUT_DEVICE_NAME',
        'OMARCHY_SDL_INPUT_DEVICE_NAME'}}
    env.update(OMARCHY_SDL_OUTPUT_DEVICE_NAME=output,
               OMARCHY_SDL_INPUT_DEVICE_NAME=input_)
    audio = 'sdl,id=snd' + (',in.voices=0' if muted else '')
    with VM(args.qemu.resolve(), ['-audiodev', audio,
            '-device', 'virtio-sound-pci,audiodev=snd'], env=env) as vm:
        if vm.call('query-status')['running']:
            raise SystemExit(f'{label}: VM unexpectedly running')
        vm.call('quit')
        if vm.process.wait(timeout=10) != 0:
            raise SystemExit(f'{label}: QEMU failed')
        vm.log.seek(0)
        detail = vm.log.read().decode('utf-8', errors='replace')
    for direction in ('playback', 'recording'):
        warning = f'Requested SDL {direction} device is unavailable'
        if (warning in detail) != (direction in expected):
            raise SystemExit(f'{label}: unexpected {direction} routing: {detail}')
    if 'SDL_OpenAudioDevice for' in detail:
        raise SystemExit(f'{label}: default endpoint failed: {detail}')
    print(f'PASS: {label}')
