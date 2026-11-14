"""Verify UWSM-managed GPU capture without accepting a normal launcher desktop."""
import re

from verify_hyprland_lifecycle_capture import require
from verify_omarchy_live_gpu_lifecycles import validate_content_frame


UNIT = 'wayland-wm@wddmprobe.desktop.service'
CGROUP = '/user.slice/user-1000.slice/user@1000.service/session.slice/' + UNIT


def session_metadata(guest, source, compositor_pid):
    manifest = source['manifest']
    require(manifest.get('uwsmManagedProbe') is True and
            manifest.get('normalOmarchySessionVerified') is False,
            'Missing scoped UWSM fixture or unsupported normal-session claim')
    require('factoryPromptDeferredInRam=true normalLauncher=false' in guest,
            'Missing disposable factory-prompt scope')
    primary = re.findall(
        r'^NORMAL_ROOT_UWSM_PRIMARY pid=(\d+) launcherPid=(\d+) exactExecutable=true '
        r'exactCgroup=true pinnedPidfd=true normalOmarchySession=false$', guest, re.M)
    require(len(primary) == 1 and int(primary[0][0]) == compositor_pid and
            int(primary[0][1]) > 0 and int(primary[0][1]) != compositor_pid,
            'UWSM compositor is not the root-selected service process')
    pinned = re.findall(
        r'^BROKER_COORDINATOR_PRIMARY pid=(\d+) rootChannel=true pinnedPidfd=true gpuProof=false$', guest, re.M)
    require(pinned == [str(compositor_pid)], 'Missing broker root pidfd admission')
    login = re.findall(
        r'^UWSM_COMPOSITOR_LOGIN pid=(\d+) uid=1000 sessionId=([A-Za-z0-9_-]{1,64}) '
        r'realPamSession=true gpuProof=false$', guest, re.M)
    require(len(login) == 1 and int(login[0][0]) == compositor_pid,
            'Missing PID-correlated ordinary PAM compositor')
    session_id = login[0][1]
    require(f"New session '{session_id}' of user 'omarchy' with class 'user' and type 'tty'." in guest,
            'Missing independent logind user session')
    running = re.findall(
        r'^NORMAL_ROOT_UWSM_STATE_BEGIN expectedPid=(\d+) phase=running\n(.*?)'
        r'^NORMAL_ROOT_UWSM_STATE_END phase=running$', guest, re.M | re.S)
    require(len(running) == 1 and int(running[0][0]) == compositor_pid,
            'Missing independent running-session snapshot')
    units = {}
    current = None
    for line in running[0][1].splitlines():
        field = re.fullmatch(r'(Id|ActiveState|SubState|MainPID|ControlGroup|Result|ExecMainStatus|NoNewPrivileges)=(.*)', line)
        if not field:
            continue  # Other authenticated actors may log between these lines.
        name, value = field.groups()
        if name == 'Id':
            require(value not in units, 'Duplicate session unit')
            current = units[value] = {}
        else:
            require(current is not None and name not in current, 'Orphan or duplicate unit property')
            current[name] = value
    require(set(units) == {UNIT, 'wayland-wm-env@wddmprobe.desktop.service', 'graphical-session.target'},
            'Wrong UWSM service snapshot')
    wm = units[UNIT]
    require(wm.get('ActiveState') == 'active' and wm.get('SubState') == 'running' and
            wm.get('MainPID') == str(compositor_pid) and wm.get('ControlGroup') == CGROUP and
            wm.get('NoNewPrivileges') == 'yes' and wm.get('Result') == 'success',
            'Compositor service was not active with the admitted identity')
    target = units['graphical-session.target']
    require(target.get('ActiveState') == 'active' and target.get('SubState') == 'active',
            'Graphical session target never became active')
    environment = units['wayland-wm-env@wddmprobe.desktop.service']
    require(environment.get('ActiveState') == 'active' and environment.get('Result') == 'success',
            'UWSM environment preparation did not remain active')
    return dict(uwsmSessionMetadataVerified=True, compositorPid=compositor_pid,
                compositorUnit=UNIT, compositorCgroup=CGROUP, pamSessionId=session_id,
                runningGraphicalTargetVerified=True, rootPidfdAdmissionVerified=True,
                normalLauncherVerified=False, normalOmarchySessionVerified=False,
                gpuProof=False)


def validate_uwsm_gpu_capture(report, guest, records, frames, source, wallpaper_reference):
    # Keep the published GPU/pixel/retirement auditor unchanged. UWSM metadata
    # cannot substitute for native submissions, all client identities, exact
    # 4K bytes or causal live-retirement receipts.
    capture = validate_content_frame(report, guest, records, frames, source, wallpaper_reference)
    metadata = session_metadata(guest, source, capture['compositorPid'])
    return dict(capture=capture, session=metadata, uwsmManagedGpuCaptureVerified=True,
                normalLauncherVerified=False, normalOmarchySessionVerified=False,
                fullDesktopAccepted=False, nearNativePerformanceVerified=False)
