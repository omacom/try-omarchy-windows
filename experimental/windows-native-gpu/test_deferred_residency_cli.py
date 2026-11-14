"""Reject incomplete residency opt-ins before starting a VM or using a GPU."""
import argparse
from pathlib import Path
import subprocess
parser=argparse.ArgumentParser()
parser.add_argument('--bridge',required=True,type=Path)
args=parser.parse_args()
message='Deferred residency requires the explicit high-resolution owned SDL coordinator, residency, retirement and read-only root'
cases=[([],message),(['--stdio'],message),(['--driver-residency'],message),
       (['--driver-coordinator'],message),
       (['--driver-coordinator','--driver-sdl-relay'],message),
       (['--driver-coordinator','--driver-sdl-relay','--driver-residency'],message)]
for prefix,expected in cases:
    result=subprocess.run([str(args.bridge.resolve()),*prefix,'--driver-deferred-residency'],capture_output=True,text=True,timeout=5,
                          creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode!=1 or result.stdout or expected not in result.stderr:
        raise RuntimeError('Deferred residency guard reached startup: '+repr(prefix))
print('PASS: 6 isolated deferred residency guards before runtime startup')
