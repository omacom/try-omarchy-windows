"""Export real launcher arguments through a Go overlay and check the native composer.

No launcher source is changed. This executes only the selected pure test, not
the launcher, QEMU, a renderer, or desktop/input automation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--go', type=Path, required=True)
    parser.add_argument('--composer-test', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    app = here.parent.parent / 'app'
    out = args.output.resolve(); out.mkdir(exist_ok=False)
    sources = {str(p.relative_to(app)): sha(p) for p in app.rglob('*') if p.is_file() and p.suffix in ('.go', '.mod', '.sum')}
    virtual = app / 'zz_native_gpu_argument_export_test.go'
    if virtual.exists(): raise ValueError('Export overlay collides with launcher source')
    overlay = out / 'overlay.json'
    overlay.write_text(json.dumps({'Replace': {str(virtual): str(here / 'launcher-args-probe/export_test.go')}}), encoding='utf-8')
    export = out / 'launcher-arguments.json'
    env = os.environ.copy(); env['OMARCHY_NATIVE_GPU_ARGS_OUTPUT'] = str(export)
    env['GOTOOLCHAIN'] = 'local'; env['GOMAXPROCS'] = '2'
    command = [str(args.go.resolve(strict=True)), 'test', '-overlay', str(overlay), '-run', '^TestNativeGPUArgumentExport$', '-count=1', '.']
    options = dict(creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS) if os.name == 'nt' else {}
    with (out / 'go.log').open('x') as log:
        subprocess.run(command, cwd=app, env=env, check=True, stdout=log, stderr=subprocess.STDOUT, **options)
    if not export.is_file(): raise ValueError('Go did not execute the required export test')
    probe = args.composer_test.resolve(strict=True)
    result = subprocess.run([str(probe), str(export)], capture_output=True, text=True, check=True, **options)
    proof = json.loads(result.stdout)
    expected = dict(launcherProfilesVerified=5, compatibleProfiles=2, unsupportedProfilesRejected=3,
        officialArgumentsPreserved=True, qemuLaunched=False, inputInjected=False, processHandoffVerified=False, dailyUseAccepted=False)
    if proof != expected: raise ValueError('Composer test has wrong proof scope')
    after = {str(p.relative_to(app)): sha(p) for p in app.rglob('*') if p.is_file() and p.suffix in ('.go', '.mod', '.sum')}
    if sources != after or virtual.exists(): raise ValueError('Launcher source changed')
    receipt = dict(schema=1, proof=proof, actualGoLauncherSourceHashes=sources,
        launcherArgumentsSha256=sha(export), composerTestSha256=sha(probe), goSha256=sha(args.go),
        composerHeaderSha256=sha(here / 'qemu_graphics_arguments.h'),
        composerTestSourceSha256=sha(here / 'qemu_launcher_arguments_test.cpp'),
        exportTestSha256=sha(here / 'launcher-args-probe/export_test.go'), verifierSha256=sha(Path(__file__)),
        sourceUnchanged=True, hardwareTested=False, windowsOpened=0)
    with (out / 'receipt.json').open('x', encoding='utf-8', newline='\n') as f:
        json.dump(receipt, f, indent=2); f.write('\n')
    print('PASS: actual launcher-generated arguments preserved; no VM/input/window; process handoff unverified')


if __name__ == '__main__': main()
