"""Compile the explicit guest bootstrap and prove the old default is unchanged.

Runs only the real PID1 refusal and pure kernel-option policy on the host. Mount,
overlay installation, provisioning and normal GPU boot need guest validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

GPU = Path(__file__).resolve().parent
BASELINE = '2040dcd82174256c323df2751454ef5bd3fae6cad2b028d2ee6e0a624ffcd15d'
FILES = ('normal_root_guest_init.c', 'normal_session_root_payload.h',
         'normal_session_command_line.h', 'test_normal_session_bootstrap.py')
GUARDS = ('#ifndef WDDM_NORMAL_SESSION_BOOTSTRAP\n',
          '#else\n#include "normal_session_root_payload.h"\n#endif\n',
          '#ifdef WDDM_NORMAL_SESSION_BOOTSTRAP\n    if(!graphics || access("/normal-session-overlay",F_OK)) return 125;\n#endif\n')
POLICY = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "normal_session_command_line.h"
int main(void) {
    const struct { const char* text;int selected; } cases[] = {
        {"",0},{"console=tty0",0},{"tryomarchy.nvgpu=1",1},
        {"console=tty0 tryomarchy.nvgpu=1 quiet",1},
        {"\ttryomarchy.nvgpu=1\r\n",1},{"tryomarchy.nvgpu=0",0},
        {"tryomarchy.nvgpu=2",0},{"tryomarchy.nvgpu=",0},
        {"tryomarchy.nvgpu=11",0},{"tryomarchy.nvgpu=1x",0},
        {"tryomarchy.nvgpu=1 tryomarchy.nvgpu=1",0},
        {"tryomarchy.nvgpu=0 tryomarchy.nvgpu=1",0},
        {"tryomarchy.nvgpu=1 tryomarchy.nvgpu=0",0},
        {"other.tryomarchy.nvgpu=1",0},
        {"tryomarchy.instant=1 tryomarchy.nvgpu=1",1}
    };
    if(wddm_normal_session_selected(NULL)) return 1;
    for(unsigned i=0;i<sizeof cases/sizeof cases[0];++i) {
        char text[256];strcpy(text,cases[i].text);
        if(wddm_normal_session_selected(text)!=cases[i].selected) {
            fprintf(stderr,"BOOT_POLICY_FAILURE case=%u\n",i);return 1;
        }
    }
    puts("NORMAL_SESSION_BOOT_POLICY_PASS controls=16 gpu=false");
}
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(output, cc='cc'):
    if os.getpid() == 1:
        raise RuntimeError('Run CPU bootstrap controls as an ordinary child process')
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    frozen = output/'source'
    frozen.mkdir()
    for name in FILES:
        (frozen/name).write_bytes((GPU/name).read_bytes())
    current = (frozen/'normal_root_guest_init.c').read_text()
    original = current
    for guard in GUARDS:
        if original.count(guard) != 1:
            raise ValueError('Default bootstrap source boundary changed')
        original = original.replace(guard, '', 1)
    if hashlib.sha256(original.encode()).hexdigest() != BASELINE:
        raise ValueError('Recovered original bootstrap digest mismatch')
    before = output/'original'
    before.mkdir()
    (before/'normal_root_guest_init.c').write_text(original, encoding='utf-8', newline='\n')
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
    commands = []
    for name, source, extra in (('original', before/'normal_root_guest_init.c', []),
                                ('default', frozen/'normal_root_guest_init.c', []),
                                ('normal', frozen/'normal_root_guest_init.c', ['-DWDDM_NORMAL_SESSION_BOOTSTRAP=1'])):
        command = [cc, *flags, '-static', *extra, str(source), '-o', str(output/(name+'-init'))]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (output/(name+'-compile.log')).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(name+' compile failed: '+str(output))
        commands.append(command)
    preprocessed = []
    for source in (before/'normal_root_guest_init.c', frozen/'normal_root_guest_init.c'):
        preprocessed.append(subprocess.check_output([cc, '-std=c11', '-E', '-P', str(source)]))
    if preprocessed[0] != preprocessed[1] or (output/'original-init').read_bytes() != (output/'default-init').read_bytes():
        raise AssertionError('Default bootstrap behavior or compiled bytes changed')
    refusals = []
    for name in ('original', 'default', 'normal'):
        result = subprocess.run([str(output/(name+'-init'))], capture_output=True, text=True, timeout=5)
        if result.returncode != 125 or result.stdout or result.stderr:
            raise AssertionError('Guest-only PID1 guard failed for '+name)
        refusals.append(name)
    normal = (output/'normal-init').read_bytes()
    if b'omarchy-wddm-normal-graphics.service' in normal or b'ordinaryUid=1000' in normal:
        raise AssertionError('Diagnostic account/service leaked into normal bootstrap')
    (frozen/'policy.c').write_text(POLICY, encoding='utf-8', newline='\n')
    command = [cc, *flags, str(frozen/'policy.c'), '-o', str(output/'policy')]
    subprocess.run(command, check=True)
    result = subprocess.run([str(output/'policy')], capture_output=True, text=True, check=True)
    (output/'policy.log').write_text(result.stdout+result.stderr)
    receipt = dict(schema=1, sourceSha256={name:sha(frozen/name) for name in FILES},
                   originalSourceSha256=BASELINE, defaultPreprocessedCodeExact=True, defaultBinaryExact=True,
                   binaries={name:sha(output/(name+'-init')) for name in ('original', 'default', 'normal')},
                   staticBootstrapCompiled=True, pid1Refusals=refusals, policyControls=16,
                   diagnosticServiceExcluded=True, commands=commands, mountsExecuted=False,
                   overlayInstalled=False, guestBooted=False, gpuUsed=False, normalSessionVerified=False)
    (output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
    print('NORMAL_SESSION_BOOTSTRAP_PASS defaultBinaryExact=true pid1Refusals=3 policyControls=16 guestBooted=false')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True)
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    args = parser.parse_args()
    run(args.output, args.cc)
