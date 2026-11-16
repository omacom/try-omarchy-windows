"""Check compiled OpenGL support and retained official input source."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def verify(source, header):
    source, header = Path(source), Path(header)
    value = json.loads((source / 'sdl-candidate-provenance.json').read_text(encoding='utf-8'))
    for name, digest in value['inputFiles'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('Input source changed after official patch application: ' + name)
    for name, digest in value.get('graphicsRelaySources', {}).items():
        if hashlib.sha256((source / 'hw/display' / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError('Graphics relay source changed during compilation: ' + name)
    reconnect = value.get('socketReconnectFix')
    if (not reconnect or reconnect.get('upstreamCommit') != 'ddf5738503e2ba6adb74359abf3b6e7aa81af420' or
            hashlib.sha256((source/'chardev/char-socket.c').read_bytes()).hexdigest() != reconnect.get('sourceSha256')):
        raise RuntimeError('Socket reconnect backport missing or changed during compilation')
    lines = header.read_text(encoding='utf-8').splitlines()
    if not any(re.fullmatch(r'#define\s+CONFIG_OPENGL(?:\s+1)?', line.strip()) for line in lines):
        raise RuntimeError('Candidate was not compiled with OpenGL support')
    print('SDL_CANDIDATE_BUILD_VERIFIED officialInputRetained=true guestFrameVerified=false')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('header', type=Path)
    args = parser.parse_args()
    verify(args.source, args.header)
