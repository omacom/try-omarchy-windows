import argparse
from pathlib import Path
import subprocess

parser=argparse.ArgumentParser();parser.add_argument('--bridge',required=True,type=Path);args=parser.parse_args()
message='Virtio trace requires the explicit high-resolution owned SDL coordinator and read-only root'
cases=[([],message),(['--stdio'],message),(['--driver-coordinator'],message),
       (['--driver-coordinator','--driver-sdl-relay'],message),
       (['--driver-sdl-relay'],'SDL graphics requires the owned coordinator')]
for prefix,expected in cases:
    result=subprocess.run([str(args.bridge.resolve()),*prefix,'--driver-virtio-trace'],capture_output=True,text=True,timeout=5,
                          creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode!=1 or result.stdout or expected not in result.stderr:raise RuntimeError('Graphics trace guard reached startup: '+repr(prefix))
print('PASS: 5 isolated virtio graphics trace guards before runtime startup')
