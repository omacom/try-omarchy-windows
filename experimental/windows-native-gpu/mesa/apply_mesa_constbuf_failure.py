#!/usr/bin/env python3
"""Apply the pinned state-tracker upload-failure guard; source files only."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile

RELATIVE = 'src/mesa/state_tracker/st_atom_constbuf.c'
UPLOADER = 'src/gallium/auxiliary/util/u_upload_mgr.c'


def transform(text):
    include = '#include "main/shaderapi.h"\n'
    site = '''            alignment, &cb.buffer_offset, &cb.buffer, &releasebuf, (void**)&ptr);

         int uniform_bytes'''
    guarded = '''            alignment, &cb.buffer_offset, &cb.buffer, &releasebuf, (void**)&ptr);
         if (unlikely(!ptr)) {
            st_add_releasebuf(st, releasebuf);
            st_unbind_unused_cb0(st, stage);
            _mesa_error(st->ctx, GL_OUT_OF_MEMORY, "st_upload_constants");
            return;
         }

         int uniform_bytes'''
    if text.count(include) != 1 or text.count(site) != 1:
        raise ValueError('Pinned constant-buffer failure site differs')
    return text.replace(include, include + '#include "main/errors.h"\n').replace(site, guarded)


def apply(root):
    root = root.resolve(strict=True)
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'constbuf-failure-patch.json').read_text(encoding='utf-8'))
    digest = lambda raw: hashlib.sha256(raw).hexdigest()
    patch = (here / '0010-mesa-constbuf-upload-failure.patch').read_bytes().replace(b'\r\n', b'\n')
    if digest(patch) != manifest['patchSha256']:
        raise ValueError('Constant-buffer patch digest differs')
    if (root / 'VERSION').read_text(encoding='utf-8').strip() != manifest['mesaVersion']:
        raise ValueError('Requires pinned Mesa version')
    for relative in (RELATIVE, UPLOADER):
        path = root / relative
        if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root):
            raise ValueError('Source path escapes the tree or is a symlink')
    if digest((root / UPLOADER).read_bytes()) != manifest['requiredUploaderSha256']:
        raise ValueError('Apply the pinned uploader ownership fix first')
    path = root / RELATIVE
    raw = path.read_bytes()
    record = manifest['files'][RELATIVE]
    if digest(raw) == record['patchedSha256']:
        return 'Pinned constant-buffer failure guard already verified'
    if digest(raw) != record['originalSha256']:
        raise ValueError('Constant-buffer source differs; no files changed')
    before = raw.decode('utf-8')
    after = transform(before)
    delta = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                      fromfile='a/' + RELATIVE, tofile='b/' + RELATIVE))
    encoded = after.encode('utf-8')
    if delta.encode() != patch or digest(encoded) != record['patchedSha256']:
        raise ValueError('Generated source differs from the reviewed patch')
    mode = stat.S_IMODE(path.stat().st_mode)
    fd, temporary = tempfile.mkstemp(prefix='.constbuf-fix-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        if path.read_bytes() != raw:
            raise ValueError('Source changed during preparation')
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return 'Pinned constant-buffer failure guard applied and verified'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    args = parser.parse_args()
    try:
        print(apply(args.source))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
