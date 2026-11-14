"""Frame diagnostic receipts without changing the frozen renderer/input fixture."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE='20f66a957729ce113b90e9d159fb135827699117f6a01cf3d6c077f560b2c9d1'


def generate(gpu,output):
    gpu,output=Path(gpu),Path(output)
    baseline=gpu/'multiapp-frame-probe/generate.py'
    if hashlib.sha256(baseline.read_bytes()).hexdigest()!=BASE:
        raise ValueError('Framing requires the frozen CPU384 generator')
    spec=importlib.util.spec_from_file_location('framed_multiapp_base',baseline)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    receipt=module.generate(gpu,output);original=dict(receipt['generatedSources'])
    path=output/'active_queue_probe.h';text=path.read_text(encoding='utf-8')
    changes=[
        ('#include <time.h>','#include <time.h>\n#include "guest_trace_line.h"'),
        ('    printf("HYPRLAND_HOST_QUEUE_RESULT','    if(guest_trace_line("HYPRLAND_HOST_QUEUE_RESULT'),
        ('destroyed?"true":"false",result?"false":"true");','destroyed?"true":"false",result?"false":"true")) goto failure;'),
        ('    if(flags&2u) printf("HYPRLAND_HOST_QUEUE_END_ACK frame=%llu outputDrained=true\\n",(unsigned long long)frame);',
         '    if((flags&2u) && guest_trace_line("HYPRLAND_HOST_QUEUE_END_ACK frame=%llu outputDrained=true\\n",(unsigned long long)frame)) goto failure;'),
        ('    printf("HYPRLAND_ACTIVE_QUEUE_TIMING','    if(guest_trace_line("HYPRLAND_ACTIVE_QUEUE_TIMING'),
        ('        retries+1,(unsigned long long)active_finished);','        retries+1,(unsigned long long)active_finished)) goto failure;'),
        ('    puts("HYPRLAND_HOST_QUEUE_FAILURE displayVerified=false");','    (void)guest_trace_line("HYPRLAND_HOST_QUEUE_FAILURE displayVerified=false\\n");')]
    for old,new in changes:
        if text.count(old)!=1:raise ValueError('Queue receipt framing boundary changed: '+old)
        text=text.replace(old,new,1)
    path.write_text(text,encoding='utf-8',newline='\n')
    (output/'guest_trace_line.h').write_bytes((gpu/'guest_trace_line.h').read_bytes())
    generated={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in (*original,'guest_trace_line.h')}
    if [n for n in original if original[n]!=generated[n]]!=['active_queue_probe.h']:
        raise ValueError('Receipt framing changed rendering, workload or input')
    receipt=dict(receipt,framingBaselineGeneratedSources=original,framingBaselineGeneratorSha256=BASE,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),generatedSources=generated,
        receiptFramingChanged=True,renderingCodeChanged=False,inputImplementationChanged=False,
        gpuProtocolChanged=False,workloadChanged=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt
