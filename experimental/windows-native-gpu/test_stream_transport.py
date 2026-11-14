"""Stream negotiation corruption tests using an owned fake transport; no GPU."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

WORKER = r'''#!/usr/bin/env python3
import os, struct, sys
mode = os.environ['STREAM_FAULT']
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
        caps = 103 | (1<<30) | (1<<29) | (1<<25) | (1<<31)
        if 'missing-' in mode: caps &= ~(1 << int(mode.rsplit('-',1)[1]))
        send(struct.pack('<IIiI4I',op,0,0,0,1,caps,4318,11352))
    elif op in (0x2001,0x2004):
        send(struct.pack('<IIiIiIQ',op,1 if op == 0x2001 else 2,0,0,0,0,0))
    elif op in (0x2003,0x2005): send(struct.pack('<IIiIiIQ',op,handle,0,0,0,0,0))
    elif op == 0x205c:
        assert not handle and size == 16; begins += 1
        values = [1,64,16,16,1024,256,50000,0]
        if 'field-' in mode: values[int(mode.rsplit('-',1)[1])] += 1
        response = struct.pack('<IIiI8I',op+1 if mode.endswith('opcode') else op,
            99 if mode.endswith('handle') else 0,-95 if mode.endswith('status') else 0,
            1 if mode.endswith('padding') else 0,*values)
        if mode.endswith('short'): response = response[:-1]
        if mode.endswith('long'): response += b'x'
        send(response)
    else: raise AssertionError(op)
assert begins == (0 if mode.endswith(('unowned','disabled')) or 'missing-' in mode else 1), begins
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--shim',type=Path,required=True); p.add_argument('--probe',type=Path,required=True)
    p.add_argument('--disabled-profile',action='store_true'); a = p.parse_args()
    modes = ['disabled'] if a.disabled_profile else ['normal','unowned','opcode','handle','status','padding','short','long'] + [f'field-{i}' for i in range(8)] + [f'missing-{i}' for i in (25,29,30,31)]
    with tempfile.TemporaryDirectory(prefix='stream-transport-') as temporary:
        worker = Path(temporary)/'worker.py'; worker.write_text(WORKER); worker.chmod(0o700)
        for case in modes:
            mode = 'stream-profile-'+case
            env = {k:v for k,v in os.environ.items() if not k.startswith('WDDM_BRIDGE_') and k != 'LD_PRELOAD'}
            env.update(LD_PRELOAD=str(a.shim.resolve()),WDDM_BRIDGE_WINDOWS_WORKER=str(worker),STREAM_FAULT=mode)
            result = subprocess.run([str(a.probe.resolve()),mode],env=env,capture_output=True,text=True,timeout=20)
            if result.returncode or 'workerReaped=true exit=0' not in result.stderr:
                raise RuntimeError(f'{mode}: {result.returncode}: {result.stderr}')
    print(f'PASS: {len(modes)} explicit stream transport cases')


if __name__ == '__main__': main()
