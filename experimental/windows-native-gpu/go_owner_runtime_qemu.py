"""Run the actual Go supervisor boundary against the unchanged hidden GPU harness.

The Go test executable invokes the same start/publish/stop/wait functions used
by main.supervise. It does not run the launcher UI or establish normal guest
boot acceptance. Native stdout/stderr and every original GPU oracle stay intact.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from broker_runtime_qemu import load_harness


def main():
    values = {}
    for option in ('--go-owner-test', '--go-owner-receipt'):
        if sys.argv.count(option) != 1: raise ValueError('One ' + option + ' is required')
        index = sys.argv.index(option); values[option] = Path(sys.argv[index + 1]).resolve()
        del sys.argv[index:index + 2]
    executable, receipt = values['--go-owner-test'], values['--go-owner-receipt']
    if not executable.is_file() or receipt.exists(): raise ValueError('Requires a built Go test host and fresh receipt')
    bridge = Path(sys.argv[sys.argv.index('--bridge') + 1]).resolve(strict=True)
    original_popen = subprocess.Popen
    state = dict(schema=1, scope='Go supervisor hidden integration adapter',
        adapterSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        goExecutableSha256=hashlib.sha256(executable.read_bytes()).hexdigest(), parentHarnessCommand=list(sys.argv))
    launched = False

    def popen(command, *arguments, **options):
        nonlocal launched
        if not isinstance(command, (list, tuple)) or Path(command[0]).resolve() != bridge:
            return original_popen(command, *arguments, **options)
        if launched or '--run-qemu' not in command: raise RuntimeError('Unexpected Go supervisor launch boundary')
        launched = True
        environment = dict(options.get('env') or os.environ)
        environment.update(OMARCHY_NATIVE_OWNER_PRIVATE_COMMAND=json.dumps(command),
            OMARCHY_NATIVE_OWNER_PRIVATE_RECEIPT=str(receipt))
        options['env'] = environment
        actual = [str(executable), '-test.run=^TestNativeGPUProcessHost$']
        owner = original_popen(actual, *arguments, **options)
        state.update(originalNativeCommand=list(command), actualGoCommand=actual, goCreatorPid=owner.pid)
        return owner

    try:
        subprocess.Popen = popen
        result = load_harness().main()
    finally:
        subprocess.Popen = original_popen
        with receipt.with_name('go-adapter.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(state, stream, indent=2); stream.write('\n')
    proof = json.loads(receipt.read_bytes())
    if (not launched or proof['actualGoCreatorPid'] != state['goCreatorPid'] or proof['publishedQemuPid'] == proof['bridgePid'] or
            proof['identity']['launcherPid'] != state['goCreatorPid']):
        raise RuntimeError('Go process identity publication mismatch')
    if proof['qemuReapedThroughHeldHandle'] is not True or proof['qemuExit'] != 0 or proof['nativeExit'] != 0:
        raise RuntimeError('Go supervisor did not verify natural owned VM exit')
    return result


if __name__ == '__main__': raise SystemExit(main())
