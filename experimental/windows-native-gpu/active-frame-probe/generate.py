"""Derive an explicit active-workload probe without altering frozen fixtures.

The ordinary desktop fixture intentionally drives notifications every 50 ms.
This probe uses an animated real Alacritty application instead. It changes no
input implementation, renderer, GPU ABI, memory guard or stream finalization.
"""
from pathlib import Path
import hashlib
import json

BASE = {
    'hyprland_startup_probe.c': 'd1d2de1cbbef7d9c29cf65bcce4cc194199d1d71f13c30bec20ca2d4d16bd6bf',
    'hyprland_queue_probe.h': '5e3a93881b4e06cd51e7d1d17694369f0106167ae0b91d72eb9a904c22da6f25',
    'gpu-terminal-scene.sh': 'ee614b91eb045c7e2dc34457118ccbbe8d90898285ff69e2818753423897a9ff',
}


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Active probe source boundary changed')
    return text.replace(old, new, 1)


def generate(gpu, output):
    gpu, output = Path(gpu), Path(output)
    originals = {name: (gpu / name).read_bytes() for name in BASE}
    if {name: hashlib.sha256(raw).hexdigest() for name, raw in originals.items()} != BASE:
        raise ValueError('Active probe requires the exact inspected baseline sources')
    if output.exists():
        raise FileExistsError(output)
    texts = {name: raw.decode('utf-8').replace('\r\n', '\n') for name, raw in originals.items()}
    header = texts['hyprland_queue_probe.h']
    header = replace_once(header, 'static _Atomic unsigned queued_frames;', '''static uint64_t active_monotonic_ns(void) {
    struct timespec value;
    if(clock_gettime(CLOCK_MONOTONIC,&value)) return 0;
    return (uint64_t)value.tv_sec*1000000000ull+(uint64_t)value.tv_nsec;
}
static _Atomic unsigned queued_frames;''')
    header = replace_once(header, '    void* (*currentDisplay)',
        '    const uint64_t active_entered=active_monotonic_ns();\n    if(!active_entered) goto failure;\n    void* (*currentDisplay)')
    header = replace_once(header, '    do {\n        result=queue',
        '    const uint64_t active_queued=active_monotonic_ns();\n    do {\n        result=queue')
    header = replace_once(header, '    const int error=errno;',
        '    const uint64_t active_returned=active_monotonic_ns();\n    const int error=errno;')
    header = replace_once(header, '    atomic_store(&queued_frames,expected);', '''    const uint64_t active_finished=active_monotonic_ns();
    if(!active_queued || active_queued<active_entered || active_returned<active_queued || active_finished<active_returned) goto failure;
    printf("HYPRLAND_ACTIVE_QUEUE_TIMING frame=%llu audit=%u fenceSetupNs=%llu queueRoundTripNs=%llu totalNs=%llu queueAttempts=%u monotonicNs=%llu cpuWallClock=true gpuTimestamp=false\\n",
        (unsigned long long)frame,flags&1u,(unsigned long long)(active_queued-active_entered),
        (unsigned long long)(active_returned-active_queued),(unsigned long long)(active_finished-active_entered),
        retries+1,(unsigned long long)active_finished);
    atomic_store(&queued_frames,expected);''')
    hook = replace_once(texts['hyprland_startup_probe.c'],
        '#include "hyprland_queue_probe.h"', '#include "active_queue_probe.h"')
    hook = replace_once(hook,
        '        for(unsigned tick=0;tick<HYPRLAND_QUEUE_FRAMES*2+100 && atomic_load(&queued_frames)<HYPRLAND_QUEUE_FRAMES && !atomic_load(&queue_failed);++tick) {',
        '''        const uint64_t active_start=active_monotonic_ns();
        if(!active_start) atomic_store(&queue_failed,1);
        const uint64_t active_deadline=active_start+90000000000ull;
        printf("HYPRLAND_ACTIVE_WORKLOAD_BEGIN targetUpdateHz=60 notificationRepaints=false observerSleepUs=2000 deadlineSeconds=90 inputUnchanged=true gpuPerformanceAccepted=false\\n");
        for(unsigned tick=0;active_monotonic_ns()<active_deadline && atomic_load(&queued_frames)<HYPRLAND_QUEUE_FRAMES && !atomic_load(&queue_failed);++tick) {''')
    first = hook.index('            char* const legacy_repaint[]=')
    last = hook.index('            usleep(50000);', first) + len('            usleep(50000);')
    block = hook[first:last]
    if block.count('run_ctl(repaint)') != 1 or 'hyprctl","notify"' not in block:
        raise ValueError('Notification repaint boundary changed')
    hook = hook[:first] + '''            // Real application output drives damage. This controller only
            // observes stream/input/resize progress; it does not pace rendering.
            usleep(2000);''' + hook[last:]
    shell = texts['gpu-terminal-scene.sh']
    start = shell.index('while :; do\n')
    shell = shell[:start] + 'exec /opt/wddm-root/bin/active-terminal-pulse\n'
    generated = {'active_probe.c': hook.encode(), 'active_queue_probe.h': header.encode(),
                 'gpu-terminal-scene.sh': shell.encode(),
                 'terminal_pulse.c': (Path(__file__).parent / 'terminal_pulse.c').read_bytes()}
    output.mkdir(parents=True)
    for name, raw in generated.items():
        (output / name).write_bytes(raw)
    receipt = dict(schema=1,baselineSources=BASE,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        generatedSources={name:hashlib.sha256(raw).hexdigest() for name,raw in generated.items()},
        targetUpdateHz=60,notificationRepaints=False,controllerObserverSleepUs=2000,
        workloadDeadlineSeconds=90,frameCount=300,inputImplementationChanged=False,
        cpuWallClockTimings=True,gpuTimestamps=False,nearNativePerformanceVerified=False)
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    return receipt


if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    print(json.dumps(generate(Path(__file__).resolve().parent.parent,args.output),indent=2))
