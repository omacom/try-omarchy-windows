"""Audit a real Foot surface composed by Hyprland; keep desktop/display gates separate."""
import argparse
import json
from pathlib import Path
import re
from verify_hyprland_lifecycle_capture import require
from verify_hyprland_queued_presentation import validate as queued


BACKGROUND_REGIONS=((0,32,128,160),(608,640,128,160),(0,32,280,360),(608,640,280,360))
PALETTE=((235,70,70),(70,200,100),(60,130,235))


def validate(report,guest,records,frames,*,expected_frames,require_changed_content=False,normal_root=False):
    result=queued(report,guest,records,frames,(0x11,0x44,0x77),profile=5,
                  expected_frames=expected_frames,background_regions=BACKGROUND_REGIONS,normal_root=normal_root)
    profiles=[row for row in records if 'openGlProfileNegotiated' in row]
    expected=dict(openGlProfileNegotiated=True,version=5,frames=0,commandLimit=0,pendingCommands=16,
        reservationLimit=64,reservedByteLimit=17179869184,requestLimit=0,objectLimit=1024,
        hwQueueLimit=16,vendorObjectLimit=512,syncObjectLimit=192)
    require(len(profiles)==1 and profiles[0]==expected and all(type(profiles[0][key]) is type(value)
            for key,value in expected.items()),'Missing/extraneous/malformed rolling profile')
    require('HYPRLAND_SCENE_FAILURE' not in guest,'Wayland scene failed')
    lines=guest.splitlines()
    def once(line):
        require(lines.count(line)==1,'Missing/duplicate scene receipt: '+line)
        return lines.index(line)
    ready=once('HYPRLAND_SCENE_READY realWaylandClient=true applicationGpuAccelerated=false compositorGpu=true')
    closed=once('HYPRLAND_SCENE_CLOSED clientUnmapped=true childExitMarker=true')
    first=next(i for i,line in enumerate(lines) if line.startswith('HYPRLAND_HOST_QUEUE_RESULT result=0 errno=0 frame=1 audit=1 retries='))
    last=next(i for i,line in enumerate(lines) if f'HYPRLAND_HOST_QUEUE_END frames={expected_frames} target={expected_frames} failed=0' in line)
    require(ready<first<last<closed,'Invalid scene/presentation lifecycle order')
    for index,path in enumerate(('/usr/share/fonts/noto/NotoSans-Regular.ttf',
            '/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf'),1):
        require(once(f'HYPRLAND_SCENE_FONT index={index} file={path}')<ready,'Fonts checked after readiness')
    encoded=re.findall(r'^HYPRLAND_SCENE_CLIENTS_HEX ([0-9a-f]+)$',guest,re.M)
    require(len(encoded)==guest.count('HYPRLAND_SCENE_CLIENTS_HEX ')==1,'Missing/duplicate client metadata')
    try:
        clients=json.loads(bytes.fromhex(encoded[0]).decode('utf-8'))
    except (ValueError,UnicodeError) as error:
        raise ValueError('Malformed client metadata') from error
    require(type(clients) is list and len(clients)==1 and type(clients[0]) is dict,'Expected one actual scene client')
    client=clients[0]
    require(all(client.get(key) is value for key,value in dict(mapped=True,visible=True,hidden=False,
            floating=True,xwayland=False).items()) and client.get('at')==[80,120] and client.get('size')==[480,200] and
            client.get('class')=='wddm-scene' and client.get('title')=='Wayland desktop scene' and
            type(client.get('pid')) is int and client['pid']>0,'Wrong/unmapped Wayland surface')
    palette_counts=[]
    for name in ('compositor-copy-1.raw','compositor-copy-2.raw'):
        pixels=frames[name]
        rgb=lambda x,y:tuple(pixels[(y*640+x)*4+k] for k in (2,1,0))
        # Independent colors come from wayland_scene.ini/sh. This interior
        # excludes text, decorations and the compositor's synthetic cursor.
        known=0
        for y in range(280,300):
            for x in range(96,544):
                require(rgb(x,y)==(16,24,32),'Independent terminal background mismatch')
                known+=1
        counts=dict.fromkeys(PALETTE,0)
        for y in range(120,320):
            for x in range(80,560):
                value=rgb(x,y)
                if value in counts: counts[value]+=1
        require(all(value>=100 for value in counts.values()),'Missing independently specified scene colors')
        palette_counts.append(dict(terminalBackgroundPixelsVerified=known,
            palettePixelsVerified=[dict(rgb=list(color),pixels=counts[color]) for color in PALETTE]))
    changed=result['frames'][0]['rawSha256']!=result['frames'][1]['rawSha256']
    require(not require_changed_content or changed,'Animated scene did not change between audits')
    # Changed whole-frame bytes alone do not independently verify text glyphs
    # or prove every intermediate frame reached the monitor.
    return dict(schema=1,realWaylandSurfaceComposedVerified=True,realWaylandClientClosedVerified=True,
        applicationGpuAccelerationVerified=False,compositorGpuRenderingVerified=True,
        fontsResolvedVerified=True,changedAuditBytes=changed,scopedScenePixelAudit=palette_counts,
        fullTextGlyphOracleVerified=False,fullOmarchySceneVerified=False,desktopVerified=False,
        normalLauncherIntegrationVerified=False,longRunStabilityVerified=False,
        compositor4kVerified=False,nearNativePerformanceVerified=False,queued=result)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--expected-frames',type=int,required=True)
    parser.add_argument('--require-changed-content',action='store_true')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    records=[json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    frames={f'compositor-copy-{i}.raw':(args.report.parent/f'compositor-copy-{i}.raw').read_bytes() for i in (1,2)}
    result=validate(json.loads(args.report.read_text()),args.report.with_suffix('.log').read_text(),records,frames,
        expected_frames=args.expected_frames,require_changed_content=args.require_changed_content)
    with args.output.open('x',newline='\n') as stream: json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(result,indent=2))
