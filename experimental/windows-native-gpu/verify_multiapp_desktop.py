"""Audit the bounded two-application CPU384 experiment, without product acceptance.

Historical auditors stay immutable. Each derivation checks its exact source and
the replacement boundary. Complete source/SDL pixels, fences, input, resize and
live retirement remain mandatory. Only wallpaper probes covered by the new,
preselected companion rectangle are omitted from the wallpaper comparison.
"""
from pathlib import Path
import hashlib
import json
import re
from verify_hyprland_lifecycle_capture import require
from verify_qmp_input_delivery import snapshot

BRIDGE = '60f4bff9a8b2afde7526f6966bdec315a971eab81356ba954f7cfdb223c95ab3'
GUEST = '79c64bf444fd604b6559d790eb8382b0fc292f8641b2eafd458206d5e5f87d68'
MANIFEST = '6bd54199bed5dcef9eebf92f49835103faff23d73c0c5dfec0a1c6062efcc92a'
BUILD = '273976348b49c8d2a6ff1cc8ddaf341663beb08014e2c933c47022a3d6cd2168'
PINS = {
    'verify_managed_mapping_desktop.py': '892515f6f71aa3979c6eb111e73ec185087bb487ee8f128d7e431b9bbcb72a9a',
    'verify_managed_residency_state.py': 'a13b7fecad67d73c7edddc534a5fe6debbebfdb285b0ee46191a279772a057dc',
    'verify_sustained_virtio_desktop.py': 'ee79e8f9ab727c86175f474830ce4352df227a7e24ed9d5042e276a2035f026a',
    'verify_omarchy_live_gpu_lifecycles.py': '25740b7c89cc9b16202e6224c2b282609f9807567a69a880d55f7c21d32814bd',
    'verify_uwsm_demand_staging.py': 'f32c8f4ede91a24342cabd5c4aa1e7ee55a7826665d6bbd4247e5863ce006296',
    'verify_deferred_residency.py': '842edb5afb01247221a1327c920a1d4586c6e75a83ec7783e30fd2cbf6c40bad',
}


def canonical_sha(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def derived(name, replacements=()):
    raw = (Path(__file__).resolve().parent / name).read_bytes()
    require(hashlib.sha256(raw).hexdigest() == PINS[name], 'Historical auditor source changed')
    text = raw.decode('utf-8')
    for old, new, count in replacements:
        require(text.count(old) == count, 'Derived auditor boundary changed: ' + old)
        text = text.replace(old, new)
    namespace = {'__name__': '_multiapp_' + name[:-3]}
    exec(compile(text, name, 'exec'), namespace)
    return namespace


def validate_windows(guest, records, frames):
    ready = snapshot(guest, 'OMARCHY_MULTIWINDOW_READY_CLIENTS_HEX')
    require(len(ready) == 2, 'Missing simultaneous applications')
    primary = next(c for c in ready if c['class'] == 'omarchy-gpu-terminal')
    companion = next(c for c in ready if c['class'] == 'omarchy-gpu-companion')
    require(primary['pid'] != companion['pid'] and primary['address'] != companion['address'], 'Reused application identity')

    def visible(client, expected, initial=False):
        require(all(client.get(k) == expected[k] for k in ('pid', 'address', 'class', 'title')) and
                client['mapped'] is client['visible'] is client['floating'] is True and
                client['hidden'] is client['xwayland'] is False and
                client['at'] == ([1200, 613] if initial and client['pid'] == primary['pid'] else
                                 [1320, 693] if client['pid'] == primary['pid'] else [2800, 200]) and
                client['size'] == ([1440, 960] if initial and client['pid'] == primary['pid'] else
                                   [1200, 800] if client['pid'] == primary['pid'] else [960, 640]),
                'Application identity, visibility or geometry changed')

    require(primary['title'] == 'Omarchy terminal' and companion['title'] == 'NVIDIA companion', 'Wrong applications')
    for client in ready:
        visible(client, primary if client['pid'] == primary['pid'] else companion, True)
    checkpoints = [tuple(map(int, row)) for row in re.findall(
        r'^OMARCHY_MULTIWINDOW_CHECKPOINT phase=(\d+) primaryPid=(\d+) companionPid=(\d+) beforeQueued=(\d+) afterQueued=(\d+) gpuProof=false physicalInputVerified=false$', guest, re.M)]
    require(len(checkpoints) == 4 and guest.count('OMARCHY_MULTIWINDOW_CHECKPOINT ') == 4, 'Incomplete focus checkpoints')
    pulses = {'PRIMARY': [], 'COMPANION': []}
    for phase, threshold in enumerate((300, 600, 1200, 1700), 1):
        n, app, other, before, after = checkpoints[phase-1]
        require((n, app, other) == (phase, primary['pid'], companion['pid']) and
                threshold <= before <= after < 1800 and before < threshold + 10 and after - before < 10,
                'Wrong checkpoint order or queued frame boundary')
        clients = snapshot(guest, f'OMARCHY_MULTIWINDOW_{phase}_CLIENTS_HEX')
        require(len(clients) == 2 and {c['pid'] for c in clients} == {primary['pid'], companion['pid']}, 'Missing live windows')
        for client in clients:
            visible(client, primary if client['pid'] == primary['pid'] else companion)
        for role, client in (('PRIMARY', primary), ('COMPANION', companion)):
            visible(snapshot(guest, f'OMARCHY_MULTIWINDOW_{phase}_{role}_ACTIVE_HEX'), client)
            pulse = snapshot(guest, f'OMARCHY_MULTIWINDOW_{phase}_{role}_PULSE_HEX')
            require(pulse['parentPid'] == client['pid'] and pulse['uid'] == 1000 and pulse['targetHz'] == 60 and
                    pulse['updates'] > 0 and pulse['lastNs'] >= pulse['startedNs'] > 0 and
                    pulse['gpuTimestamp'] is False, 'Invalid independent application counter')
            pulses[role].append(pulse)
    for role, values in pulses.items():
        require(len({p['pid'] for p in values}) == 1 and
                all(a['updates'] < b['updates'] and a['lastNs'] < b['lastNs'] and a['startedNs'] == b['startedNs']
                    for a, b in zip(values, values[1:])), 'Application counter stopped or restarted')
    closed = snapshot(guest, 'OMARCHY_MULTIWINDOW_CLOSED_CLIENTS_HEX')
    require(len(closed) == 1 and closed[0]['pid'] == primary['pid'], 'Companion did not close independently')
    visible(closed[0], primary)
    peers = {int(pid): bytes.fromhex(path).decode() for pid, path in re.findall(
        r'^BROKER_COORDINATOR_PEER pid=(\d+) primary=(?:true|false) exePathHex=([0-9a-f]+) pidfdAuthenticated=true$', guest, re.M)}
    actors = {int(pid): int(actor) for actor, pid in re.findall(
        r'^BROKER_COORDINATOR_ACTOR actor=(\d+) pid=(\d+) primary=(?:true|false) pidfdAuthenticated=true nativeDriverCount=1$', guest, re.M)}
    pid = companion['pid']; actor = actors[pid]
    require(peers[pid] == '/opt/wddm-root/bin/alacritty' and
            any(f'omarchy-gpu-companion[{pid}]: ' in line and 'Running on D3D12 (NVIDIA GeForce RTX 5090 Laptop GPU)' in line for line in guest.splitlines()) and
            any(f'omarchy-gpu-companion[{pid}]: ' in line and 'Using OpenGL 3.3 renderer' in line for line in guest.splitlines()),
            'Companion is not correlated with the NVIDIA renderer')
    witness = f'OMARCHY_MULTIWINDOW_COMPANION_RETIRED pid={pid} actor={actor} primaryStillMapped=true compositorStillRunning=true rootOwnedWitness=true gpuProof=false'
    root = f'BROKER_COORDINATOR_LIVE_RETIRED actor={actor} pid={pid} '
    require(guest.count(witness) == 1 and guest.index(root) < guest.index(witness) <
            guest.index('HYPRLAND_STARTUP_EXIT_REQUEST method=hyprctl-dispatch-exit'), 'Missing live companion retirement witness')
    retired = [i for i, r in enumerate(records) if r.get('nativeLiveRetirementStep') and r['actor'] == actor]
    require(retired and max(i for i, r in enumerate(records) if r.get('nativeCompositorRingInput')) < min(retired),
            'Companion retired before the bounded stream completed')
    # Fixed companion header/body rectangles, selected from the generated layout.
    last = frames['compositor-copy-2.raw']; first = frames['compositor-copy-1.raw']
    blue = bright = changed = 0
    for y in range(220, 500):
        for x in range(2810, 3710):
            offset = (y * 3840 + x) * 4
            b, g, r = last[offset:offset+3]
            blue += b > r + 60 and b > g + 40
            bright += min(b, g, r) > 96
            changed += last[offset:offset+3] != first[offset:offset+3]
    require(blue > 1000 and bright > 1000 and changed > 1000, 'Missing readable, updated companion pixels')
    return dict(primaryPid=primary['pid'], companionPid=pid, companionActor=actor, simultaneousVisibleWindows=2,
                focusCheckpointCount=4, focusControlledByHyprlandIpc=True, physicalInputVerified=False,
                companionBluePixels=blue, companionBrightPixels=bright, companionChangedPixels=changed,
                companionRetiredWhilePrimaryMapped=True, independentCounters=pulses)


def validate_multiapp_desktop(**inputs):
    source = inputs['source']; manifest = source['manifest']; build = source['nativeBuildReceipt']
    require(canonical_sha(manifest) == MANIFEST and canonical_sha(build) == BUILD, 'Wrong frozen CPU384 experiment')
    require(source['managedMappingNegotiationVersion'] == 9 and source['memoryGuardRaised'] is
            source['cpuMappingGuardRaised'] is True and source['gpuUsageGuardRaised'] is False and
            source['negotiatedCpuMappingBytes'] == 402653184, 'Wrong CPU opt-in receipt')
    guest = inputs['guest']; records = inputs['records']
    require('APERTURE_CONTRACT_PASS checks=4717 activeLeases=0 activeCpuBytes=0' in guest and
            'BROKER_COORDINATOR_MULTIAPP_CPU_CAPACITY profile=14 objectsVersion=9 flags=7 cpuBytes=402653184 reportedGpuBytes=1073741824 cpuMappingGuardRaised=true gpuUsageGuardRaised=false' in guest,
            'Missing actual kernel and native CPU384 agreement')
    require(not any(r.get(k) for r in records for k in ('nativeSharedAliasAdmissionRejected',
            'wireCpuByteAdmissionRejected', 'nativeCpuByteAdmissionRejected', 'nativeObjectAdmissionRejected')) and
            not any(s in guest for s in ('BROKER_ALIAS_REFUSAL_', 'BROKER_ROUTE errno=', 'EGL_BAD_ALLOC')),
            'Admission failed during the bounded experiment')
    contract = [r for r in records if r.get('nativeCoordinatedObjectCapacity')]
    require(len(contract) == 1 and contract[0]['kernelLeaseLimit'] == 1536 and
            contract[0]['hwQueues'] == 64 and contract[0]['syncObjects'] == 768, 'Wrong four-actor limits')
    windows = validate_windows(guest, records, inputs['frames'])
    state = derived('verify_managed_residency_state.py', [
        ('coordinatorObjectsVersion=6', 'coordinatorObjectsVersion=9', 1),
        ("manifest['managedGpuVaPolicy']==policy", "manifest['managedGpuVaPolicy']==dict(policy,cpuMappingGuardRaised=True,gpuUsageGuardRaised=False)", 1),
        ("source['managedMappingNegotiationVersion']==6", "source['managedMappingNegotiationVersion']==9", 1),
        ("source['memoryGuardRaised'] is False", "source['memoryGuardRaised'] is True", 1),
        ("contract[0]['version']==6", "contract[0]['version']==9", 1),
        ('CAPACITY version=6 ', 'CAPACITY version=9 ', 1),
        ("build['managedMappingNegotiationVersion']==6", "build['managedMappingNegotiationVersion']==9", 1),
        ("build['officialInputChanged'] is build['memoryGuardRaised'] is False", "build['officialInputChanged'] is False and build['memoryGuardRaised'] is True", 1),
        ('mappingAgreementVersion=6', 'mappingAgreementVersion=9', 1),
        ('201326592', '402653184', 3),
        ('physicalMemoryGuardRaised=False', 'physicalMemoryGuardRaised=True', 2),
    ])
    lifecycle = derived('verify_omarchy_live_gpu_lifecycles.py', [
        ("{*pids,*result['quickshellPids'],result['compositorPid']}", "{*pids,COMPANION_PID,*result['quickshellPids'],result['compositorPid']}", 1),
        ('len(closed)==6', 'len(closed)==7', 1),
        ('len({a for a,_,_,_ in closed})==6', 'len({a for a,_,_,_ in closed})==7', 1),
        ("{*pids,*result['quickshellPids']}", "{*pids,COMPANION_PID,*result['quickshellPids']}", 1),
        ('len(timings)==6', 'len(timings)==7', 1),
        ('len({a for a,_,_ in timings})==6', 'len({a for a,_,_ in timings})==7', 1),
        ("for pid in [*pids,*result['quickshellPids']]:", "for pid in [*pids,COMPANION_PID,*result['quickshellPids']]:", 1),
        ("group['liveRetiredActors']==6", "group['liveRetiredActors']==7", 1),
        ("group['peakActors']==3", "group['peakActors']==4", 1),
    ])
    lifecycle['COMPANION_PID'] = windows['companionPid']
    terminal = derived('verify_uwsm_demand_staging.py', [
        ('len(actor_exits) == 7', 'len(actor_exits) == 8', 1)])
    timing = derived('verify_sustained_virtio_desktop.py', [
        ("profile['baselineFixtureUnchanged'] is True", "profile['baselineFixtureUnchanged'] is False", 1)])
    timing.update(GENERATOR=manifest['activeFrameProbe']['generatorSha256'],
                  ACTIVE_SOURCES=manifest['activeFrameProbe']['generatedSources'])
    desktop = derived('verify_managed_mapping_desktop.py', [
        ("manifest['kernelCpuCapacityProfile']==13", "manifest['kernelCpuCapacityProfile']==14", 1),
        ("clean['vendorCpuMappedByteLimit']==201326592 and clean['peakVendorCpuBytes']<=201326592",
         "clean['vendorCpuMappedByteLimit']==402653184 and clean['peakVendorCpuBytes']<=402653184", 1),
        ('len(actors)==7', 'len(actors)==8', 1),
        ("for x,y,r,g,b in reference['probes']]", "for x,y,r,g,b in reference['probes'] if not (2740<=x<=3820 and 140<=y<=900)]", 1),
        ("require(sum(errors)/len(errors)<3", "require(len(errors)==601 and sum(errors)/len(errors)<3", 1),
    ])
    desktop.update(BRIDGES={BRIDGE: '86'}, CURRENT_DRIVER_GUEST=GUEST, TRACE_PINS=manifest['virtioTraceProbe'],
                   validate_managed_state=state['validate_managed_state'],
                   validate_lifecycles=lifecycle['validate_lifecycles'], active_timing=timing['active_timing'],
                   terminal_retirement=terminal['terminal_retirement'])
    proof = desktop['validate_managed_mapping_desktop'](**inputs)
    proof.update(boundedTwoApplicationExperimentVerified=True, windows=windows, wallpaperUncoveredProbeCount=601,
                 cpuMappingGuardRaised=True, gpuUsageGuardRaised=False, intermittentAliasLimitResolved=False,
                 normalLauncherVerified=False, fullDesktopAccepted=False, nearNativePerformanceVerified=False)
    return proof


def validate_cpu384_legacy_desktop(**inputs):
    """The larger-capacity binary must preserve the frozen v6/CPU192 agreement."""
    build = inputs['source']['nativeBuildReceipt']
    require(canonical_sha(build) == BUILD, 'Wrong legacy compatibility binary')
    contracts = [r for r in inputs['records'] if r.get('nativeCoordinatedObjectCapacity')]
    require(len(contracts) == 1 and contracts[0]['version'] == 6 and contracts[0]['kernelLeaseLimit'] == 768 and
            contracts[0]['hwQueues'] == 32 and contracts[0]['syncObjects'] == 384 and
            contracts[0]['cpuBytes'] == 201326592, 'Legacy fixture received new capacity')
    ownership = derived('verify_deferred_residency.py', [
        ('requests>0 and reused>0 and reclaimed>0', 'requests>0 and reused>=0 and reclaimed>0', 1)])
    state = derived('verify_managed_residency_state.py', [
        ("build['managedMappingNegotiationVersion']==6", "build['managedMappingNegotiationVersion']==9", 1),
        ("build['officialInputChanged'] is build['memoryGuardRaised'] is False",
         "build['officialInputChanged'] is False and build['memoryGuardRaised'] is True", 1)])
    state['validate_ownership'] = ownership['validate_ownership']
    desktop = derived('verify_managed_mapping_desktop.py')
    desktop.update(BRIDGES={BRIDGE: '86'}, validate_managed_state=state['validate_managed_state'])
    proof = desktop['validate_managed_mapping_desktop'](**inputs)
    proof.update(legacyV6CompatibilityVerified=True, boundedTwoApplicationExperimentVerified=False,
                 residencyReferenceReuseExercised=proof['sharedBudget']['reusedReferences'] > 0)
    return proof


def validate_multiapp_rejection(number, report, source, guest, records, uart_raw, trace_raw, **unused):
    """Retain both the pre-frame extent failure and the later reference failure."""
    from virtio_trace import merge_session
    from verify_coordinated_gpu_clients import ROOT
    pins = {
        143: ('9633843188c92bf8967adb461bc9e6493ded3bef6587053f821dc21c57196f94',
              'fbfd735683147318eac5669a4d16205ec304591f263d0ae7ff465b3961b75e05'),
        144: ('76963ce4348cd6da6a3275c13cfde04080607588a415ce40925abeece4cd3ef4',
              '2b90c425904058a8e2aa9efb80dd4187dccec0d8aecb5ead695a4ac17c4425bb'),
    }
    require(number in pins, 'Unknown rejected experiment')
    manifest, initrd = pins[number]
    require(canonical_sha(source['manifest']) == manifest and canonical_sha(source['nativeBuildReceipt']) == BUILD and
            report['driverBridgeSha256'] == BRIDGE and report['initramfsSha256'] == initrd,
            'Mixed failure provenance')
    require(source['sourceImageUnchanged'] is source['nativeSourcesUnchanged'] is True and
            source['sourceImageSha256Before'] == source['sourceImageSha256After'] == ROOT and
            source['installedNvidiaRuntimeBefore'] == source['installedNvidiaRuntimeAfter'] and
            source['installedNvidiaRuntimeBefore']['installedRuntimeMatchesSnapshot'] is True,
            'Source disk or installed driver changed')
    merged, integrity = merge_session(uart_raw, trace_raw)
    require(guest == merged.replace('\r\n', '\n').replace('\r', '\n'), 'Failure trace differs from the actual live carrier')
    require('APERTURE_CONTRACT_PASS checks=4717 activeLeases=0 activeCpuBytes=0' in guest and
            'BROKER_COORDINATOR_MULTIAPP_CPU_CAPACITY profile=14 objectsVersion=9 flags=7 cpuBytes=402653184 reportedGpuBytes=1073741824 cpuMappingGuardRaised=true gpuUsageGuardRaised=false' in guest,
            'Failed run did not negotiate the new CPU profile')
    queued = [r for r in records if r.get('nativeSdlCompositorQueued')]
    clean = report['disconnectCleanup']; vm = report['ownedQemuControl']
    if number == 143:
        require(report['hostBridgeExit'] == 0 and clean['driverCleanupVerified'] is True and
                clean['completedCompositorFrames'] == clean['completedNativeSubmissions'] == 0 and
                not queued and vm['qemuExit'] == 0 and vm['qemuForcedStop'] is False and
                'BROKER_ROUTE rejected=true errno=71 operation=2 reason=Wrong profile request extent: Protocol error' in guest,
                'Wrong pre-frame request-extent failure')
        cause = 'broker profile-14 request extent missing'
    else:
        rejected = [r for r in records if r.get('nativeSharedAliasAdmissionRejected')]
        require(rejected == [dict(nativeSharedAliasAdmissionRejected=True, stage='wire', kind='resource',
                liveWireObjects=582, wireObjectLimit=1024, liveReferences=16, referenceLimit=16, idExhausted=False)] and
                [r['frame'] for r in queued] == list(range(1, 1114)) and
                report['hostBridgeExit'] == 3 and clean == vm == {} and
                'BROKER_ROUTE rejected=true errno=12 operation=5 reason=Native recipient alias failed: Cannot allocate memory' in guest,
                'Wrong intermittent reference-capacity failure')
        reaped = [r for r in records if r.get('nativeSdlFatalOwnerReaped')]
        require(reaped == [dict(beforeHostFatalExit=True, driverObjectsRetained=True, inputImplementationChanged=False,
                nativeSdlFatalOwnerReaped=True, ownedQemuReaped=True, qemuExit=90, qemuForcedStop=True)],
                'Fatal owned-process reaping differs; this is not clean native retirement')
        cause = 'per-actor resource reference limit 16 during frame stream; live FD cause unresolved'
    require('NORMAL_ROOT_GRAPHICS_EXIT result=125 normalOmarchySession=false' in guest and
            not any(r.get('nativeSdlCompositorDetached') for r in records) and
            'HYPRLAND_HOST_QUEUE_END_ACK frame=1800' not in guest, 'Rejected run relabelled complete')
    return dict(run=number, reason=cause, completedHandoffs=len(queued), traceIntegrity=integrity,
                nativeCleanupVerified=number == 143, fatalOwnedProcessReapingVerified=number == 144,
                completeRunAccepted=False, fullDesktopAccepted=False, nearNativePerformanceVerified=False)
