"""Capture the companion renderer and bounded atomic kernel admission diagnostics."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE_GENERATOR='bcb6948d4fbbf73f954b6b4c237ab828654f61e7da378feb385f3e2141361384'


def generate(gpu,output):
    gpu,output=Path(gpu),Path(output)
    source=gpu/'virtio-trace-probe/generate.py'
    if hashlib.sha256(source.read_bytes()).hexdigest()!=BASE_GENERATOR:
        raise ValueError('Multiwindow observations require the frozen virtio trace generator')
    spec=importlib.util.spec_from_file_location('multiwindow_trace_base',source)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    receipt=module.generate(gpu,output);original=dict(receipt['generatedSources'])
    path=output/'normal_root_graphics_service.c';text=path.read_text(encoding='utf-8')
    old='"-t","omarchy-shell","-t","omarchy-gpu-app",'
    if text.count(old)!=2:raise ValueError('Owned application journal boundary changed')
    text=text.replace(old,old+'"-t","omarchy-gpu-companion",')
    boundary='    if(native_render_loaded && native_render_stop()) result=125;'
    if text.count(boundary)!=1:raise ValueError('Guest teardown observation boundary changed')
    addition='''    // Read-only replay of the admission snapshot recorded under the kernel lock.
    const pid_t admission_journal=fork();
    if(!admission_journal) {
        alarm(5);
        execl("/usr/bin/journalctl","journalctl","-b","-k","--no-pager","-n","16",
            "-o","short-monotonic","--grep","WDDM_APERTURE_ADMISSION_REJECTED",(char*)NULL);
        _exit(127);
    }
    if(admission_journal>0) waitpid(admission_journal,NULL,0);
'''
    text=text.replace(boundary,addition+boundary,1)
    path.write_text(text,encoding='utf-8',newline='\n')
    generated={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in original}
    if [name for name in original if original[name]!=generated[name]]!=['normal_root_graphics_service.c']:
        raise ValueError('Observation generator changed the trace transport')
    receipt=dict(receipt,baselineGeneratedSources=original,baselineTraceGeneratorSha256=BASE_GENERATOR,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),generatedSources=generated,
        companionRendererCaptured=True,kernelAdmissionCapturedAfterActorsExit=True,
        traceTransportChanged=False,inputImplementationChanged=False,gpuProtocolChanged=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
