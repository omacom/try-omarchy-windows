"""OpenGL lab negotiation corruption tests; owned fake transport, no GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

WORKER = r'''#!/usr/bin/env python3
import os, struct, sys
mode = os.environ['OPENGL_FAULT']
extended = os.environ.get('OPENGL_EXTENDED_PROFILE') == '1'
compositor = os.environ.get('OPENGL_COMPOSITOR_PROFILE') == '1'
render = os.environ.get('OPENGL_COMPOSITOR_RENDER_PROFILE') == '1'
desktop = os.environ.get('OPENGL_DESKTOP_PROFILE') == '1'
desktop4k = os.environ.get('OPENGL_DESKTOP_4K_PROFILE') == '1'
begins = 0
def send(data):
    sys.stdout.buffer.write(struct.pack('<I',len(data)) + data); sys.stdout.buffer.flush()
while True:
    length = sys.stdin.buffer.read(4)
    if not length: break
    size, = struct.unpack('<I',length); packet = sys.stdin.buffer.read(size)
    assert len(packet) == size
    op,handle,status,padding = struct.unpack_from('<IIiI',packet)
    assert not status and not padding
    if op == 0x2000:
        caps = 103 | (1<<25) | (1<<14) | (1<<17) | (1<<31)
        if desktop4k: caps |= 1<<23
        if 'missing-' in mode: caps &= ~(1 << int(mode.rsplit('-',1)[1]))
        send(struct.pack('<IIiI4I',op,0,0,0,1,caps,4318,11352))
    elif op == 0x2001: send(struct.pack('<IIiIiIQ',op,1,0,0,0,0,0))
    elif op == 0x205e:
        assert not handle and size == (20 if extended or compositor or render or desktop or desktop4k else 16); begins += 1
        if extended or compositor or render or desktop or desktop4k: assert struct.unpack_from('<I', packet, 16) == (6 if desktop4k else 5 if desktop else 4 if render else 3 if compositor else 2,)
        values = [6,0,0,64,0,4,0,1024] if desktop4k else [5,0,0,64,0,4,0,1024] if desktop else [4,2,256,32,0,4,50000,768] if render else [3,2,256,16,0,4,50000,512] if compositor else [2 if extended else 1,2,64,16,0,4,10000,384 if extended else 0]
        if 'field-' in mode: values[int(mode.rsplit('-',1)[1])] += 1
        response = struct.pack('<IIiI8I',op+1 if mode.endswith('opcode') else op,
            99 if mode.endswith('handle') else 0,1 if mode.endswith('status') else 0,
            1 if mode.endswith('padding') else 0,*values)
        if mode.endswith('unsupported'): response = struct.pack('<IIiI',op,0,-95,0)
        if mode.endswith('short'): response = response[:-1]
        if mode.endswith('long'): response += b'x'
        send(response)
    else: raise AssertionError(op)
assert begins == (0 if mode.endswith(('late','disabled')) or 'missing-' in mode else 1), begins
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shim', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--disabled-profile', action='store_true')
    profiles = parser.add_mutually_exclusive_group()
    profiles.add_argument('--extended-profile', action='store_true')
    profiles.add_argument('--compositor-profile', action='store_true')
    profiles.add_argument('--compositor-render-profile', action='store_true')
    profiles.add_argument('--desktop-profile', action='store_true')
    profiles.add_argument('--desktop-4k-profile', action='store_true')
    args = parser.parse_args()
    modes = ['disabled'] if args.disabled_profile else ['normal', 'late', 'opcode', 'handle', 'status', 'padding',
            'unsupported', 'short', 'long'] + [f'field-{i}' for i in range(8)] + [f'missing-{i}' for i in ((25,14,17,31,23) if args.desktop_4k_profile else (25,14,17,31))]
    with tempfile.TemporaryDirectory(prefix='opengl-transport-') as temporary:
        worker = Path(temporary)/'worker.py'; worker.write_text(WORKER); worker.chmod(0o700)
        for case in modes:
            mode = 'opengl-profile-'+case
            env = {k:v for k,v in os.environ.items() if not k.startswith('WDDM_BRIDGE_') and k != 'LD_PRELOAD'}
            env.update(LD_PRELOAD=str(args.shim.resolve()), WDDM_BRIDGE_WINDOWS_WORKER=str(worker), OPENGL_FAULT=mode)
            if args.extended_profile: env['OPENGL_EXTENDED_PROFILE'] = '1'
            if args.compositor_profile: env['OPENGL_COMPOSITOR_PROFILE'] = '1'
            if args.compositor_render_profile: env['OPENGL_COMPOSITOR_RENDER_PROFILE'] = '1'
            if args.desktop_profile: env['OPENGL_DESKTOP_PROFILE'] = '1'
            if args.desktop_4k_profile: env['OPENGL_DESKTOP_4K_PROFILE'] = '1'
            result = subprocess.run([str(args.probe.resolve()),mode],env=env,capture_output=True,text=True,timeout=20)
            if result.returncode or 'workerReaped=true exit=0' not in result.stderr or 'realDxgForwarding=true' in result.stderr:
                raise RuntimeError(f'{mode}: {result.returncode}: {result.stderr}')
    print(f'PASS: {len(modes)} explicit OpenGL transport cases')


if __name__ == '__main__':
    main()
