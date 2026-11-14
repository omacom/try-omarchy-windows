"""Select profile14 only for a guest carrying the new private CPU marker."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE_GENERATOR = 'b29ef029652844e158e4b2d529352c36ee53fb91f605d65e8c4c6736b7e0f7b9'
BEFORE = 'setenv("WDDM_BRIDGE_ACTOR_PROFILE",!access("/opt/wddm-root/desktop-reference-probe",R_OK)'
AFTER = ('setenv("WDDM_BRIDGE_ACTOR_PROFILE",!access("/opt/wddm-root/multiapp-cpu-probe",R_OK) ? "14" : '
         '!access("/opt/wddm-root/desktop-reference-probe",R_OK)')


def generate(gpu, output):
    gpu, output = Path(gpu), Path(output)
    baseline = gpu/'multiwindow-frame-probe/generate_service.py'
    if hashlib.sha256(baseline.read_bytes()).hexdigest() != BASE_GENERATOR:
        raise ValueError('Multiapp service requires the frozen observation generator')
    spec = importlib.util.spec_from_file_location('multiapp_service_base',baseline)
    module = importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    receipt = module.generate(gpu,output); original = dict(receipt['generatedSources'])
    path = output/'normal_root_graphics_service.c';text=path.read_text(encoding='utf-8')
    if text.count(BEFORE) != 1: raise ValueError('Multiapp service profile boundary changed')
    path.write_text(text.replace(BEFORE,AFTER,1),encoding='utf-8',newline='\n')
    generated={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in original}
    if [n for n in original if original[n] != generated[n]] != ['normal_root_graphics_service.c']:
        raise ValueError('Multiapp service changed the trace transport')
    receipt=dict(receipt,multiappBaselineGeneratedSources=original,multiappBaselineGeneratorSha256=BASE_GENERATOR,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),generatedSources=generated,
        negotiatedCpuProfile=14,coordinatedObjectsVersion=9,cpuMappingGuardRaised=True,
        gpuUsageGuardRaised=False,gpuProtocolChanged=True,traceTransportChanged=False,inputImplementationChanged=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt


if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
