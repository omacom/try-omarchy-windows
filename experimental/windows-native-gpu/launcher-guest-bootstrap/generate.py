"""Add named graphics-device discovery to the journal-only bounded guest probe.

This remains a diagnostic session. Normal user/account bootstrap is not supplied.
"""
from pathlib import Path
import hashlib
import importlib.util
import json


def generate(gpu, output, *, broker_session=False):
    if type(broker_session) is not bool:
        raise ValueError("Broker session selection must be an explicit boolean")
    gpu, output = Path(gpu), Path(output)
    logging = gpu/'launcher-guest-logging/generate.py'
    spec = importlib.util.spec_from_file_location('launcher_guest_logging', logging)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    parent = module.generate(gpu, output)
    target = output/'normal_root_graphics_service.c'
    original = target.read_text(encoding='utf-8')
    before = '    const char* port="/dev/vport0p1";'
    after = '    char port[128]={0};'
    wait = ('    for(unsigned tick=0;tick<200 && access(port,F_OK);++tick) usleep(50000);\n'
            '    if(chmod(port,0600)) { perror("native port permissions");return 125; }')
    discover = '''    int port_ready=0;
    for(unsigned tick=0;tick<200;++tick) {
        if(!wddm_graphics_port_resolve("/sys/class/virtio-ports","/dev",port,sizeof port)) { port_ready=1;break; }
        if(errno!=ENOENT && errno!=ENODEV && errno!=EAGAIN) break;
        usleep(50000);
    }
    if(!port_ready) { perror("named native graphics port");return 125; }
    if(chmod(port,0600)) { perror("native port permissions");return 125; }
    printf("NAMED_NATIVE_GRAPHICS_PORT path=%s name=org.7wdev.wddm characterIdentityVerified=true\\n",port);'''
    include = '#include "guest_session_process.h"'
    expanded = include+'\n#include "guest_graphics_port.h"\n#include "guest_progress_snapshot.h"'
    admission = '"-o","short-monotonic","--grep","WDDM_APERTURE_ADMISSION_REJECTED"'
    sync_admission = '"-o","short-monotonic","--grep","WDDM_APERTURE_ADMISSION_REJECTED|WDDM_SYNC_(ADMISSION_REJECTED|RECLAIM_WAIT|RECLAIM_BATCH)"'
    live_tail = '"--no-pager","-f","-n","100",(char*)NULL);_exit(127);'
    filtered_tail = ('"--no-pager","-f","-n","100","--grep",'
        '"^(OMARCHY_|HYPRLAND_|LINUX_BRIDGE (vendor=|nvidiaUmdPresentDuringPrivateQuery=|openGlProfileNegotiated=))|'
        'OpenGL VENDOR|Created QRhi|Running on D3D12|Using OpenGL 3.3 renderer|'
        '[Ee][Rr][Rr][Oo][Rr]|[Ff][Aa][Ii][Ll]|[Cc][Rr][Aa][Ss][Hh]|[Ww][Aa][Rr][Nn]|GL_OUT_OF_MEMORY|EGL_BAD_",'
        '(char*)NULL);_exit(127);')
    cleanup = 'cleanup:\n    if(uwsm && compositor_pid>0) {'
    outcome = '''cleanup:
    if(shell_trace>0) {
        kill(shell_trace,SIGTERM);waitpid(shell_trace,NULL,0);shell_trace=-1;
    }
    if(uwsm) {
        if(wddm_progress_snapshot_report(STDOUT_FILENO,"/tmp/omarchy-active-terminal-pulse.json",(uid_t)uid)) perror("producer progress output");
        char journal_pid[40];
        const int journal_pid_bytes=snprintf(journal_pid,sizeof journal_pid,"_PID=%ld",(long)compositor_pid);
        const int journal_pid_valid=compositor_pid>0 && journal_pid_bytes>0 && (size_t)journal_pid_bytes<sizeof journal_pid;
        {
            puts("NAMED_GRAPHICS_SESSION_OUTCOME_BEGIN normalOmarchySession=false");
            uwsm_inspect(uid,gid,0);
            fflush(stdout);
            const pid_t journal=journal_pid_valid ? fork() : -1;
            if(!journal) {
                if(setenv("SYSTEMD_COLORS","0",1) || setenv("SYSTEMD_URLIFY","0",1)) _exit(127);
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","--all","-n","160","-o","json","--output-fields=_PID,_UID,_SYSTEMD_USER_UNIT,_SYSTEMD_UNIT,_BOOT_ID,SYSLOG_IDENTIFIER,_TRANSPORT,MESSAGE","--grep","HYPRLAND_(STARTUP|HOST_QUEUE_(END|FAILURE)|FAILURE_OUTPUT_DRAIN|ACTIVE_WORKLOAD|SCENE_FAILURE|CONTROL_FAILURE)|OMARCHY_NATIVE_INPUT_[A-Z0-9_]+([ \\\\n]|$)",journal_pid,"_UID=1000","_SYSTEMD_USER_UNIT=wayland-wm@wddmprobe.desktop.service",(char*)NULL);_exit(127);
            }
            int status=0,joined=0;
            if(journal>0) {
                alarm(5);joined=waitpid(journal,&status,0)==journal;
                if(!joined) { kill(journal,SIGKILL);waitpid(journal,&status,0); }
                alarm(0);
            }
            printf("NAMED_GRAPHICS_JOURNAL_CAPTURE joined=%s exited=%s code=%d orderEstablished=false\\n",
                joined?"true":"false",journal>0 && WIFEXITED(status)?"true":"false",journal>0 && WIFEXITED(status)?WEXITSTATUS(status):-1);
            puts("NAMED_GRAPHICS_SESSION_OUTCOME_END exitStatusAloneIsNotGraphicsAcceptance=true");
        }
        {
            puts("NAMED_GRAPHICS_APPLICATION_OUTCOME_BEGIN normalOmarchySession=false");
            fflush(stdout);
            const pid_t journal=journal_pid_valid ? fork() : -1;
            if(!journal) {
                if(setenv("SYSTEMD_COLORS","0",1) || setenv("SYSTEMD_URLIFY","0",1)) _exit(127);
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","--all","-n","160","-o","json","--output-fields=_PID,_UID,_SYSTEMD_USER_UNIT,_SYSTEMD_UNIT,_BOOT_ID,SYSLOG_IDENTIFIER,_TRANSPORT,MESSAGE","--grep","OMARCHY_(BROWSER_(CDP_RECEIPT_HEX|[1-4]_(BROWSER|TERMINAL)_ACTIVE_HEX|INTERACTION_(PROOF_HEX|ACK)|RESIZE_(ACTIVE_HEX|PROOF_HEX|ACK)|LAYOUT_(ACTIVE_HEX|READY)|READY_CLIENTS_HEX|WINDOW_READY|CHECKPOINT(_CLIENTS_HEX)?|CLOSED_CLIENTS_HEX|GPU_RETIRED|FAILURE|COMPLETED_FAILURE_HEX|FINAL_DIAGNOSTICS_HEX|INPUT_ARMED)|GTK_(RENDERER_PROOF_HEX|READY_ACTIVE_HEX|APPLICATION_(READY|RETIRED)|CHECKPOINT(_CLIENTS_HEX)?|CLOSED_CLIENTS_HEX|FAILURE)|GPU_(APPLICATION_(READY_CLIENTS_HEX|CLIENTS_HEX|CLOSED|CONTENT_READY|WAIT)|LIVE_(WARMUP_CLIENTS_HEX|WARMUP_READY|RETIREMENT_OBSERVED)|RESIZE(_[1-3]_(BEFORE|AFTER)_ACTIVE_HEX)?)|DESKTOP_(LAYERS_HEX|COORDINATOR_WAIT|PROBE_CAPTURE)|RELAUNCH_(BEGIN|RETIRED|READY)|MULTIWINDOW_[A-Z0-9_]+|FAILURE_APPLICATION_CLEANUP)([ \\\\n]|$)",journal_pid,"_UID=1000","_SYSTEMD_USER_UNIT=wayland-wm@wddmprobe.desktop.service",(char*)NULL);_exit(127);
            }
            int status=0,joined=0;
            if(journal>0) {
                alarm(5);joined=waitpid(journal,&status,0)==journal;
                if(!joined) { kill(journal,SIGKILL);waitpid(journal,&status,0); }
                alarm(0);
            }
            printf("NAMED_GRAPHICS_APPLICATION_JOURNAL_CAPTURE joined=%s exited=%s code=%d orderEstablished=false\\n",
                joined?"true":"false",journal>0 && WIFEXITED(status)?"true":"false",journal>0 && WIFEXITED(status)?WEXITSTATUS(status):-1);
            puts("NAMED_GRAPHICS_APPLICATION_OUTCOME_END exitStatusAloneIsNotGraphicsAcceptance=true");
        }
        {
            puts("NAMED_GRAPHICS_BROWSER_OUTCOME_BEGIN normalOmarchySession=false");
            fflush(stdout);
            const pid_t journal=fork();
            if(!journal) {
                if(setenv("SYSTEMD_COLORS","0",1) || setenv("SYSTEMD_URLIFY","0",1)) _exit(127);
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","--all","-n","96","-o","json","--output-fields=_PID,_UID,_SYSTEMD_USER_UNIT,_SYSTEMD_UNIT,_BOOT_ID,SYSLOG_IDENTIFIER,_TRANSPORT,MESSAGE","--grep","OMARCHY_BROWSER_(VERSION|PROCESSES|GPU_INFO|GPU_THREADS_SANDBOXED|WEBGL|SHADER_IMAGE_PNG|COMPOSITING_STATUS|PAGE|PAGE_OBSERVED|DOCUMENT_OBSERVED|KEYRING_READY|INTERACTION_READY|VIEWPORT_READY|RESIZE_GPU_READY|SESSION_RETIRED|LAUNCH|COMPLETED_FAILURE)([ \\\\n]|$)","_UID=1000","SYSLOG_IDENTIFIER=omarchy-gpu-companion",(char*)NULL);_exit(127);
            }
            int status=0,joined=0;
            if(journal>0) {
                alarm(5);joined=waitpid(journal,&status,0)==journal;
                if(!joined) { kill(journal,SIGKILL);waitpid(journal,&status,0); }
                alarm(0);
            }
            printf("NAMED_GRAPHICS_BROWSER_JOURNAL_CAPTURE joined=%s exited=%s code=%d orderEstablished=false\\n",
                joined?"true":"false",journal>0 && WIFEXITED(status)?"true":"false",journal>0 && WIFEXITED(status)?WEXITSTATUS(status):-1);
            puts("NAMED_GRAPHICS_BROWSER_OUTCOME_END exitStatusAloneIsNotGraphicsAcceptance=true");
        }
        {
            puts("NAMED_GRAPHICS_JOURNAL_LOSS_OUTCOME_BEGIN normalOmarchySession=false");
            fflush(stdout);
            const pid_t journal=fork();
            if(!journal) {
                if(setenv("SYSTEMD_COLORS","0",1) || setenv("SYSTEMD_URLIFY","0",1)) _exit(127);
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","--all","-n","16","-o","json","--output-fields=_PID,_UID,_SYSTEMD_USER_UNIT,_SYSTEMD_UNIT,_BOOT_ID,SYSLOG_IDENTIFIER,_TRANSPORT,MESSAGE","--grep","(?i)(suppressed|missed|dropped) [0-9]+ messages|rate.?limit","SYSLOG_IDENTIFIER=systemd-journald",(char*)NULL);_exit(127);
            }
            int status=0,joined=0;
            if(journal>0) {
                alarm(5);joined=waitpid(journal,&status,0)==journal;
                if(!joined) { kill(journal,SIGKILL);waitpid(journal,&status,0); }
                alarm(0);
            }
            printf("NAMED_GRAPHICS_JOURNAL_LOSS_CAPTURE joined=%s exited=%s code=%d orderEstablished=false\\n",
                joined?"true":"false",journal>0 && WIFEXITED(status)?"true":"false",journal>0 && WIFEXITED(status)?WEXITSTATUS(status):-1);
            puts("NAMED_GRAPHICS_JOURNAL_LOSS_OUTCOME_END exitStatusAloneIsNotGraphicsAcceptance=true");
        }
        {
            puts("NAMED_GRAPHICS_QUEUE_TIMING_OUTCOME_BEGIN lastSamplesOnly=true gpuTimestamps=false");
            fflush(stdout);
            const pid_t journal=journal_pid_valid ? fork() : -1;
            if(!journal) {
                if(setenv("SYSTEMD_COLORS","0",1) || setenv("SYSTEMD_URLIFY","0",1)) _exit(127);
                execl("/usr/bin/journalctl","journalctl","-b","--no-pager","--all","-n","64","-o","json","--output-fields=_PID,_UID,_SYSTEMD_USER_UNIT,_SYSTEMD_UNIT,_BOOT_ID,SYSLOG_IDENTIFIER,_TRANSPORT,MESSAGE","--grep","^HYPRLAND_ACTIVE_QUEUE_TIMING ",journal_pid,"_UID=1000","_SYSTEMD_USER_UNIT=wayland-wm@wddmprobe.desktop.service",(char*)NULL);_exit(127);
            }
            int status=0,joined=0;
            if(journal>0) {
                alarm(5);joined=waitpid(journal,&status,0)==journal;
                if(!joined) { kill(journal,SIGKILL);waitpid(journal,&status,0); }
                alarm(0);
            }
            printf("NAMED_GRAPHICS_QUEUE_TIMING_CAPTURE joined=%s exited=%s code=%d orderEstablished=false\\n",
                joined?"true":"false",journal>0 && WIFEXITED(status)?"true":"false",journal>0 && WIFEXITED(status)?WEXITSTATUS(status):-1);
            puts("NAMED_GRAPHICS_QUEUE_TIMING_OUTCOME_END fullStreamTimingCoverage=false");
        }
    }
    if(uwsm && compositor_pid>0) {'''
    for old in (before, wait, include, cleanup, admission, live_tail):
        if original.count(old) != 1:
            raise ValueError('Guest graphics discovery boundary changed')
    text = original.replace(before, after, 1).replace(wait, discover, 1).replace(include, expanded, 1).replace(cleanup, outcome, 1).replace(admission, sync_admission, 1).replace(live_tail, filtered_tail, 1)
    if text.replace(after, before, 1).replace(discover, wait, 1).replace(expanded, include, 1).replace(outcome, cleanup, 1).replace(sync_admission, admission, 1).replace(filtered_tail, live_tail, 1) != original:
        raise ValueError('Unexpected guest graphics discovery change')
    # This selects only the root broker's lifetime policy. The surrounding
    # probe's account, compositor workload and root-service alarm stay bounded.
    if broker_session:
        boundary = '    puts("NORMAL_ROOT_GRAPHICS_BEGIN normalSystemdService=true normalUid=1000 diagnosticCompositor=true normalOmarchySession=false");'
        selected = ('    if(setenv("WDDM_BRIDGE_BROKER_SESSION","1",1)) { perror("broker session environment");return 125; }\n'
                    '    puts("NAMED_GRAPHICS_BROKER_SESSION requested=true rootSupervisorOwned=true normalOmarchySession=false");\n'+boundary)
        if text.count(boundary) != 1:
            raise ValueError('Root broker session selection boundary changed')
        baseline = text
        text = text.replace(boundary, selected, 1)
        if text.replace(selected, boundary, 1) != baseline:
            raise ValueError('Unexpected root broker session selection change')
    target.write_text(text, encoding='utf-8', newline='\n')
    header = output/'guest_graphics_port.h'
    header.write_bytes((gpu/header.name).read_bytes())
    progress_header = output/'guest_progress_snapshot.h'
    progress_header.write_bytes((gpu/progress_header.name).read_bytes())
    receipt = dict(schema=1, parentLogging=parent,
        generatorSha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        generatedSourceSha256=hashlib.sha256(target.read_bytes()).hexdigest(),
        graphicsPortHeaderSha256=hashlib.sha256(header.read_bytes()).hexdigest(),
        discovery='unique sysfs name and matching character-device identity',
        terminalCompositorJournalLines=160, terminalStatusCapturedBeforeUserStop=True,
        terminalCompositorJournalFormat='json',
        terminalJournalColorDisabled=True, terminalJournalHyperlinksDisabled=True,
        terminalJournalCompleteValuesRequested=True,
        terminalJournalGroups=['session','applications','browser','journal-loss','queue-timing'],
        terminalJournalGroupLineLimits=[160,160,96,16,64],
        terminalQueueTimingLastSamplesOnly=True,
        terminalQueueTimingAdditionalProducerLogs=False,
        producerProgressByteCap=512,
        producerProgressHeaderSha256=hashlib.sha256(progress_header.read_bytes()).hexdigest(),
        producerProgressReadOnly=True,
        producerProgressSingleWriteRecord=True,
        producerProgressMaximumRecordBytes=1216,
        producerProgressProcessIdentityAttested=False,
        producerLifetimeChanged=False,
        guestBrokerSessionEnabled=broker_session,
        guestBrokerLifetimeOwner='pinned-root-supervisor' if broker_session else 'diagnostic-alarm',
        guestRootSupervisorLifetimeChanged=False,
        terminalCompositorPidAndUidFilter=True,
        terminalBrowserSyslogAndUidFilter=True,
        terminalBrowserRowsRequireOwnedPidCorrelation=True,
        terminalJournalLossReportsCaptured=True,
        journalLossAbsenceEstablishesCompleteness=False,
        terminalJournalOutputByteCapImplemented=False,
        terminalJournalPerQueryDeadlineSeconds=5,
        guestBuildVerified=False, hardwareRunVerified=False,

        terminalJournalPreservesPidUidBootAndUnit=True,
        terminalJournalMatchesMultilineMessages=True,
        liveJournalStoppedBeforeTerminalCapture=True,
        terminalJournalExcludesNoisyGpuTeardown=True,
        kernelSyncAdmissionDiagnostics=True,
        liveSerialJournalFiltersVerboseIoctlDiagnostics=True,
        liveWorkloadInputRendererAndFailureMarkersRetained=True,
        guestJournalStorageChanged=False,
        fullPerIoctlGuestTraceRetainedOnHost=False,
        fixedPortOrdinal=False, opensOtherDevicePorts=False,
        inputImplementationChanged=False, gpuProtocolChanged=False,
        probeSessionStillBounded=True, normalAccountBootstrapImplemented=False,
        dailyUseAccepted=False)
    (output/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n', encoding='utf-8', newline='\n')
    return receipt


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--broker-session', action='store_true', help='Select the matched root-owned broker lifetime; the guest session remains diagnostic')
    args = parser.parse_args()
    print(json.dumps(generate(Path(__file__).resolve().parent.parent, args.output, broker_session=args.broker_session), indent=2))
