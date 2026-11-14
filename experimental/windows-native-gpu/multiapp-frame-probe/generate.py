"""Select the explicit profile14 entry point in the frozen two-window fixture."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE_GENERATOR = 'c35afb26d6df84defd79651d0458ca98381a904a0f553d1b774cd91312da09da'
PROFILE_BEFORE = 'const unsigned profile=getenv("WDDM_BRIDGE_ACTOR_PROFILE")'
PROFILE_AFTER = ('const unsigned profile=getenv("WDDM_BRIDGE_ACTOR_PROFILE") && '
                 '!strcmp(getenv("WDDM_BRIDGE_ACTOR_PROFILE"),"14") ? 14 : getenv("WDDM_BRIDGE_ACTOR_PROFILE")')
ENTRY_BEFORE = 'begin_name=profile==13 ?'
ENTRY_AFTER = 'begin_name=profile==14 ? "wddm_bridge_begin_opengl_multiapp_render" : profile==13 ?'


def generate(gpu, output):
    gpu, output = Path(gpu), Path(output)
    baseline = gpu/'multiwindow-frame-probe/generate.py'
    if hashlib.sha256(baseline.read_bytes()).hexdigest() != BASE_GENERATOR:
        raise ValueError('Multiapp profile requires the frozen multiwindow generator')
    spec = importlib.util.spec_from_file_location('multiapp_window_base', baseline)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    receipt = module.generate(gpu, output); original = dict(receipt['generatedSources'])
    path = output/'active_probe.c'; text = path.read_text(encoding='utf-8')
    for old, new in ((PROFILE_BEFORE, PROFILE_AFTER), (ENTRY_BEFORE, ENTRY_AFTER)):
        if text.count(old) != 1: raise ValueError('Multiapp startup profile boundary changed')
        text = text.replace(old, new, 1)
    path.write_text(text, encoding='utf-8', newline='\n')
    generated = {name: hashlib.sha256((output/name).read_bytes()).hexdigest() for name in original}
    if [n for n in original if original[n] != generated[n]] != ['active_probe.c']:
        raise ValueError('Multiapp profile changed rendering, window actions or input sources')
    receipt = dict(receipt, multiappBaselineGeneratedSources=original,
        multiappBaselineGeneratorSha256=BASE_GENERATOR,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), generatedSources=generated,
        negotiatedCpuProfile=14, coordinatedObjectsVersion=9, cpuMappingBytes=402653184,
        cpuMappingGuardRaised=True, gpuUsageGuardRaised=False, gpuProtocolChanged=True,
        renderingCodeChanged=False, inputImplementationChanged=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt


if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
