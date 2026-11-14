"""Derive a separately identified real-PTY stimulus covering the 90-second probe.

The original 60 Hz fixture remains unchanged. This corrects producer lifetime,
not GPU performance. A blocked synchronous PTY write still needs outer cleanup.
"""
from pathlib import Path
import hashlib,json

PARENT_SHA256='bd6db5881a7b3f3dd23edcd219cd68c4d43885e8588c202ef9d62470386130ec'
UPDATE_CAP=7200
SCHEDULED_CLOCK_CAP_SECONDS=120


def derive(source):
    if hashlib.sha256(source).hexdigest()!=PARENT_SHA256:
        raise ValueError('Terminal producer source differs from the pinned original')
    text=source.decode('utf-8');newline='\r\n' if '\r\n' in text else '\n'
    changes=[('while(!stopped && updates<3600) {','while(!stopped && updates<7200) {'),
             ('        last=monotonic_ns();if(!last || last<next) return 3;',
              '        last=monotonic_ns();if(!last || last<next) return 3;'+newline+
              '        if(last-started>=120000000000ull) break;')]
    for old,new in changes:
        if text.count(old)!=1:raise ValueError('Terminal producer lifetime boundary changed')
        text=text.replace(old,new,1)
    restored=text
    for old,new in reversed(changes):
        if restored.count(new)!=1:raise ValueError('Terminal producer inverse is ambiguous')
        restored=restored.replace(new,old,1)
    if restored.encode()!=source:raise ValueError('Unexpected non-lifetime producer change')
    return text.encode()


def generate(output):
    output=Path(output);original=Path(__file__).with_name('terminal_pulse.c').read_bytes();derived=derive(original)
    output.mkdir(parents=True,exist_ok=False)
    (output/'terminal_pulse.c').write_bytes(derived)
    receipt=dict(schema=1,parentSourceSha256=PARENT_SHA256,generatedSourceSha256=hashlib.sha256(derived).hexdigest(),
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),exactInverseSourceTransform=True,
        originalUpdateCap=3600,updateCap=UPDATE_CAP,targetHz=60,scheduledClockCapSeconds=SCHEDULED_CLOCK_CAP_SECONDS,
        blockedIoHardDeadlineImplemented=False,producerFixtureLifetimeChanged=True,
        gpuRenderingChanged=False,inputImplementationChanged=False,absoluteScheduleAndLateSlotSkippingPreserved=True,
        frameTarget=1800,workloadDeadlineSeconds=90,gpuTimestamps=False,hardwareRunVerified=False,dailyUseAccepted=False)
    with (output/'receipt.json').open('x',encoding='utf-8',newline='\n') as f:json.dump(receipt,f,indent=2);f.write('\n')
    return receipt


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('output',type=Path)
    print(json.dumps(generate(parser.parse_args().output),indent=2))
