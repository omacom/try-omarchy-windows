"""Select the existing 1,800-frame budget without changing GPU or input code."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE_GENERATOR='fafeb1f0324e21e2824b085d676c4ef48fe8f2c383d06b65b5b6fc2d4762252f'
FRAMES=1800


def generate(gpu,output):
    gpu,output=Path(gpu),Path(output)
    path=gpu/'active-frame-probe/generate.py'
    if hashlib.sha256(path.read_bytes()).hexdigest()!=BASE_GENERATOR:
        raise ValueError('Sustained workload requires the frozen active generator')
    spec=importlib.util.spec_from_file_location('sustained_base_generator',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    receipt=module.generate(gpu,output)
    # The same generated C already accepts 2..1800. The builder must compile
    # HYPRLAND_QUEUE_FRAMES=1800; the runtime auditor verifies the actual stream.
    receipt=dict(receipt,baselineGeneratorSha256=BASE_GENERATOR,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        frameCount=FRAMES,compiledFrameDefine='HYPRLAND_QUEUE_FRAMES=1800',
        previousFrameCount=300,renderingCodeChanged=False,inputImplementationChanged=False,
        physicalScanoutCadenceVerified=False,nearNativePerformanceVerified=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
