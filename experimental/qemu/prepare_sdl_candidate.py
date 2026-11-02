"""Apply the official runtime patches to a separate SDL/OpenGL lab candidate."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

INPUT_PATHS = ('ui/sdl2.c', 'include/ui/sdl2.h', 'ui/input.c', 'hw/input', 'include/hw/input')


def prepare(source):
    source = Path(source).resolve()
    root = Path(__file__).resolve().parents[2]
    recipe = root / 'runtime-build'
    lock_path = recipe / 'sources.lock.json'
    lock = json.loads(lock_path.read_text(encoding='utf-8'))['qemu']
    git = lambda *args: subprocess.check_output(['git', '-C', str(source), *args], text=True).strip()
    if git('rev-parse', 'HEAD') != lock['commit']:
        raise RuntimeError('SDL candidate must use the locked official QEMU base')
    if git('diff', '--name-only', 'HEAD', '--', *INPUT_PATHS):
        raise RuntimeError('Lab preparation changed official input source before patching')
    target = source / 'sdl-candidate-provenance.json'
    if target.exists():
        raise RuntimeError('Use a fresh SDL candidate source')
    applied = []
    input_files = {'ui/sdl2.c', 'include/ui/sdl2.h', 'ui/input.c'}
    for item in lock['patches']:
        path = (recipe / item['file']).resolve()
        if not path.is_relative_to(recipe) or hashlib.sha256(path.read_bytes()).hexdigest() != item['sha256']:
            raise RuntimeError('Official QEMU patch changed: ' + item['file'])
        for name in re.findall(r'^\+\+\+ [^/ ]+/(.+)$', path.read_text(encoding='utf-8'), re.M):
            if name.startswith(('hw/input/', 'include/hw/input/')):
                input_files.add(name)
        subprocess.run(['git', '-C', str(source), 'apply', '--check', str(path)], check=True)
        subprocess.run(['git', '-C', str(source), 'apply', str(path)], check=True)
        applied.append(item)
    graphics_patch = root / 'experimental/qemu/sdl-gpu-relay.patch'
    subprocess.run(['git', '-C', str(source), 'apply', '--check', str(graphics_patch)], check=True)
    subprocess.run(['git', '-C', str(source), 'apply', str(graphics_patch)], check=True)
    graphics_sources = {}
    for name in ('wddm-sdl-relay.c', 'wddm-sdl-command.h', 'wddm-sdl-boot.h'):
        path = root / 'experimental/qemu' / name
        shutil.copyfile(path, source / 'hw/display' / name)
        graphics_sources[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    value = dict(schema=1, qemuBaseCommit=lock['commit'],
                 officialRuntimeLockSha256=hashlib.sha256(lock_path.read_bytes()).hexdigest(),
                 officialRuntimePatches=applied, officialInputSourceInitiallyUnchanged=True,
                 socketReconnectFix=dict(upstreamCommit='ddf5738503e2ba6adb74359abf3b6e7aa81af420',
                     patchSha256=hashlib.sha256((root/'experimental/qemu/socket-reconnect-yank.patch').read_bytes()).hexdigest(),
                     sourceSha256=hashlib.sha256((source/'chardev/char-socket.c').read_bytes()).hexdigest(),
                     reconnectRuntimeVerified=False),
                 graphicsRelayPatchSha256=hashlib.sha256(graphics_patch.read_bytes()).hexdigest(),
                 graphicsRelaySources=graphics_sources,
                 graphicsRelayProtocolVersion=2,
                 graphicsBootConsole=dict(optionalProperty='boot-display', singleWindow=True,
                     normalGuestBootVerified=False, gpuTransitionVerified=False),
                 graphicsWindowAuditSchema=1,
                 inputFiles={name:hashlib.sha256((source/name).read_bytes()).hexdigest()
                             for name in sorted(input_files) if (source/name).is_file()},
                 graphicsBuildCandidate=True, guestFrameVerified=False, officialFrontendIntegrationVerified=False)
    with target.open('x', encoding='utf-8', newline='\n') as file:
        json.dump(value, file, indent=2)
        file.write('\n')
    print('SDL candidate reuses', len(applied), 'unmodified official runtime patches')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    prepare(parser.parse_args().source)
