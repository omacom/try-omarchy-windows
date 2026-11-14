"""Stage the native inner desktop entry while retaining the official watchdog.

This creates a reviewable build overlay only. It does not install files, change
SDDM, start a session, or supply the not-yet-implemented root activation service.
"""
import argparse
import hashlib
import json
from pathlib import Path

BASE_SHA256='a9ba6904364ef65ca0f8b80ba042272f191f90c035f5b78456f56e56651a1b37'
ORIGINAL=b'Exec=/usr/bin/start-hyprland\n'
NATIVE=b'Exec=/usr/bin/start-hyprland --path /opt/omarchy-nvgpu/bin/hyprland-session-exec\n'

def generate(source,output):
    source,output=Path(source),Path(output)
    raw=source.read_bytes()
    if hashlib.sha256(raw).hexdigest()!=BASE_SHA256 or raw.count(ORIGINAL)!=1:
        raise ValueError('Pinned normal Hyprland desktop entry required; no files changed')
    selected=raw.replace(ORIGINAL,NATIVE,1)
    output.mkdir(parents=True,exist_ok=False)
    target=output/'usr/local/share/wayland-sessions/hyprland.desktop'
    target.parent.mkdir(parents=True)
    target.write_bytes(selected)
    receipt=dict(sourceSha256=BASE_SHA256,selectedSha256=hashlib.sha256(selected).hexdigest(),
                 changedFields=['Exec'],sameDesktopId='hyprland.desktop',watchdogRetained=True,
                 outerOmarchyDesktopChanged=False,uwsmServiceChanged=False,inputCodeChanged=False,
                 installed=False,rootActivationServiceImplemented=False,dailyUseAccepted=False)
    (output/'manifest.json').write_text(json.dumps(receipt,indent=2)+'\n')
    return receipt

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',required=True);parser.add_argument('--output',required=True)
    args=parser.parse_args();print(json.dumps(generate(args.source,args.output)))
