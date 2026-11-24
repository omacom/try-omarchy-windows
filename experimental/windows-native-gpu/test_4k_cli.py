"""The separately compiled 4K host must reject ungated modes before GPU work."""
import argparse
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bridge',required=True)
    a=p.parse_args()
    cases=[['--stdio'],['--listen','0'],
           ['--stdio','--driver-native-metadata','--driver-consume-shared'],
           ['--run-qemu','missing-qemu','missing-firmware','missing-kernel','missing-image','missing-log',
            '--driver-native-metadata','--driver-consume-shared','--driver-cpu','--driver-cpu-slots','64']]
    for args in cases:
        r=subprocess.run([a.bridge,*args],input=b'',capture_output=True,timeout=10)
        if (r.returncode==0 or r.stdout or b'4K lab build requires' not in r.stderr or
                b'nativeTextureMetadata' in r.stderr or b'ownedQemuPid' in r.stderr):
            raise SystemExit(f'FAIL: 4K startup gate {args}: {r.returncode}, {r.stderr!r}')
    sparse=[['--stdio','--driver-cpu-slots','512'],
            ['--run-qemu','qemu','firmware','kernel','image','log','--driver-cpu','--driver-cpu-slots','512'],
            ['--run-qemu','qemu','firmware','kernel','image','log','--driver-cpu','--driver-cpu-slots','512','--driver-fence-slots','128']]
    for args in sparse:
        r=subprocess.run([a.bridge,*args],input=b'',capture_output=True,timeout=10)
        if r.returncode==0 or r.stdout or b'Sparse CPU aperture requires' not in r.stderr or b'ownedQemuPid' in r.stderr:
            raise SystemExit(f'FAIL: sparse startup gate {args}: {r.returncode}, {r.stderr!r}')
    desktop=[['--stdio','--driver-cpu-slots','1024'],
             ['--run-qemu','qemu','firmware','kernel','image','log','--driver-cpu','--driver-cpu-slots','1024'],
             ['--run-qemu','qemu','firmware','kernel','image','log','--driver-cpu','--driver-cpu-slots','1024','--driver-coordinator','--driver-fence-slots','256']]
    for args in desktop:
        r=subprocess.run([a.bridge,*args],input=b'',capture_output=True,timeout=10)
        if r.returncode==0 or r.stdout or b'1024 CPU slots require' not in r.stderr or b'ownedQemuPid' in r.stderr:
            raise SystemExit(f'FAIL: desktop slot startup gate {args}: {r.returncode}, {r.stderr!r}')
    print('PASS: 4 isolated 4K startup gates, 3 sparse-capacity gates and 3 desktop-slot gates reject before GPU/VM work')


if __name__=='__main__':
    main()
