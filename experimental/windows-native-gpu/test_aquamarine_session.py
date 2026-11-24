"""Exercise the session adapter against the actual pinned Aquamarine source.

Copies source, checks the complete inverse and untouched files, checks exact
reapplication, and rejects damaged anchors/partial patches. No GPU is used.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

GPU=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('aquamarine_session',GPU/'apply_aquamarine_session.py')
module=importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

def hashes(root):
    return {path.relative_to(root).as_posix():hashlib.sha256(path.read_bytes()).hexdigest()
            for path in root.rglob('*') if path.is_file() and '.git' not in path.relative_to(root).parts}

def run(source,output):
    source,output=Path(source).resolve(),Path(output).resolve()
    output.mkdir(parents=True,exist_ok=False)
    frozen=output/'control-source'
    frozen.mkdir()
    for name in ('apply_aquamarine_session.py','test_aquamarine_session.py'):
        (frozen/name).write_bytes((GPU/name).read_bytes())
    target=output/'source'
    shutil.copytree(source,target,ignore=shutil.ignore_patterns('.git'))
    before=hashes(target)
    path=target/'src/backend/Headless.cpp'
    original=path.read_text()
    module.apply(target)
    selected=path.read_text()
    after=hashes(target)
    changed=[name for name in before if before[name]!=after.get(name)]
    assert changed==['src/backend/Headless.cpp'] and before.keys()==after.keys(),changed
    recovered=selected
    for previous,current in module.PAIRS:
        assert recovered.count(current)==1
        recovered=recovered.replace(current,previous,1)
    assert recovered==original
    module.apply(target)
    assert after==hashes(target)
    negatives=0
    for previous,_ in module.PAIRS:
        damaged=original.replace(previous,'/* source boundary deliberately removed */',1)
        try:module.patch_text(damaged)
        except ValueError:negatives+=1
        else:raise AssertionError('Missing source boundary accepted')
    for _,current in module.PAIRS:
        damaged=selected.replace(current,current+'\n/* altered adapter */',1)
        # A duplicated complete adapter is invalid, not a successful reapplication.
        damaged=damaged.replace(current,current+'\n'+current,1)
        try:module.patch_text(damaged)
        except ValueError:negatives+=1
        else:raise AssertionError('Duplicated session block accepted')
    path.write_text(selected+'// deliberately altered trailing source\n')
    try:module.apply(target)
    except ValueError:negatives+=1
    else:raise AssertionError('Altered completed source accepted')
    path.write_bytes(selected.encode())
    version=target/'VERSION'
    original_version=version.read_bytes()
    version.write_text('0.15.2\n')
    try:module.apply(target)
    except ValueError:negatives+=1
    else:raise AssertionError('Wrong version accepted')
    version.write_bytes(original_version)
    assert after==hashes(target)
    receipt=dict(sourceVersion='0.15.1',changedFiles=changed,sourceFiles=len(before),
                 unchangedOtherFiles=True,fullInverseExact=True,reapplicationExact=True,
                 negativeControlsPassed=negatives,gpuUsed=False,desktopLaunched=False,
                 headlessOriginalSha256=before[changed[0]],headlessSelectedSha256=after[changed[0]],
                 sourceSha256={name:hashlib.sha256((frozen/name).read_bytes()).hexdigest() for name in ('apply_aquamarine_session.py','test_aquamarine_session.py')})
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source');parser.add_argument('output')
    args=parser.parse_args();run(args.source,args.output)
