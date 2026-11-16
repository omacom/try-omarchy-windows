"""Route the existing guest probe session to journald without a trace device.

This changes logging only. The generated service still contains the existing
bounded probe/session setup and is not a normal-account launcher bootstrap.
"""
from pathlib import Path
import hashlib
import importlib.util
import json

BASE = 'f199b29ea9bffa3a9a09fb358b7cc827951714a81660fbbaedeb7163c630e77f'
BASE_GENERATOR = 'becf538123e37818ca705e5d843021a64291c860971947e92d657ddaa39e11f8'


def generate(gpu, output):
    gpu, output = Path(gpu), Path(output)
    baseline = gpu/'multiapp-frame-probe/generate_service.py'
    if hashlib.sha256(baseline.read_bytes()).hexdigest() != BASE_GENERATOR:
        raise ValueError('Launcher logging requires the inspected multiapp recipe')
    spec = importlib.util.spec_from_file_location('launcher_logging_multiapp', baseline)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    # Reproduce the actual composed supervisor, including profile14 and both
    # existing observers. The raw root-service source alone is an older profile.
    parent = module.generate(gpu, output)
    target = output/'normal_root_graphics_service.c'
    raw = target.read_bytes()
    if hashlib.sha256(raw).hexdigest() != BASE:
        raise ValueError('Launcher logging requires the inspected guest service')
    text = raw.decode('utf-8').replace('\r\n', '\n')
    start = text.index('    // The original direct fixture inherits this dedicated guest console.')
    call = '    if(trace_session_begin(uid,gid)) { perror("named graphics trace session");return -1; }'
    end = text.index(call, start) + len(call)
    console = text[start:end]
    if console.count(call) != 1:
        raise ValueError('Guest trace session initialization changed')
    journal = ('    // Keep user logs in the ordinary journal; no diagnostic trace\n'
               '    // device or user permission to the serial console is required.\n'
               '    (void)uid; (void)gid;')
    text = text[:start]+journal+text[end:]
    before = '        "[Service]\\nNoNewPrivileges=yes\\nStandardOutput=file:/run/wddm-graphics-trace/session.fifo\\nStandardError=inherit\\n"'
    after = '        "[Service]\\nNoNewPrivileges=yes\\nStandardOutput=journal\\nStandardError=inherit\\n"'
    if text.count(before) != 1:
        raise ValueError('Guest unit logging selection changed')
    text = text.replace(before, after, 1)
    include = '#include "trace_session.h"\n\n'
    teardown = '    if(trace_session_end()) result=125;\n'
    if text.count(include) != 1 or text.count(teardown) != 1:
        raise ValueError('Guest trace session boundaries changed')
    text = text.replace(include, '', 1).replace(teardown, '', 1)
    # An exact reverse transform proves the bounded setup, actor registration,
    # privilege drop, input and GPU teardown were not silently rewritten.
    restored = text.replace(after, before, 1).replace(journal, console, 1)
    restored = restored.replace('static int uwsm_prepare(', include+'static int uwsm_prepare(', 1)
    restored = restored.replace('    printf("NORMAL_ROOT_GRAPHICS_EXIT', teardown+'    printf("NORMAL_ROOT_GRAPHICS_EXIT', 1)
    if restored != raw.decode('utf-8').replace('\r\n', '\n'):
        raise ValueError('Unexpected guest service change')
    target.write_text(text, encoding='utf-8', newline='\n')
    for name in ('trace_port.h', 'trace_session.h'):
        (output/name).unlink()
    receipt = dict(schema=1, baselineGeneratedSupervisorSha256=BASE,
        baselineGeneratorSha256=BASE_GENERATOR, sourceRecipe=parent,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        generatedSourceSha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        loggingCarrier='systemd-journal', requiredTraceDevice=False,
        userSerialConsoleGrant=False, otherSourceBytesUnchanged=True,
        originalGeneratedProfilePreserved=True, companionAndAdmissionObserversPreserved=True,
        synchronousReceiptOrderEstablished=False, inputImplementationChanged=False,
        gpuProtocolChanged=False, probeSessionStillBounded=True,
        normalAccountBootstrapImplemented=False, dailyUseAccepted=False)
    with (output/'receipt.json').open('w', encoding='utf-8', newline='\n') as stream:
        json.dump(receipt, stream, indent=2)
        stream.write('\n')
    return receipt


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(generate(Path(__file__).resolve().parent.parent, args.output), indent=2))
