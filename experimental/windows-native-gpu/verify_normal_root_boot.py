"""Verify a read-only normal-image boot prerequisite, never GPU or desktop acceptance."""
import hashlib
import json
from pathlib import PureWindowsPath
import re
from verify_hyprland_lifecycle_capture import require


def validate(report,console,*,expected_source_sha256,expected_source_path):
    require(type(expected_source_sha256) is str and re.fullmatch('[0-9a-f]{64}',expected_source_sha256) is not None,'Invalid independent image digest')
    require(type(console) is bytes and hashlib.sha256(console).hexdigest()==report.get('consoleSha256'),'Console digest mismatch')
    require(report.get('normalRootBootVerified') is True,'Host boot check rejected')
    require(type(report.get('qemuExit')) is int and report['qemuExit']==0 and report.get('forcedStop') is False,'VM did not power off normally')
    require(report.get('sourceSha256Before')==report.get('sourceSha256After')==expected_source_sha256 and
            report.get('sourceUnchanged') is True,'Source image changed or wrong input')
    require(report.get('qemuSha256')=='9d23c23fdd29a5dbdb0a12fa5fa4672de88df17636395bb3d503b07a7fb9b77e' and
            report.get('kernelSha256')=='fb3e75cf32214fad38a22939f8bdafffa78bd7e0e5b2aa2afa89f0ebe02849ba','Wrong pinned QEMU/kernel')
    require(report.get('graphicsVerified') is False and report.get('desktopVerified') is False,'Unsupported graphics/desktop claim')
    require(report.get('readonlyFileNode') is True and report.get('readonlyRawNode') is True and
            report.get('temporaryOverlayBytes')==536870912,'Wrong root policy')
    args=report.get('arguments')
    require(type(args) is list and all(type(value) is str for value in args),'Invalid recorded QEMU command')
    require(args.count('-blockdev')==2 and args.count('-device')==1 and args[-2:]==['-device','virtio-blk-pci,drive=base-root'] and
            not any(value in args for value in ('-drive','-hda','-hdb','-hdc','-hdd','-cdrom','-readconfig','-netdev','-nic')),
            'Unexpected/missing disk or device')
    try:nodes=[json.loads(args[index+1]) for index,value in enumerate(args) if value=='-blockdev']
    except (ValueError,IndexError) as error:raise ValueError('Malformed block node') from error
    require(all(type(node) is dict for node in nodes),'Invalid block node type')
    require(nodes[0].keys()=={'driver','filename','node-name','read-only'} and nodes[0]['driver']=='file' and
            nodes[0]['node-name']=='base-file' and nodes[0]['read-only'] is True and
            type(nodes[0]['filename']) is str and
            PureWindowsPath(nodes[0]['filename'])==PureWindowsPath(expected_source_path),'Base file not the independently selected read-only input')
    require(nodes[1]=={'driver':'raw','file':'base-file','node-name':'base-root','read-only':True} and
            nodes[1]['read-only'] is True,'Raw node is writable or wrong backing')
    text=console.decode('utf-8')
    begin='NORMAL_ROOT_BOOT_BEGIN graphics=false sourceWritesAllowed=false'
    switched='NORMAL_ROOT_SWITCH_ROOT sourceBlockReadonly=true overlayBytes=536870912 graphics=false'
    ready='NORMAL_ROOT_OS_READY sourceBlockReadonly=true rootOverlay=true normalSystemd=true graphics=false desktop=false'
    require(all(text.count(marker)==1 for marker in (begin,switched,ready)) and 'NORMAL_ROOT_BOOT_FAILURE' not in text,
            'Missing/duplicate/failed boot receipt')
    require(text.index(begin)<text.index(switched)<text.index(ready)<text.index('reboot: Power down'),'Incorrect boot/shutdown order')
    require('systemd 262 (262-1-arch)' in text and re.search(r'\]: 4\.0\.4\r?\n',text) is not None,'Unexpected normal OS version')
    boot_ids=re.findall(r'bootid=([0-9a-f]{32});pid=1;',text)
    require(len(boot_ids)==1,'Missing/duplicate normal systemd boot identity')
    return dict(schema=1,normalRootBootVerified=True,sourceImageUnchanged=True,sourceBlockReadonlyVerified=True,
        temporaryRamRootVerified=True,normalSystemdVerified=True,bootId=boot_ids[0],
        normalGraphicsVerified=False,fullOmarchySceneVerified=False,desktopVerified=False,
        normalLauncherIntegrationVerified=False,multiGpuClientVerified=False,nearNativePerformanceVerified=False)
