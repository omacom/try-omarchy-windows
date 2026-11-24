"""Exercise library-alias checks using copied files and real symlinks only."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
from verify_mesa_stage import verify_aliases


LINKS = {
    'usr/lib/libEGL.so': 'libEGL.so.1',
    'usr/lib/libEGL.so.1': 'libEGL.so.1.0.0',
    'usr/lib/libgbm.so': 'libgbm.so.1',
    'usr/lib/libgbm.so.1': 'libgbm.so.1.0.0',
    'usr/lib/libGLESv2.so': 'libGLESv2.so.2',
    'usr/lib/libGLESv2.so.2': 'libGLESv2.so.2.0.0',
}


def check(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--recipe', type=Path,
                        help='Versioned layout contract; omitted uses the historical contract')
    args = parser.parse_args()
    source = args.stage.resolve(strict=True)
    out = args.output.resolve()
    check(not out.is_relative_to(source), 'Test output must be outside the supplied staging tree')
    here = Path(__file__).resolve().parent
    recipe = args.recipe.resolve(strict=True) if args.recipe else here
    contract = json.loads((recipe / 'stage-contract.json').read_text(encoding='utf-8'))
    aliases = contract['libraryAliases']
    check(set(aliases) == set(LINKS), 'Loader alias names differ from the required EGL/GBM/GLES2 names')
    out.mkdir(parents=True, exist_ok=False)
    fixture = out / 'fixture'
    fixture.mkdir()
    original = {}
    for relative in contract['libraries']:
        path = (source / relative).resolve(strict=True)
        check(path.is_relative_to(source), 'Input library escapes supplied staging tree')
        target = fixture / relative
        check(target.resolve().is_relative_to(fixture), 'Fixture destination escapes owned output')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        original[relative] = digest(path)
    for relative, target in LINKS.items():
        (fixture / relative).symlink_to(target)
    (out / 'outside.bin').write_bytes(b'Owned data outside the fixture; never executed.\n')
    cases = []

    def accepts(name):
        observed = verify_aliases(fixture, aliases)
        check(observed == LINKS, 'Valid alias graph differs')
        cases.append(dict(name=name,expectedAccepted=True,accepted=True))

    def rejects(name, exception_types=(ValueError, OSError, RuntimeError), reason=None):
        try:
            verify_aliases(fixture, aliases)
        except exception_types as error:
            check(reason is None or reason in str(error), 'Unexpected rejection reason: ' + str(error))
            cases.append(dict(name=name,expectedAccepted=False,accepted=False,errorType=type(error).__name__))
        else:
            raise ValueError('Invalid alias graph accepted: ' + name)

    @contextmanager
    def replace(relative, target=None, regular=False):
        path = fixture / relative
        before = os.readlink(path)
        path.unlink()
        try:
            if regular:
                path.write_bytes(b'Not a symbolic link.\n')
            elif target is not None:
                path.symlink_to(target)
            yield
        finally:
            path.unlink(missing_ok=True)
            path.symlink_to(before)

    accepts('valid-loader-chains')
    for relative in LINKS:
        wrong = 'libgbm.so.1' if Path(relative).name.startswith('libEGL') else 'libEGL.so.1'
        with replace(relative, wrong):
            rejects('wrong-target-' + Path(relative).name, (ValueError,), 'wrong library')
    with replace('usr/lib/libEGL.so', str(out / 'outside.bin')):
        rejects('alias-escapes-fixture', (ValueError,), 'escapes destination')
    with replace('usr/lib/libEGL.so', 'missing-library.so'):
        rejects('dangling-alias', (FileNotFoundError,))
    with replace('usr/lib/libEGL.so.1', 'libEGL.so'):
        rejects('alias-cycle', (OSError, RuntimeError))
    with replace('usr/lib/libEGL.so'):
        rejects('missing-required-alias', (ValueError,), 'alias missing')
    with replace('usr/lib/libEGL.so', regular=True):
        rejects('regular-file-alias', (ValueError,), 'alias missing')
    optional = fixture / 'usr/lib/optional-link'
    optional.symlink_to(str(out / 'outside.bin'))
    try:
        rejects('unlisted-link-escapes-fixture', (ValueError,), 'escapes destination')
    finally:
        optional.unlink()
    accepts('restored-loader-chains')
    check(all(digest(source / n) == h and digest(fixture / n) == h for n, h in original.items()),
          'Alias controls modified input or copied library contents')
    result = dict(schema=1,cases=cases,validCases=2,rejectedCases=12,
                  librarySha256=original,libraryContentsPreserved=True,inputStageUnchanged=True,
                  aliasGateTested=True,wholeLinuxStagingVerified=False,runtimeLoaded=False,
                  gpuWorkloadRun=False,vmLaunched=False,hardwareIntegrationTested=False,dailyUseAccepted=False)
    (out / 'result.json').write_text(json.dumps(result,indent=2) + '\n',encoding='utf-8',newline='\n')
    print('MESA_STAGE_ALIAS_CONTROLS_PASS cases=' + str(len(cases)) + ' runtimeLoaded=false')


if __name__ == '__main__':
    main()
