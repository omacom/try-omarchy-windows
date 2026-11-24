"""Require compiled user NAT and an actual virtio NIC, with no guest or Internet traffic."""
import argparse
import json
from pathlib import Path
import subprocess
from test_section import Qmp


def verify(qemu, firmware):
    result=subprocess.run([str(qemu.resolve()),'-netdev','help'],capture_output=True,text=True,timeout=15,
                          creationflags=subprocess.CREATE_NO_WINDOW)
    backends=(result.stdout+'\n'+result.stderr).splitlines()
    if result.returncode or 'user' not in [line.strip() for line in backends]:
        raise RuntimeError('The QEMU candidate lacks the user networking backend')
    command=[str(qemu.resolve()),'-L',str(firmware.resolve()),'-machine','q35,accel=tcg','-m','64M',
             '-display','none','-nodefaults','-monitor','none','-serial','none','-S','-qmp','stdio',
             '-netdev','user,id=browser-net,restrict=on',
             '-device','virtio-net-pci,id=browser-nic,netdev=browser-net,romfile=']
    monitor=Qmp(command)
    try:
        devices=monitor.call('query-pci')
        def find(value):
            if isinstance(value,dict):
                return value.get('qdev_id')=='browser-nic' or any(find(item) for item in value.values())
            return isinstance(value,list) and any(find(item) for item in value)
        if not find(devices):raise RuntimeError('The user-network virtio NIC was not realized')
        monitor.call('quit');monitor.process.wait(timeout=10)
        if monitor.process.returncode:raise RuntimeError('QEMU network capability check did not exit cleanly')
    finally:
        monitor.close()
    return dict(userBackendCompiled=True,virtioNicRealized=True,restrictedProbe=True,
                hostForwards=[],internetConnectivityVerified=False,browserRenderingVerified=False)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu',type=Path,required=True)
    parser.add_argument('--firmware',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args();proof=verify(args.qemu,args.firmware)
    with args.report.open('x',encoding='utf-8') as out:json.dump(proof,out,indent=2);out.write('\n')
    print('PASS: user networking compiled and virtio NIC realized; browsing remains unverified')
