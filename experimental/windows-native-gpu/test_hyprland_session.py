"""Build and exercise the real preload entry with mocked GPU/EGL and real FDs.

No VM, GPU, display server, input, root privilege or desktop is used.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import subprocess
import socket

GPU = Path(__file__).resolve().parent
ARGS = ['--config', '/home/test/My settings.lua', '--socket', 'test socket']

def run(output, compiler='cc'):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    frozen = output / 'source'
    frozen.mkdir()
    for name in ('hyprland_session.c','hyprland_session_invocation.h','hyprland_session_fixture.c','test_hyprland_session.py'):
        (frozen/name).write_bytes((GPU/name).read_bytes())
    executable = output / 'Hyprland'
    library = output / 'hyprland-session.so'
    test_library = output / 'hyprland-session-cpu-path.so'
    flags = ['-std=c11', '-O2', '-Wall', '-Wextra', '-Werror']
    commands = [
        [compiler, *flags, '-shared', '-fPIC', str(frozen/'hyprland_session.c'), '-ldl', '-o', str(library)],
        [compiler, *flags, '-shared', '-fPIC', '-DWDDM_SESSION_COMPOSITOR_PATH='+json.dumps(str(executable)), str(frozen/'hyprland_session.c'), '-ldl', '-o', str(test_library)],
        [compiler, *flags, '-rdynamic', str(frozen/'hyprland_session_fixture.c'), '-ldl', '-o', str(executable)],
    ]
    for index, command in enumerate(commands):
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        (output/f'compile-{index}.log').write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f'Compile failed: {output}/compile-{index}.log')
    base = os.environ.copy()
    for name in ('WDDM_BRIDGE_COMPOSITOR_SESSION','WDDM_BRIDGE_SESSION_SIZE','D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE','D3D12_EXPERIMENTAL_NATIVE_BUFFERS','AQ_EXPERIMENTAL_NATIVE_CAPTURE'):
        base.pop(name, None)
    base.update(LD_PRELOAD=str(test_library), SESSION_UNCHANGED='literal value with spaces')
    results = []
    def control(name, changes, expected, marker=None, timeout=15, arguments=None, pass_fds=()):
        environment = base.copy()
        environment.update(changes)
        for key in [key for key,value in environment.items() if value is None]:
            del environment[key]
        if arguments is not None:
            environment['SESSION_TEST_ARGUMENTS']=''.join(arg+'\n' for arg in arguments)
        result = subprocess.run([str(executable), *(ARGS if arguments is None else arguments)], env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout,pass_fds=pass_fds)
        (output/(name+'.log')).write_text(result.stdout)
        if result.returncode != expected or (marker and marker not in result.stdout):
            raise AssertionError(f'{name}: expected {expected}, got {result.returncode}: {result.stdout}')
        results.append(dict(name=name, exit=result.returncode, gpuMock=True))
    selected = dict(WDDM_BRIDGE_COMPOSITOR_SESSION='1', D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE='true',D3D12_EXPERIMENTAL_NATIVE_BUFFERS='true')
    # The installed start-hyprland watchdog probes --version-json before the
    # real compositor. Queries and rejected syntax must not claim its GPU role.
    query_sets=[['--version-json'],['--version'],['-v'],['--help'],['-h'],['--systeminfo'],['--verify-config'],
                ['--socket','--version-json','--help'],['--config'],['--unknown-option'],['--version-json','--config','ignored']]
    for index,arguments in enumerate(query_sets):
        control('query-or-syntax-'+str(index),dict(selected,SESSION_TEST_QUERY='1',SESSION_TEST_CASE='begin_failure'),
                17,'nativeBegins=0 argvPreserved=true',arguments=arguments)
    control('query-without-gpu-environment',dict(WDDM_BRIDGE_COMPOSITOR_SESSION='1',SESSION_TEST_QUERY='1'),
            17,'nativeBegins=0',arguments=['--version-json'])
    # An option-like config/socket/command value cannot bypass native startup.
    for option in ('--config','-c','--socket','--locked-cmd'):
        control('literal-value-'+option.lstrip('-'),dict(selected,SESSION_TEST_CASE='roundtrip'),0,
                'frames=10000',arguments=[option,'--version-json'])
    watchdog_parent,watchdog_child=socket.socketpair()
    try:
        watchdog_parent.settimeout(2)
        fd=watchdog_child.fileno()
        control('watchdog-descriptor-and-flags',dict(selected,SESSION_TEST_CASE='roundtrip',SESSION_TEST_WATCHDOG_FD=str(fd)),
                0,'frames=10000',arguments=['--watchdog-fd',str(fd),'--safe-mode','--locked',*ARGS],pass_fds=(fd,))
        assert watchdog_parent.recv(1)==b'W'
    finally:
        watchdog_child.close();watchdog_parent.close()
    control('default', {}, 23, 'inactive argvPreserved=true')
    control('production-path-is-exact', dict(selected, LD_PRELOAD=str(library)), 23, 'inactive argvPreserved=true')
    for bad in ('', '0', 'true', '01'):
        control('invalid-option-'+repr(bad).replace("'",''), dict(selected, WDDM_BRIDGE_COMPOSITOR_SESSION=bad),125)
    control('missing-native-sync', dict(selected,D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE=None),125)
    control('disabled-native-sync',dict(selected,D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE='false'),125)
    control('missing-native-buffers',dict(selected,D3D12_EXPERIMENTAL_NATIVE_BUFFERS=None),125)
    control('disabled-native-buffers',dict(selected,D3D12_EXPERIMENTAL_NATIVE_BUFFERS='false'),125)
    control('reject-cpu-capture',dict(selected,AQ_EXPERIMENTAL_NATIVE_CAPTURE='1'),125)
    control('unsupported-size',dict(selected,WDDM_BRIDGE_SESSION_SIZE='4096x2160'),125)
    for size in ('1920x1080','3840x2160'):
        control('rolling-'+size,dict(selected,WDDM_BRIDGE_SESSION_SIZE=size,SESSION_TEST_CASE='roundtrip'),0,'frames=10000 attempts=10000 fdBalanced=true')
    for scenario in ('retry','fd_zero','reentrant','main_failure','wrong_bytes','no_display','startup_no_display','late_no_display','startup_wrong_bytes','create_failure','duplicate_failure',
                     'destroy_failure','no_egl_entry','queue_failure','wrong_sequence','invalid_fd','wrong_size','wrong_stride','wrong_format','deadline'):
        expected = 0 if scenario in ('retry','fd_zero','reentrant','startup_no_display') else 37 if scenario=='main_failure' else 125
        control(scenario,dict(selected,SESSION_TEST_CASE=scenario),expected,'fdBalanced=true')
        failure_stages={'wrong_bytes':'buffer-bytes','startup_wrong_bytes':'buffer-bytes','no_display':'egl-current-display','late_no_display':'egl-current-display','no_egl_entry':'egl-entry-points',
                        'create_failure':'egl-create-sync','duplicate_failure':'egl-duplicate-sync',
                        'queue_failure':'native-queue','wrong_sequence':'queue-sequence','destroy_failure':'destroy-egl-sync'}
        if scenario in failure_stages:
            assert 'stage='+failure_stages[scenario] in (output/(scenario+'.log')).read_text()
        if scenario in ('startup_no_display','no_display'):
            assert 'nativeQueueCalls=0 fencesCreated=0 renderedFrames=0' in (output/(scenario+'.log')).read_text()
    control('begin_failure',dict(selected,SESSION_TEST_CASE='begin_failure'),125)
    # A real full pipe with a BLOCKING inherited writer must not hang either
    # startup, failure, or return diagnostics. The original flags stay unchanged.
    for scenario,expected in [('roundtrip',0),('queue_failure',125)]:
        read_fd,write_fd=os.pipe2(os.O_CLOEXEC|os.O_NONBLOCK)
        try:
            filled=0
            while filled<1024*1024:
                try:filled+=os.write(write_fd,b'x'*4096)
                except BlockingIOError:break
            else:raise AssertionError('Could not obtain actual pipe backpressure')
            original_flags=fcntl.fcntl(write_fd,fcntl.F_GETFL)&~os.O_NONBLOCK
            fcntl.fcntl(write_fd,fcntl.F_SETFL,original_flags)
            environment=base.copy();environment.update(selected,SESSION_TEST_CASE=scenario)
            result=subprocess.run([str(executable),*ARGS],env=environment,stdout=subprocess.PIPE,
                                  stderr=write_fd,text=True,timeout=5)
            name='full-stderr-pipe-'+scenario
            (output/(name+'.log')).write_text(result.stdout)
            assert result.returncode==expected and 'fdBalanced=true' in result.stdout
            assert fcntl.fcntl(write_fd,fcntl.F_GETFL)==original_flags
            results.append(dict(name=name,exit=result.returncode,gpuMock=True,
                                actualPipeBackpressureBytes=filled,parentDiagnosticFlagsUnchanged=True))
        finally:
            os.close(write_fd);os.close(read_fd)
    # Reopening a regular log must append, never overwrite earlier records.
    with (output/'existing-diagnostics.log').open('w+b') as sink:
        prefix=b'existing diagnostic prefix must survive\n'
        sink.write(prefix);sink.flush()
        original_flags=fcntl.fcntl(sink.fileno(),fcntl.F_GETFL)
        environment=base.copy();environment.update(selected,SESSION_TEST_CASE='roundtrip')
        result=subprocess.run([str(executable),*ARGS],env=environment,stdout=subprocess.PIPE,
                              stderr=sink,text=True,timeout=5)
        assert result.returncode==0 and 'fdBalanced=true' in result.stdout
        assert fcntl.fcntl(sink.fileno(),fcntl.F_GETFL)==original_flags
        sink.seek(0);log=sink.read()
        assert log.startswith(prefix) and b'HYPRLAND_SESSION_BEGIN' in log and b'HYPRLAND_SESSION_RETURN' in log
        results.append(dict(name='existing-regular-log-preserved',exit=0,gpuMock=True,
                            parentDiagnosticFlagsUnchanged=True,existingLogPrefixPreserved=True))
    # Real journald-style UNIX stream sockets cannot be reopened via /proc.
    # Ordinary messages must arrive; full or closed peers must never block or
    # raise SIGPIPE, and the inherited open-file flags must stay unchanged.
    for socket_mode in ('readable','full','closed'):
        reader,writer=socket.socketpair()
        try:
            filled=0
            if socket_mode=='full':
                writer.setblocking(False)
                while filled<4*1024*1024:
                    try:filled+=writer.send(b'x'*4096)
                    except BlockingIOError:break
                else:raise AssertionError('Could not fill actual diagnostic socket')
                writer.setblocking(True)
            if socket_mode=='closed':reader.close()
            original_flags=fcntl.fcntl(writer.fileno(),fcntl.F_GETFL)
            environment=base.copy();environment.update(selected,SESSION_TEST_CASE='queue_failure')
            result=subprocess.run([str(executable),*ARGS],env=environment,stdout=subprocess.PIPE,
                                  stderr=writer,text=True,timeout=5)
            assert result.returncode==125 and 'fdBalanced=true' in result.stdout
            assert fcntl.fcntl(writer.fileno(),fcntl.F_GETFL)==original_flags
            if socket_mode=='readable':
                reader.settimeout(2);received=reader.recv(4096)
                assert b'HYPRLAND_SESSION_BEGIN' in received and b'HYPRLAND_SESSION_FAILURE' in received
            results.append(dict(name='journal-socket-'+socket_mode,exit=result.returncode,gpuMock=True,
                                parentDiagnosticFlagsUnchanged=True,backpressureBytes=filled))
        finally:
            reader.close();writer.close()
    receipt = dict(cpuControlsPassed=len(results), gpuMock=True, vmLaunched=False, inputSent=False,
                   persistentHardwareSessionVerified=False, dailyUseAccepted=False, controls=results,
                   sourceSha256={name:hashlib.sha256((frozen/name).read_bytes()).hexdigest() for name in ('hyprland_session.c','hyprland_session_invocation.h','hyprland_session_fixture.c','test_hyprland_session.py')},
                   compileCommands=commands,
                   productionLibrarySha256=hashlib.sha256(library.read_bytes()).hexdigest())
    (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({key:receipt[key] for key in ('cpuControlsPassed','gpuMock','vmLaunched','dailyUseAccepted')}))

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',required=True)
    parser.add_argument('--cc',default='cc')
    args=parser.parse_args()
    run(args.output,args.cc)
