"""Derive only the compositor diagnostic carrier; keep GPU/input fixtures frozen."""
from pathlib import Path
import hashlib
import json

BASE='f2cfd5a41a7771719d9978170834892734f266d9f544689193bdad5112d2a5f6'


def generate(gpu,output):
    gpu,output=Path(gpu),Path(output)
    raw=(gpu/'normal_root_graphics_service.c').read_bytes()
    if hashlib.sha256(raw).hexdigest()!=BASE:raise ValueError('Virtio trace requires the inspected supervisor')
    if output.exists():raise FileExistsError(output)
    text=raw.decode('utf-8').replace('\r\n','\n')
    start=text.index('    const int console=open("/dev/ttyS0"')
    end=text.index('    close(console);if(!granted) return -1;',start)+len('    close(console);if(!granted) return -1;')
    old=text[start:end]
    if 'device.st_rdev==makedev(4,64)' not in old:raise ValueError('Supervisor console grant changed')
    text=text[:start]+'''    if(trace_session_begin(uid,gid)) { perror("named graphics trace session");return -1; }'''+text[end:]
    before='''        "[Service]\\nNoNewPrivileges=yes\\nStandardOutput=tty\\nStandardError=tty\\n"
        "TTYPath=/dev/ttyS0\\nTTYReset=no\\nTTYVHangup=no\\n"'''
    after='''        "[Service]\\nNoNewPrivileges=yes\\nStandardOutput=file:/run/wddm-graphics-trace/session.fifo\\nStandardError=inherit\\n"'''
    if text.count(before)!=1:raise ValueError('Supervisor stdout selection changed')
    text=text.replace(before,after,1)
    before='static int uwsm_prepare(unsigned uid,unsigned gid) {'
    if text.count(before)!=1:raise ValueError('Supervisor preparation boundary changed')
    text=text.replace(before,'#include "trace_session.h"\n\n'+before,1)
    before='    printf("NORMAL_ROOT_GRAPHICS_EXIT result=%d normalOmarchySession=false\\n",result);'
    if text.count(before)!=1:raise ValueError('Supervisor final receipt boundary changed')
    text=text.replace(before,'    if(trace_session_end()) result=125;\n'+before,1)
    generated={'normal_root_graphics_service.c':text.encode('utf-8'),
               'trace_port.h':(Path(__file__).parent/'trace_port.h').read_bytes(),
               'trace_session.h':(Path(__file__).parent/'trace_session.h').read_bytes()}
    output.mkdir(parents=True)
    for name,data in generated.items():(output/name).write_bytes(data)
    receipt=dict(schema=1,baselineSupervisorSha256=BASE,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        generatedSources={name:hashlib.sha256(data).hexdigest() for name,data in generated.items()},
        carrier='virtio-serial',port=2,portName='org.7wdev.graphics.trace',wholeGraphicsSession=True,
        maximumTraceBytes=64*1024*1024,orderedFifo=True,
        liveHostCapture=True,inputImplementationChanged=False,gpuProtocolChanged=False,
        performanceGainEstablished=False,nearNativePerformanceVerified=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
