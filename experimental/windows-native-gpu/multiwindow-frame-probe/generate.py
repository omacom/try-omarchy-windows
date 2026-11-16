"""Add a second real NVIDIA application to the frozen sustained desktop probe."""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE_GENERATOR='93e7638d9972be4f31808941351415dad6759182c1f5f4686672263fccf0cf55'


def replace_once(text,old,new):
    if text.count(old)!=1:raise ValueError('Multiwindow source boundary changed')
    return text.replace(old,new,1)


def generate(gpu,output):
    gpu,output=Path(gpu),Path(output)
    path=gpu/'sustained-frame-probe/generate.py'
    if hashlib.sha256(path.read_bytes()).hexdigest()!=BASE_GENERATOR:
        raise ValueError('Multiwindow probe requires the frozen sustained generator')
    spec=importlib.util.spec_from_file_location('multiwindow_sustained_base',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    receipt=module.generate(gpu,output);original=dict(receipt['generatedSources'])
    path=output/'active_probe.c';text=path.read_text(encoding='utf-8')
    text=replace_once(text,'static _Atomic unsigned compositor_commits;',
        'static _Atomic unsigned compositor_commits;\nstatic _Atomic int multiwindow_done;')
    boundary='    if(frame>=2) return queue_output(fd,width,height,format,stride);'
    text=replace_once(text,boundary,
        '    if(atomic_load(&queued_frames)>=HYPRLAND_QUEUE_FRAMES-1 && !atomic_load(&multiwindow_done)) return 0;\n'+boundary)
    text=replace_once(text,'static int prepare_scene(void) {',
        '#include "multiwindow_control.h"\nstatic int prepare_scene(void) {')
    boundary='        if(input_delivery_enabled() && input_delivery_snapshot("BEFORE")) return -1;'
    text=replace_once(text,boundary,'        if(multiwindow_start()) return -1;\n'+boundary)
    text=replace_once(text,'        return close_gpu_terminal(last_gpu_terminal_pid,1);',
        '        if(multiwindow_close()) return -1;\n        return close_gpu_terminal(last_gpu_terminal_pid,1);')
    boundary='        unsigned resize_phase=1;'
    text=replace_once(text,boundary,boundary+'\n        unsigned multiwindow_phase=0;')
    boundary='            // Real application output drives damage. This controller only'
    addition='''            const unsigned thresholds[4]={300,600,1200,1700};
            if(multiwindow_phase<4 && atomic_load(&queued_frames)>=thresholds[multiwindow_phase]) {
                if(multiwindow_checkpoint(multiwindow_phase+1)) { atomic_store(&queue_failed,1);break; }
                if(++multiwindow_phase==4) atomic_store(&multiwindow_done,1);
            }
'''
    text=replace_once(text,boundary,addition+boundary)
    path.write_text(text,encoding='utf-8',newline='\n')
    pulse=(output/'terminal_pulse.c').read_text(encoding='utf-8')
    pulse=replace_once(pulse,'/tmp/omarchy-active-terminal-%ld.tmp','/tmp/omarchy-companion-terminal-%ld.tmp')
    pulse=replace_once(pulse,'/tmp/omarchy-active-terminal-pulse.json','/tmp/omarchy-companion-terminal-pulse.json')
    pulse=replace_once(pulse,'Active application updates:','Companion active updates:')
    boundary='    unsigned updates=0;'
    header='\\033[2J\\033[H\\033[48;2;32;96;192m\\033[38;2;255;255;255mNVIDIA companion window\\033[0m\\n\\nIndependent Wayland application\\nReal Alacritty OpenGL rendering\\nConcurrent with the Omarchy terminal\\n\\n'
    addition='\n    const char banner[]="'+header+'";\n    if(write_all(STDOUT_FILENO,banner,sizeof banner-1)) return 5;'
    pulse=replace_once(pulse,boundary,boundary+addition)
    (output/'companion_pulse.c').write_text(pulse,encoding='utf-8',newline='\n')
    (output/'multiwindow_control.h').write_bytes((Path(__file__).parent/'multiwindow_control.h').read_bytes())
    names=(*original,'companion_pulse.c','multiwindow_control.h')
    generated={name:hashlib.sha256((output/name).read_bytes()).hexdigest() for name in names}
    if [name for name in original if original[name]!=generated[name]]!=['active_probe.c']:
        raise ValueError('Multiwindow probe changed the baseline queue, pulse, or input shell')
    receipt=dict(receipt,baselineGeneratedSources=original,sustainedBaselineGeneratorSha256=BASE_GENERATOR,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),generatedSources=generated,
        multiwindowProbe=dict(primaryClass='omarchy-gpu-terminal',companionClass='omarchy-gpu-companion',
            companionSize=[960,640],companionAt=[2800,200],checkpointThresholds=[300,600,1200,1700],
            simultaneousVisibleWindows=2,officialInputChanged=False,hostBackendChanged=False),
        renderingCodeChanged=False,inputImplementationChanged=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8',newline='\n')
    return receipt


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    args=parser.parse_args();print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
