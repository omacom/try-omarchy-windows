"""Audit real-root NVIDIA composition, retaining the separate normal-session/product gates."""
import argparse
import json
from pathlib import Path
from verify_hyprland_lifecycle_capture import require
from verify_hyprland_wayland_scene import validate as scene


SOURCE_SHA256='3954123192111ce2d28cafb9dd108e1ca14c288bfc2429ed899b0f36aeccf942'
NATIVE_SHA256='26aff97284df7d1d9ed6b1a6f9b985b5fa0daa452dd6f0ca7bf0a9824e106de0'
# Independently retained MSVC builds: the original real-root worker and the
# new worker used for a legacy-profile hardware regression. Never trust a
# report-supplied hash as its own provenance oracle.
KNOWN_NATIVE_SHA256=(NATIVE_SHA256,'277f720994b5287a07fb4c542eda868f67bdef38e36e4461badd27875f1904c1',
                     '0d8af5dc954826c32f1863faad62e9f77fd8621aae3436909b5602019374605b',
                     '6c5a1115d90c318d9a1dddce3be1507d084423aed339c72517136a763ccc1ba4',
                     'a0c95013376eee6b0688485e82100e763d3ce0c5d0c0e93a23e9b94daa7e40ed',
                     '6586c6e6583d45974048709bae24ae5c719b15cb9a107e4b22e5fe9b5eb4274e')
KNOWN_QEMU_SHA256=('9d23c23fdd29a5dbdb0a12fa5fa4672de88df17636395bb3d503b07a7fb9b77e',
                   'dcd422e6a7882af4ede4d6a79915dcfa6b344253b0b008cd6cebb55e62889111')


def validate(report,guest,records,frames,source,*,expected_frames):
    require(source.get('sourceImageSha256Before')==source.get('sourceImageSha256After')==SOURCE_SHA256 and
            source.get('sourceImageUnchanged') is True,'Wrong/changed real root image')
    manifest=source['manifest']
    require(manifest.get('sourceImageSha256')==SOURCE_SHA256 and manifest.get('ordinaryUid')==1000 and
            manifest.get('expectedFrames')==expected_frames and manifest.get('consoleGettyMasked') is True,
            'Wrong real-root test inputs')
    require(report.get('initramfsSha256')==manifest.get('initramfsSha256') and
            report.get('driverBridgeSha256')==source['nativeBuildReceipt'].get('bridgeSha256') and
            report.get('driverBridgeSha256') in KNOWN_NATIVE_SHA256 and
            report.get('kernelSha256')=='fb3e75cf32214fad38a22939f8bdafffa78bd7e0e5b2aa2afa89f0ebe02849ba' and
            report.get('qemuSha256') in KNOWN_QEMU_SHA256,'Wrong native/kernel/QEMU/image provenance')
    roots=[row for row in records if 'ownedQemuReadonlyRootAttached' in row]
    require(len(roots)==1 and roots[0]==dict(ownedQemuReadonlyRootAttached=True,fileNodeReadonly=True,
        rawNodeReadonly=True,sourceWriteSharingDenied=True),'Missing read-only owner policy')
    owners=[row for row in records if 'ownedQemuPid' in row]
    require(len(owners)==1 and owners[0].get('guestMemoryMiB')==3072 and type(owners[0].get('ownedQemuPid')) is int and
            owners[0]['ownedQemuPid']>0,'Wrong owned real-root VM')
    def one(text):require(guest.count(text+'\n')==1,'Missing/duplicate real-root receipt: '+text)
    for text in ('NORMAL_ROOT_BOOT_BEGIN graphicsRequested=true sourceWritesAllowed=false',
        'NORMAL_ROOT_SWITCH_ROOT sourceBlockReadonly=true overlayBytes=536870912 graphicsRequested=true',
        'NORMAL_ROOT_SERIAL_CONSOLE dedicatedToGraphics=true serialGettyMaskedInRam=true',
        'NORMAL_ROOT_GRAPHICS_BEGIN normalSystemdService=true normalUid=1000 diagnosticCompositor=true normalOmarchySession=false',
        'NORMAL_ROOT_GRAPHICS_STOP serviceResult=success exitCode=exited exitStatus=0'):
        one(text)
    require('NORMAL_ROOT_BOOT_FAILURE' not in guest and 'NORMAL_ROOT_GRAPHICS_CHILD_SIGNAL' not in guest,
            'Root bootstrap/renderer failed')
    require(type(manifest.get('payloadBytes')) is int and 0<manifest['payloadBytes']<=384*1024*1024,'Wrong payload bound')
    one(f"NORMAL_ROOT_GRAPHICS_PAYLOAD copiedBytes={manifest['payloadBytes']} ordinaryUid=1000 persistentWrites=false")
    require('reboot: Power down' in guest,'Missing normal OS poweroff')
    value=scene(report,guest,records,frames,expected_frames=expected_frames,
        require_changed_content=expected_frames>=300,normal_root=True)
    return dict(schema=1,realRootNvidiaCompositionVerified=True,ordinaryRenderUid=1000,supervisedSystemdServiceVerified=True,
        sourceImageUnchanged=True,readOnlySourcePolicyVerified=True,diagnosticConsoleIsolatedInRam=True,
        normalOmarchySessionVerified=False,applicationGpuAccelerationVerified=False,desktopVerified=False,
        normalLauncherIntegrationVerified=False,longRunStabilityVerified=False,compositor4kVerified=False,
        nearNativePerformanceVerified=False,scene=value)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report',type=Path,required=True)
    parser.add_argument('--expected-frames',type=int,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    records=[json.loads(line) for line in args.report.with_suffix('.host.log').read_text().splitlines() if line.startswith('{')]
    frames={f'compositor-copy-{i}.raw':(args.report.parent/f'compositor-copy-{i}.raw').read_bytes() for i in (1,2)}
    result=validate(json.loads(args.report.read_text()),args.report.with_suffix('.log').read_text(),records,frames,
        json.loads((args.report.parent/'source.json').read_text()),expected_frames=args.expected_frames)
    with args.output.open('x',newline='\n') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(result,indent=2))
