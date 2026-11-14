"""Failure injection is closed unless the explicit owned SDL fixture is selected."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge',required=True,type=Path)
    args=parser.parse_args()
    required='SDL failure tests require the explicit high-resolution owned SDL coordinator and read-only root'
    cases=[]
    for flag in ('--driver-sdl-failure-test','--driver-sdl-retirement-failure-test'):
        for prefix in ([],['--stdio'],['--driver-coordinator'],['--driver-coordinator','--driver-sdl-relay']):
            cases.append((prefix+[flag],required))
        cases.append((['--driver-sdl-relay',flag],
                      'SDL graphics requires the owned coordinator'))
    for options,message in cases:
        result=subprocess.run([str(args.bridge.resolve()),*options],capture_output=True,text=True,
                              timeout=5,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        if result.returncode!=1 or message not in result.stderr or result.stdout:
            raise RuntimeError(f'SDL failure control reached startup: {options}: {result.returncode}: {result.stderr}')
    print(f'PASS: {len(cases)} SDL failure probe guards before runtime startup')


if __name__=='__main__':main()
