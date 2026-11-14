"""Add one bounded D3D12 diagnostic before the frozen graphical frame stream."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE_GENERATOR='93e7638d9972be4f31808941351415dad6759182c1f5f4686672263fccf0cf55'

def generate(gpu,output):
    gpu,output=Path(gpu),Path(output)
    path=gpu/'sustained-frame-probe/generate.py'
    if hashlib.sha256(path.read_bytes()).hexdigest()!=BASE_GENERATOR:
        raise ValueError('GPU pressure probe requires the frozen sustained generator')
    spec=importlib.util.spec_from_file_location('pressure_sustained_base',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    receipt=module.generate(gpu,output)
    original=dict(receipt['generatedSources'])
    path=output/'active_queue_probe.h';text=path.read_text(encoding='utf-8')
    marker='    void* fence=create(display,0x3144,NULL);'
    if text.count(marker)!=1:raise ValueError('GPU pressure insertion boundary changed')
    addition='''    if(expected==1) {
        static _Atomic int pressure_state;
        int waiting=0;
        if(!atomic_compare_exchange_strong(&pressure_state,&waiting,1)) goto failure;
        void* (*currentContext)(void)=(void* (*)(void))dlsym(RTLD_DEFAULT,"eglGetCurrentContext");
        void (*finish)(void)=(void (*)(void))proc("glFinish");
        if(!currentContext || !finish) goto failure;
        finish();
        void* pressure=dlopen("/opt/wddm-root/lib/d3d12-va-pressure-probe.so",RTLD_NOW|RTLD_LOCAL);
        if(!pressure) { fprintf(stderr,"HYPRLAND_VA_PRESSURE_LOAD_FAILURE %s\\n",dlerror());goto failure; }
        int (*probe)(void*,void*)=(int (*)(void*,void*))dlsym(pressure,"wddm_gpu_va_pressure_probe");
        if(!probe || probe(display,currentContext())) { dlclose(pressure);goto failure; }
        if(dlclose(pressure)) goto failure;
        atomic_store(&pressure_state,2);
        puts("HYPRLAND_VA_PRESSURE_RETIRED beforeFirstQueue=true nativeObjectsReleased=true frameLoopUnchanged=true");
    }
'''
    path.write_text(text.replace(marker,addition+marker,1),encoding='utf-8',newline='\n')
    generated={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in original}
    if [name for name in original if original[name]!=generated[name]]!=['active_queue_probe.h']:
        raise ValueError('Pressure probe changed the rendering/input workload')
    receipt=dict(receipt,baselineGeneratedSources=original,sustainedBaselineGeneratorSha256=BASE_GENERATOR,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),generatedSources=generated,
        preStreamD3D12Pressure=True,renderingCodeChanged=False,inputImplementationChanged=False,
        pressureSources={name:hashlib.sha256((gpu/name).read_bytes()).hexdigest() for name in
            ('d3d12_va_pressure_probe.cpp','gpu_pressure_pattern.h','build_gpu_va_pressure_probe.sh')})
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
