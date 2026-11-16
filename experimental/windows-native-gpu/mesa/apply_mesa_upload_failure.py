#!/usr/bin/env python3
"""Apply the pinned Gallium uploader failure-lifetime fix; no runtime work."""
import argparse
import difflib
import hashlib
import json
import os
from pathlib import Path
import stat
import tempfile

RELATIVE = 'src/gallium/auxiliary/util/u_upload_mgr.c'


def transform(text):
    old = '      pipe_resource_release(upload->pipe, upload->buffer);\n      return 0;'
    new = '      pipe_resource_release(upload->pipe, upload->buffer);\n      upload->buffer = NULL;\n      return 0;'
    failure = '''      if (unlikely(!buffer_size)) {
         *out_offset = ~0;
         *ptr = NULL;
         *releasebuf = NULL;
         return;
      }'''
    if text.count(old) != 1 or text.count(failure) != 1:
        raise ValueError('Pinned uploader failure sites differ')
    return text.replace(old, new).replace(failure, failure.replace('         *releasebuf = NULL;\n', ''))


def apply(root):
    root = root.resolve(strict=True)
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'upload-failure-patch.json').read_text(encoding='utf-8'))
    digest = lambda raw: hashlib.sha256(raw).hexdigest()
    patch = (here / '0009-gallium-upload-failure-lifetime.patch').read_bytes().replace(b'\r\n', b'\n')
    if digest(patch) != manifest['patchSha256']:
        raise ValueError('Upload-failure patch digest differs')
    if (root / 'VERSION').read_text(encoding='utf-8').strip() != manifest['mesaVersion']:
        raise ValueError('Requires pinned Mesa version')
    path = root / RELATIVE
    if path.is_symlink() or not path.resolve(strict=True).is_relative_to(root):
        raise ValueError('Uploader path escapes the source tree or is a symlink')
    raw = path.read_bytes()
    record = manifest['files'][RELATIVE]
    if digest(raw) == record['patchedSha256']:
        return 'Pinned uploader failure fix already verified'
    if digest(raw) != record['originalSha256']:
        raise ValueError('Uploader source differs; no files changed')
    before = raw.decode('utf-8')
    after = transform(before)
    delta = ''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True),
                                      fromfile='a/' + RELATIVE, tofile='b/' + RELATIVE))
    encoded = after.encode('utf-8')
    if delta.encode() != patch or digest(encoded) != record['patchedSha256']:
        raise ValueError('Generated source differs from the reviewed patch')
    mode = stat.S_IMODE(path.stat().st_mode)
    fd, temporary = tempfile.mkstemp(prefix='.upload-fix-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        if path.read_bytes() != raw:
            raise ValueError('Uploader source changed during preparation')
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return 'Pinned uploader failure fix applied and verified'


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
