"""Audit the retained failed input run; never turn its cleanup into acceptance."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from virtio_trace import merge_session
from verify_real_sdl_desktop import pixel_audit


def audit(folder, manifest_path):
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    read = lambda name: json.loads((folder / name).read_bytes())
    source, report = read('source.json'), read('driver.json')
    assert sha(manifest_path) == '8739d00b40d5335e60d7be94d2e8a0d2df8226dd70758c6d9193193091208598'
    assert source['manifest'] == json.loads(manifest_path.read_bytes())
    assert report['driverBridgeSha256'] == 'fba526a5ec94f0774136f97cafd546a1ed2f4e099f54399cdc4b2a3c45e00916'
    assert report['qemuSha256'] == '5404287396eb03a181941f66f09afaee2bc5859a9b87830d64ea1e9eaa91a87c'
    assert report['initramfsSha256'] == 'c5bac7540d5450135a42f96f15ce6dd43abd93b0c4ef439bd4e6df187a9efca8'
    guest, trace = merge_session((folder / 'driver.log').read_bytes(),
                                 (folder / 'graphics-virtio.trace').read_bytes())
    assert guest.encode() == (folder / 'driver.combined-guest.log').read_bytes()
    guards = ('sourceImageUnchanged', 'nativeSourcesUnchanged', 'qemuArtifactUnchanged',
              'activeFrameProbeSourcesUnchanged', 'multiwindowSourcesUnchanged',
              'kernelAdmissionSourcesUnchanged', 'traceProbeSourcesUnchanged')
    assert all(source[k] is True for k in guards)
    assert source['sourceImageSha256Before'] == source['sourceImageSha256After'] == '3954123192111ce2d28cafb9dd108e1ca14c288bfc2429ed899b0f36aeccf942'
    assert source['installedNvidiaRuntimeBefore'] == source['installedNvidiaRuntimeAfter']
    assert source['installedNvidiaRuntimeAfter']['installedRuntimeMatchesSnapshot'] is True

    def hex_one(label):
        values = [json.loads(bytes.fromhex(m[1])) for m in
                  re.finditer(r'(?m)^' + re.escape(label) + r' ([0-9a-f]+)$', guest)]
        assert len(values) == 1, label
        return values[0]

    failure = hex_one('OMARCHY_BROWSER_COMPLETED_FAILURE_HEX')
    focus = hex_one('OMARCHY_BROWSER_FINAL_FOCUS_HEX')
    assert failure['accepted'] is False and failure['error'] == 'TimeoutError'
    prefix = 'Windows-to-SDL browser key/wheel observation incomplete: '
    assert failure['reason'].startswith(prefix)
    observed = json.loads(failure['reason'][len(prefix):])
    assert observed == dict(value='', keys=[], trustedWheels=0, maximumScroll=0,
                            currentScroll=0, documentHasFocus=True, inputFocused=True)
    assert failure['browserPid'] == focus['browserPid'] == focus['activeWindow']['pid']
    assert focus['activeWindow']['class'] == 'omarchy-gpu-browser'
    assert focus['documentHasFocus'] is focus['inputFocused'] is True
    assert 'HYPRLAND_INTERACTIVE_QUEUE_END frames=2406 minimum=1800 maximum=6000 failed=1 endAcknowledged=false limitReached=false' in guest
    assert 'HYPRLAND_FAILURE_SCENE_DAMAGE exit=0 originalWorkloadStillRejected=true' in guest
    assert 'HYPRLAND_HOST_QUEUE_END_ACK frame=2407 outputDrained=true' in guest
    assert 'HYPRLAND_FAILURE_OUTPUT_DRAIN done=true frame=2407 originalWorkloadStillRejected=true' in guest
    assert 'OMARCHY_FAILURE_APPLICATION_CLEANUP failed=0 originalWorkloadStillRejected=true' in guest
    assert 'OMARCHY_BROWSER_SDL_INPUT_RECEIVED ' not in guest

    rows = [json.loads(line) for line in (folder / 'driver.errors.log').read_text().splitlines() if line.strip()]
    ring = [r for r in rows if r.get('nativeCompositorRingFrame') is True]
    sdl = [r for r in rows if r.get('nativeSdlCompositorQueued') is True]
    assert [r['frame'] for r in ring] == list(range(1, 2408))
    assert [r['frame'] for r in sdl] == list(range(1, 2408))
    assert all(r['cpuUpload'] is False for r in ring + sdl)
    assert [r['frame'] for r in ring if r['cpuReadback']] == [1, 2407]
    assert [r['frame'] for r in sdl if r['audit']] == [1, 2407]
    assert all(r['ordinaryCpuFenceWait'] is False for r in sdl if not r['audit'])
    ends = [r for r in rows if r.get('nativeCompositorEndRetired') is True]
    assert len(ends) == 1 and ends[0]['frame'] == 2407
    assert ends[0]['beforeFinalGuestReply'] is ends[0]['beforeOwnedVmStop'] is True

    qemu, clean = report['ownedQemuControl'], report['disconnectCleanup']
    assert report['hostBridgeExit'] == qemu['qemuExit'] == 0
    assert qemu['ownedQemuExited'] is True and qemu['qemuForcedStop'] is False
    assert clean['driverCleanupVerified'] is True and clean['cleanupFailures'] == 0
    assert clean['completedCompositorFrames'] == 2407
    assert clean['completedNativeSubmissions'] == 5717
    assert clean['completedDeviceExecutionQueries'] == 5887
    assert all(v == 0 for k, v in clean.items() if k.startswith(('failed', 'live', 'pending', 'forced')) or k.endswith(('Timeouts', 'ReleasedAfterVmExit')))
    assert all(v == 0 for k, v in qemu.items() if k.startswith('live') or k.endswith('ReleasedAfterVmExit'))

    ui = read('host-ui-actions-v1.json')
    keys = [r['key'] for r in ui['actions'] if r['action'] in ('key', 'retryKeyAfterRefocus')]
    assert keys == ['o', 'o'] and ui['keyboardSequenceCompleted'] is False
    assert ui['scrollSequenceCompleted'] is False and ui['physicalHardwareVerified'] is False
    pixels = []
    for index, frame in enumerate((1, 2407), 1):
        window = read(f'relay-window-state-{frame:016d}.json')
        pixels.append(dict(frame=frame, **pixel_audit(
            (folder / f'compositor-copy-{index}.raw').read_bytes(),
            (folder / f'relay-source-{frame:016d}.rgba').read_bytes(),
            (folder / f'relay-window-{frame:016d}.rgba').read_bytes(),
            window['captureWidth'], window['captureHeight'])))

    files = ('source.json', 'driver.json', 'driver.errors.log', 'driver.log',
             'graphics-virtio.trace', 'driver.combined-guest.log', 'host-ui-actions-v1.json')
    return dict(schema=1, run=249, guestBuild=208, nativeBuild=111,
        workloadAccepted=False, ordinaryBrowserInputAccepted=False, dailyUseAccepted=False,
        continuedPastFormer1799Hold=True, failedWorkloadFrames=2406, actualDrainFrame=2407,
        boundedFailureDrainVerified=True, sourceAndWindowPixels=pixels,
        guestInputObservation=observed, ownedBrowserFocus=focus,
        returnedHostKeyCalls=keys, scrollSequenceVerified=False,
        nativeSubmissions=5717, deviceHealthQueries=5887,
        nativeFailures=0, deviceHealthFailures=0, hostExit=0, naturalVmExit=True,
        nativeObjectsAndApertureMappingsRetiredBeforeVmExit=True,
        officialInputChanged=False, performanceAccepted=False, traceProof=trace,
        sourceGuards={key: source[key] for key in guards},
        privateFiles={name: sha(folder / name) for name in files},
        guestManifestSha256=sha(manifest_path), auditorSha256=sha(Path(__file__)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory', type=Path)
    parser.add_argument('guest_manifest', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    value = audit(args.run_directory, args.guest_manifest)
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(value, output, indent=2); output.write('\n')
    print('Verified failure preservation, continuous GPU transport, complete boundary pixels and live cleanup; browser input remains rejected.')
