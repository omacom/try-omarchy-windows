"""Actual PowerShell preflight: invalid diagnostic profiles must never build."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import subprocess
import tempfile


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-script',type=Path,default=Path(__file__).with_name('build.ps1'))
    parser.add_argument('--output-directory',type=Path)
    args=parser.parse_args()
    if os.name!='nt':parser.error('This control requires Windows PowerShell; it never runs a GPU workload')
    out=args.output_directory or Path(tempfile.mkdtemp(prefix='residency-build-controls-'))
    if args.output_directory:out.mkdir(parents=True,exist_ok=False)
    rows=[]
    for name,flags in [
        ('no-lab',['-DiagnosticResidencyRejectionFrame','100']),
        ('too-late',['-HighResolutionDiagnostic','-DiagnosticResidencyRejectionFrame','1701']),
        ('streaming',['-StreamingDiagnostic','-DiagnosticResidencyRejectionFrame','100'])]:
        destination=out/name
        command=['powershell.exe','-NoProfile','-File',str(args.build_script.resolve()),'-OutputDirectory',str(destination.resolve()),*flags]
        result=subprocess.run(command,capture_output=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
        (out/(name+'.log')).write_bytes(result.stdout+result.stderr)
        assert result.returncode!=0 and not destination.exists(),(name,result.returncode)
        assert b'Diagnostic residency rejection requires' in result.stderr,(name,result.stderr)
        rows.append(dict(name=name,exit=result.returncode,outputDirectoryCreated=False,compilerStarted=False,gpu=False))
    receipt=dict(buildScriptSha256=hashlib.sha256(args.build_script.read_bytes()).hexdigest(),cases=rows)
    (out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
