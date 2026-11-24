"""Compile pinned uploader/state-tracker functions with CPU callbacks only."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

UPLOADER = 'src/gallium/auxiliary/util/u_upload_mgr.c'
TRACKER = 'src/mesa/state_tracker/st_atom_constbuf.c'


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--msvc', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    manifest = json.loads((here / 'constbuf-failure-patch.json').read_text(encoding='utf-8'))
    upload_manifest = json.loads((here / 'upload-failure-patch.json').read_text(encoding='utf-8'))
    digest = lambda raw: hashlib.sha256(raw).hexdigest()
    original = {relative: (args.source / relative).read_bytes() for relative in (UPLOADER, TRACKER)}
    check((args.source / 'VERSION').read_text(encoding='utf-8').strip() == manifest['mesaVersion'],
          'Requires pinned Mesa version')
    for relative, expected in [(UPLOADER, upload_manifest['files'][UPLOADER]['originalSha256']),
                               (TRACKER, manifest['files'][TRACKER]['originalSha256'])]:
        check(digest(original[relative]) == expected, 'Use the original pinned source: ' + relative)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    creationflags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)

    def apply(script, root, succeeds=True):
        result = subprocess.run([sys.executable, str(script), str(root)], capture_output=True,
                                timeout=15, creationflags=creationflags)
        check((result.returncode == 0) == succeeds,
              'Patch result differs: ' + result.stderr.decode('utf-8', 'replace'))

    def tree(name, version='26.0.8\n', uploader=None, tracker=None):
        root = out / name
        for relative, data in [(UPLOADER, original[UPLOADER] if uploader is None else uploader),
                               (TRACKER, original[TRACKER] if tracker is None else tracker)]:
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (root / 'VERSION').write_text(version, encoding='utf-8', newline='\n')
        return root

    root = tree('patched-source')
    apply(here / 'apply_mesa_upload_failure.py', root)
    uploader = (root / UPLOADER).read_bytes()
    check(digest(uploader) == manifest['requiredUploaderSha256'], 'Uploader dependency differs')
    apply(here / 'apply_mesa_constbuf_failure.py', root)
    fixed = (root / TRACKER).read_bytes()
    check(digest(fixed) == manifest['files'][TRACKER]['patchedSha256'], 'Patched tracker differs')
    before = (root / TRACKER).stat().st_mtime_ns
    apply(here / 'apply_mesa_constbuf_failure.py', root)
    check((root / TRACKER).read_bytes() == fixed and (root / TRACKER).stat().st_mtime_ns == before,
          'Repeated application rewrote the source')
    for name, version, up, st in [
            ('wrong-version', '0.0\n', uploader, original[TRACKER]),
            ('missing-uploader-fix', '26.0.8\n', original[UPLOADER], original[TRACKER]),
            ('modified-tracker', '26.0.8\n', uploader, original[TRACKER] + b'/* changed */\n')]:
        rejected = tree(name, version, up, st)
        apply(here / 'apply_mesa_constbuf_failure.py', rejected, False)
        check((rejected / TRACKER).read_bytes() == st and (rejected / UPLOADER).read_bytes() == up,
              'Rejected patch modified source')
    corrupt = out / 'corrupt-recipe'
    corrupt.mkdir()
    for name in ('apply_mesa_constbuf_failure.py', 'constbuf-failure-patch.json',
                 '0010-mesa-constbuf-upload-failure.patch'):
        shutil.copyfile(here / name, corrupt / name)
    with (corrupt / '0010-mesa-constbuf-upload-failure.patch').open('ab') as stream:
        stream.write(b'corrupt patch\n')
    rejected = tree('bad-patch', uploader=uploader)
    apply(corrupt / 'apply_mesa_constbuf_failure.py', rejected, False)
    check((rejected / TRACKER).read_bytes() == original[TRACKER], 'Corrupt patch modified source')

    base = (here / 'upload_failure_stubs.h').read_text(encoding='utf-8')
    declaration = 'struct pipe_context { struct pipe_screen *screen; };'
    check(base.count(declaration) == 1, 'CPU model pipe declaration differs')
    extended = '''struct u_upload_mgr;
struct pipe_constant_buffer;
struct pipe_context {
    struct pipe_screen *screen;
    struct u_upload_mgr *const_uploader;
    void (*set_constant_buffer)(struct pipe_context*, unsigned, unsigned, const struct pipe_constant_buffer*);
    void (*set_inlinable_constants)(struct pipe_context*, unsigned, unsigned, const uint32_t*);
};'''
    base = base.replace(declaration, extended)
    callbacks = (here / 'constbuf_failure_stubs.h').read_text(encoding='utf-8')
    cases = (here / 'constbuf_failure_cases.h').read_text(encoding='utf-8')
    strip_includes = lambda text: re.sub(r'^#include[^\n]*\n', '', text, flags=re.M)
    # Preserve complete functions, including every branch; no hand-rewritten
    # implementation. Later stage-specific wrappers and UBO functions are outside
    # this regression's scope. Include the source's original copyright notice.
    marker = '/**\n * Vertex shader:\n */'
    warnings_on = '''#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4245 4018)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
'''
    warnings_off = '''
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
'''
    results = {}
    for label, data in [('baseline', original[TRACKER]), ('patched', fixed)]:
        source = data.decode('utf-8')
        check(source.count(marker) == 1, 'Pinned function boundary differs')
        functions = strip_includes(source.split(marker)[0])
        check(functions.count('st_upload_constants(struct') == 1 and
              functions.count('st_unbind_unused_cb0(struct') == 1, 'Complete functions missing')
        unit = out / (label + '.c')
        unit.write_text(base + '\n' + callbacks + '\n' + warnings_on +
                        strip_includes(uploader.decode('utf-8')) + '\n' +
                        '#define memcpy checked_memcpy\n' + functions + '\n#undef memcpy\n' +
                        warnings_off + '\n' + cases, encoding='utf-8', newline='\n')
        executable = out / (label + ('.exe' if sys.platform == 'win32' else ''))
        guarded = int(label == 'patched')
        if args.msvc:
            command = [args.cc, '/nologo', '/TC', '/std:c11', '/W4', '/WX', '/O2', '/DNDEBUG',
                       '/DEXPECT_GUARDED=' + str(guarded), str(unit), '/Fe:' + str(executable)]
        else:
            command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-DNDEBUG',
                       '-DEXPECT_GUARDED=' + str(guarded), str(unit), '-o', str(executable)]
        process = subprocess.run(command, cwd=out, capture_output=True, timeout=90,
                                 creationflags=creationflags)
        (out / (label + '-compile.log')).write_bytes(process.stdout + process.stderr)
        check(process.returncode == 0, label + ' CPU compile failed; see its private log')
        process = subprocess.run([str(executable)], cwd=out, capture_output=True, timeout=20,
                                 creationflags=creationflags)
        (out / (label + '-results.jsonl')).write_bytes(process.stdout)
        (out / (label + '-stderr.log')).write_bytes(process.stderr)
        check(process.returncode == 0, label + ' CPU invariant failed; see its private log')
        rows = [json.loads(row) for row in process.stdout.splitlines()]
        check(len(rows) == 338, 'Unexpected case count')
        faults = [row for row in rows if row.get('fault')]
        check(len(faults) == 216, 'Fault matrix incomplete')
        if label == 'baseline':
            check(sum(row['invalidCopy'] for row in faults) == 144 and
                  sum(row['invalidState'] for row in faults) == 144 and
                  sum(row['invalidBind'] for row in faults) == 216,
                  'Original invalid-write/bind paths not reproduced')
        else:
            check(all(row['guarded'] and row['errors'] == 1 and row['retry'] for row in faults),
                  'Patched failure/retry path failed')
        results[label] = rows
    for relative in (UPLOADER, TRACKER):
        check((args.source / relative).read_bytes() == original[relative], 'Input tree changed')
    receipt = dict(schema=1, mesaVersion=manifest['mesaVersion'], sourceSha256=manifest['files'],
                   requiredUploaderSha256=manifest['requiredUploaderSha256'],
                   actualWholeUploaderCompiled=True, actualWholeTrackerFunctionsCompiled=2,
                   cpuCallbackModel=True, nullWritesInterceptedWithoutDereference=True,
                   libraryAssertionsDisabled=True, explicitFixtureChecksActive=True,
                   baselineCases=338, patchedCases=338, failureCasesPerVariant=216,
                   positiveCasesPerVariant=122, baselineInvalidCopies=144,
                   baselineInvalidStateWrites=144, baselineInvalidBinds=216,
                   patchedFaultsGuarded=216, patchedCpuRetries=216, allEightShaderStages=True,
                   callerReleaseOwnershipChecked=True, cleanupReferenceCountsChecked=True,
                   patchIdempotent=True, rejectedInputs=4, inputTreeUnchanged=True,
                   gpuUsed=False, vmLaunched=False, wslLaunched=False, nativeRuntimeBuilt=False,
                   intentionalNativeCrash=False, quickshellCrashFixed=False)
    (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print('MESA_CONSTBUF_FAILURE_CPU_PASS cases=676 guardedFaults=216 cpuRetries=216 gpu=false')


if __name__ == '__main__':
    main()
