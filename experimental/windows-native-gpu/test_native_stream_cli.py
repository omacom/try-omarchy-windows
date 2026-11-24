#!/usr/bin/env python3
"""Reject native reference misuse before opening a GPU or window."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--probe',type=Path,required=True)
args=parser.parse_args()
probe=args.probe.resolve()
environment=os.environ.copy()
environment.pop('WDDM_BRIDGE_CAPTURE_DIRECTORY',None)
cases=[[],['--unknown'],['--frames','0'],['--frames','59'],['--frames','3601'],
       ['--frames','-1'],['--frames','60x'],['--frames','4294967296'],
       ['--frames','60','--unknown'],['--frames','60','--topmost','extra']]
for case in cases:
    result=subprocess.run([str(probe),*case],env=environment,capture_output=True,text=True,timeout=10)
    assert result.returncode==2 and 'nativeStreamStarted' not in result.stderr,(case,result)
for value in (None,'relative-path',str(probe)):
    env=environment.copy()
    if value is not None: env['WDDM_BRIDGE_CAPTURE_DIRECTORY']=value
    result=subprocess.run([str(probe),'--frames','60'],env=env,capture_output=True,text=True,timeout=10)
    assert result.returncode==1 and 'nativeStreamStarted' not in result.stderr,(value,result)
with tempfile.TemporaryDirectory(dir=probe.parent,prefix='stream-cli-') as temp:
    capture=Path(temp)/'native-backbuffer-1.rgba'
    capture.write_bytes(b'preserve existing capture')
    env=environment.copy();env['WDDM_BRIDGE_CAPTURE_DIRECTORY']=temp
    result=subprocess.run([str(probe),'--frames','60'],env=env,capture_output=True,text=True,timeout=10)
    assert result.returncode==1 and 'nativeStreamStarted' not in result.stderr,result
    assert capture.read_bytes()==b'preserve existing capture'
    assert not (Path(temp)/'native-backbuffer-2.rgba').exists()
print('PASS: 14 native stream CLI rejection cases, no GPU/window startup or capture overwrite')
