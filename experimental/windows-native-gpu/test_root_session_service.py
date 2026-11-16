"""Exercise the real root service with real credentials/pidfds and a CPU broker.

Never loads modules, boots a VM, touches input or starts a desktop. Root is used
only inside an owned Linux temporary directory for file/peer credential controls.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

GPU=Path(__file__).resolve().parent
UID=GID=65534
ARGS=['--config','/home/test/My settings.lua','--socket','test socket']

def run(output,cc='cc'):
    if os.getuid()!=0:raise RuntimeError('Root required only for isolated CPU credential controls')
    # Linux UAPI PR_SET_CHILD_SUBREAPER=36. Scope is only this test process:
    # an orphaned CPU broker must be observable and reaped, never left to PID1.
    libc=ctypes.CDLL(None,use_errno=True)
    libc.prctl.argtypes=[ctypes.c_int,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_ulong]
    assert libc.prctl(36,1,0,0,0)==0
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=False)
    frozen=output/'source';frozen.mkdir();closure={}
    def freeze(name):
        if name in closure:return
        path=GPU/name;raw=path.read_bytes();target=frozen/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(raw)
        closure[name]=hashlib.sha256(raw).hexdigest()
        for include in re.findall(r'^\s*#include\s+"([^"\n]+)"',raw.decode(),re.M):freeze((Path(name).parent/include).as_posix())
    for name in ('normal_session_service.c','normal_session_devices.c','normal_session_service_test.c','normal_session_peer_test.c',
                 'normal_session_broker_test.c','hyprland_session.c','hyprland_session_fixture.c','guest_session_activation_test.c','test_root_session_service.py'):freeze(name)
    assert 'guest-kernel/wddm_sync_file.h' not in closure
    temporary=Path(tempfile.mkdtemp(prefix='omarchy-root-session-'));temporary.chmod(0o755)
    assert temporary.parent==Path(tempfile.gettempdir()) and temporary.name.startswith('omarchy-root-session-')
    root=temporary/'r';broker_dir=temporary/'b';owner=temporary/'owner.conf';umd=temporary/'umd';witness=temporary/'broker-started'
    wrapper=temporary/'wrapper';broker=temporary/'broker';target=temporary/'Hyprland';hook=temporary/'session.so';client=temporary/'client'
    service=temporary/'service';wrong_group=temporary/'wrong-group-service';notify_path=temporary/'notify.sock'
    cgroup=Path('/proc/self/cgroup').read_text().strip();assert cgroup.startswith('0::') and '\n' not in cgroup;cgroup=cgroup[3:]
    flags=['-std=c11','-O2','-Wall','-Wextra','-Werror']
    define=lambda key,value:'-D'+key+'='+json.dumps(str(value))
    service_flags=[define('WDDM_SESSION_ROOT',root),define('WDDM_SESSION_OWNER_FILE',owner),define('WDDM_SESSION_WRAPPER',wrapper),
                   define('WDDM_SESSION_BROKER',broker),define('WDDM_SESSION_BROKER_DIRECTORY',broker_dir),
                   define('WDDM_SESSION_PRELOAD',hook),define('WDDM_SESSION_UMD_FILE',umd)]
    wrapper_flags=[define('WDDM_SESSION_EXEC_TARGET',target),define('WDDM_SESSION_ENVIRONMENT',root/'compositor.env'),
                   define('WDDM_SESSION_ACTIVATION_SOCKET',root/'session.sock')]
    commands=[];children=[];results=[]
    try:
        def compile(name,source,extra=(),dest=None):
            dest=dest or temporary/name
            command=[cc,*flags,*extra,str(frozen/source),'-o',str(dest)]
            commands.append(command);result=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
            (output/(name+'.build.log')).write_text(result.stdout)
            if result.returncode:raise RuntimeError('Compile failed: '+str(output/(name+'.build.log')))
            return dest
        compile('Hyprland','hyprland_session_fixture.c',('-rdynamic','-ldl'))
        compile('session.so','hyprland_session.c',('-shared','-fPIC',define('WDDM_SESSION_COMPOSITOR_PATH',target),'-ldl'))
        compile('wrapper','normal_session_peer_test.c',wrapper_flags)
        compile('client','guest_session_activation_test.c')
        compile('service','normal_session_service_test.c',[*service_flags,define('WDDM_SESSION_TEST_CGROUP',cgroup)])
        compile('wrong-group-service','normal_session_service_test.c',[*service_flags,define('WDDM_SESSION_TEST_CGROUP','/definitely-not-this-unit')])
        variants={}
        for mode in (0,1,2,3,4,5):
            variants[mode]=compile('broker-'+str(mode),'normal_session_broker_test.c',
                                   ['-DCPU_BROKER_MODE='+str(mode),define('CPU_BROKER_WITNESS',witness)])
        compile('production-service','normal_session_service.c',dest=output/'normal-session-service')
        compile('production-devices','normal_session_devices.c',dest=output/'normal-session-devices')
        assert pwd.getpwnam('nobody').pw_uid==UID and pwd.getpwnam('nobody').pw_gid==GID
        normal='[Autologin]\nUser=nobody\nSession=omarchy.desktop\n'
        def config(text=normal,mode=0o644,user=0):
            if owner.exists() or owner.is_symlink():owner.unlink()
            owner.write_bytes(text.encode() if isinstance(text,str) else text);os.chown(owner,user,0);owner.chmod(mode)
        config();umd.write_text('libcpu_mock.so\n');umd.chmod(0o644)
        base=os.environ.copy()
        for key in list(base):
            if key.startswith(('WDDM_','SESSION_','ROOT_SERVICE_','D3D12_EXPERIMENTAL_')) or key in ('LD_PRELOAD','LD_LIBRARY_PATH','NOTIFY_SOCKET'):base.pop(key)
        base.update(SESSION_UNCHANGED='literal value with spaces',SESSION_TEST_CASE='roundtrip',SESSION_EXPECT_HOME='/home/custom-owner',HOME='/home/custom-owner',
                    SESSION_TEST_ARGUMENTS=''.join(arg+'\n' for arg in ARGS),SESSION_TEST_CLEAN_ENV='1',
                    EGL_PLATFORM='wayland',WDDM_BRIDGE_PORT='forbidden',WDDM_BRIDGE_WINDOWS_WORKER='forbidden',
                    GBM_BACKENDS_PATH='/must-not-inherit',SESSION_EXPECT_GBM_BACKENDS_PATH='/opt/wddm-mesa/lib/gbm',
                    D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE='false',D3D12_EXPERIMENTAL_NATIVE_BUFFERS='false',SESSION_EXPECT_D3D12_INTEROP='1')
        def spawn(binary,args=(),env=None,user=0,group=0,pass_fds=(),umask=-1):
            child=subprocess.Popen([str(binary),*args],env=env or base,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                                    user=user,group=group,extra_groups=[],pass_fds=pass_fds,umask=umask)
            children.append(child);return child
        def finish(child,name,expected=0,timeout=15):
            try:stdout,stderr=child.communicate(timeout=timeout)
            except BaseException:
                # Do not wait for EOF on a pipe held by an orphaned descendant.
                if child.poll() is None:child.kill()
                child.wait(timeout=3)
                if child.stdout:child.stdout.close()
                if child.stderr:child.stderr.close()
                raise
            (output/(name+'.log')).write_text(stdout+stderr)
            acceptable={expected} if isinstance(expected,int) else set(expected)
            assert child.returncode in acceptable,(name,child.returncode,stdout,stderr)
            return stdout
        def service_start(mode=0,wrong=False):
            assert not root.exists() and not broker_dir.exists()
            if witness.exists():witness.unlink()
            shutil.copyfile(variants[mode],broker);broker.chmod(0o755)
            if notify_path.exists():notify_path.unlink()
            notification=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM);notification.bind(str(notify_path));notification.settimeout(3)
            env=base.copy();env.update(NOTIFY_SOCKET=str(notify_path),WDDM_BRIDGE_SESSION_SIZE='3840x2160')
            parent=spawn(wrong_group if wrong else service,env=env,umask=0o077)
            try:assert notification.recv(512).startswith(b'READY=1\n')
            except BaseException:
                parent.kill();out,err=parent.communicate();raise AssertionError((parent.returncode,out,err))
            finally:notification.close()
            assert (root/'compositor.env').exists() and (root/'session.sock').is_socket() and not witness.exists()
            return parent
        def service_stop(parent,name,expected=0):
            if parent.poll() is None:parent.send_signal(signal.SIGTERM)
            finish(parent,name,expected,timeout=18)
            assert not root.exists() and not broker_dir.exists(),(name,'runtime paths retained')
        def fd_count(parent):return len(list(Path(f'/proc/{parent.pid}/fd').iterdir()))
        def peer(mode=None):
            env=base.copy()
            if mode:env['ROOT_SERVICE_PEER_TEST']=mode
            return spawn(wrapper,ARGS,env,user=UID,group=GID)
        # No broker lifetime or primary-registration timer starts during idle.
        parent=service_start();baseline_fds=fd_count(parent);started=time.monotonic()
        while time.monotonic()-started<31:
            assert parent.poll() is None and not witness.exists();time.sleep(.2)
        assert fd_count(parent)==baseline_fds
        service_stop(parent,'idle-before-primary')
        results.append(dict(name='idle-over-primary-registration-budget',seconds=time.monotonic()-started,brokerStarted=False,fdBounded=True))
        # Real service -> real wrapper -> canonical mock compositor, same PID.
        parent=service_start();read_gate,write_gate=os.pipe2(os.O_CLOEXEC)
        env=base.copy();env['SESSION_TEST_PAUSE_FD']=str(read_gate)
        child=spawn(wrapper,ARGS,env,user=UID,group=GID,pass_fds=(read_gate,))
        try:
            assert select.select([child.stdout],[],[],5)[0]
            line=child.stdout.readline();assert f'pid={child.pid} ' in line,line
            assert os.readlink(f'/proc/{child.pid}/exe')==str(target) and witness.read_bytes()==b'S'
            assert parent.poll() is None
            os.write(write_gate,b'G');out=finish(child,'normal-owned-primary')
            assert 'frames=10000' in out and 'fdBalanced=true' in out
            finish(parent,'normal-service-retirement');assert not root.exists() and not broker_dir.exists()
            results.append(dict(name='real-service-and-wrapper-handoff',samePidThroughExec=True,brokerStarts=1,mockFrames=10000))
        finally:os.close(read_gate);os.close(write_gate)
        # Packet rejection leaves the actual service alive, FD-balanced and with
        # ZERO GPU-broker starts, including delivered/truncated SCM_RIGHTS.
        parent=service_start();baseline_fds=fd_count(parent)
        for mode in ('magic','version','size','status','reserved','nonce','short','long','rights','truncated-rights','idle'):
            for repeat in range(8 if mode in ('rights','truncated-rights') else 1):
                child=peer(mode);finish(child,'reject-'+mode+'-'+str(repeat))
                until=time.monotonic()+2
                while fd_count(parent)!=baseline_fds and time.monotonic()<until:time.sleep(.01)
                assert parent.poll() is None and fd_count(parent)==baseline_fds and not witness.exists()
            results.append(dict(name='reject-'+mode,brokerStarted=False,serverFdsBalanced=True))
        # Wrong image and UID have a real root-authenticated socket but cannot
        # reach the backend. Client API observes a normal denial and closes FDs.
        for name,uid in [('wrong-image',UID),('wrong-uid',UID-1)]:
            child=spawn(client,[str(root/'session.sock'),'1000','1'],user=uid,group=GID)
            out=finish(child,name);assert 'fdBalancedBeforeExit=true' in out
            assert parent.poll() is None and not witness.exists()
            results.append(dict(name=name,brokerStarted=False))
        service_stop(parent,'reject-service-cleanup')
        parent=service_start(wrong=True);child=peer('magic');finish(child,'wrong-cgroup-peer',expected=(0,-6))
        assert not witness.exists();service_stop(parent,'wrong-cgroup-cleanup')
        results.append(dict(name='wrong-cgroup',brokerStarted=False,actualMembershipChecked=True))
        # Cancellation during a partial request must not wait for its deadline.
        parent=service_start();child=peer('idle');time.sleep(.05);started=time.monotonic()
        service_stop(parent,'request-cancelled');finish(child,'request-cancelled-peer')
        assert time.monotonic()-started<2 and not witness.exists()
        results.append(dict(name='request-cancellation',brokerStarted=False))
        # Backend startup failure, bad readiness and early post-registration
        # failure all fail closed. Every mock broker is reaped and paths clean.
        for mode in (1,2,3,4):
            parent=service_start(mode);child=peer();finish(child,'backend-failure-peer-'+str(mode),expected=(0,125,-15) if mode==4 else (125,-15))
            finish(parent,'backend-failure-root-'+str(mode),125,timeout=18)
            assert witness.read_bytes()==b'S' and not root.exists() and not broker_dir.exists()
            results.append(dict(name='backend-failure-'+str(mode),rootExit=125,ownedBrokerReaped=True))
        parent=service_start(5);child=peer();finish(child,'broker-refuses-drain-peer')
        finish(parent,'broker-refuses-drain-root',125,timeout=17)
        assert witness.read_bytes()==b'S' and not root.exists() and not broker_dir.exists()
        results.append(dict(name='broker-drain-expiry',rootExit=125,ownedBrokerKilledAndReaped=True))
        # Active cancellation signals only the pinned compositor. Mock broker
        # observes the actual pidfd exit and retires before the root exits.
        parent=service_start();read_gate,write_gate=os.pipe2(os.O_CLOEXEC)
        env=base.copy();env['SESSION_TEST_PAUSE_FD']=str(read_gate)
        child=spawn(wrapper,ARGS,env,user=UID,group=GID,pass_fds=(read_gate,))
        try:
            assert select.select([child.stdout],[],[],5)[0] and f'pid={child.pid} ' in child.stdout.readline()
            service_stop(parent,'active-cancelled');finish(child,'active-cancelled-peer',expected=-15)
            assert witness.read_bytes()==b'S'
            results.append(dict(name='active-cancellation',primaryPinned=True,ownedBrokerReaped=True,gpuMock=True))
        finally:os.close(read_gate);os.close(write_gate)
        # No account mutation: malformed/untrusted owner files fail before any
        # runtime directory, listener or backend is created.
        invalid=[('missing-user','[Autologin]\nSession=omarchy.desktop\n'),('root-user',normal.replace('nobody','root')),
                 ('wrong-session',normal.replace('omarchy.desktop','other.desktop')),('duplicate-user',normal+'User=nobody\n'),
                 ('duplicate-section',normal+'[Autologin]\n'),('unknown-user',normal.replace('nobody','no-such-omarchy-user-83')),('nul',normal.encode()+b'\0')]
        for name,data in invalid:
            config(data);parent=spawn(service);finish(parent,'owner-'+name,125,timeout=2)
            assert not root.exists() and not broker_dir.exists()
            results.append(dict(name='owner-'+name,runtimeCreated=False))
        for name,mode,user in [('writable',0o666,0),('untrusted',0o644,UID)]:
            config(mode=mode,user=user);parent=spawn(service);finish(parent,'owner-'+name,125,timeout=2)
            assert not root.exists();results.append(dict(name='owner-'+name,runtimeCreated=False))
        config();other=temporary/'owner-real';owner.rename(other);owner.symlink_to(other)
        parent=spawn(service);finish(parent,'owner-symlink',125,timeout=2);assert not root.exists()
        results.append(dict(name='owner-symlink',runtimeCreated=False))
        assert all(child.poll() is not None for child in children)
        try:
            unexpected=os.waitpid(-1,os.WNOHANG)
        except ChildProcessError:
            pass
        else:
            raise AssertionError(('Owned broker was not reaped by its service',unexpected))
        # Deliberately orphan a CPU-only pipe holder. The harness timeout must
        # return promptly even though the already exited parent cannot close it.
        holder="import os,time\nif os.fork()==0:\n time.sleep(30);os._exit(0)\nos._exit(0)"
        child=spawn(Path(sys.executable),['-c',holder]);started=time.monotonic()
        try:finish(child,'harness-inherited-pipe-timeout',timeout=.2)
        except subprocess.TimeoutExpired:pass
        else:raise AssertionError('The orphaned pipe was not held open')
        assert time.monotonic()-started<3
        adopted=Path(f'/proc/self/task/{os.getpid()}/children').read_text().split()
        assert adopted
        for value in adopted:
            pid=int(value);os.kill(pid,signal.SIGKILL);assert os.waitpid(pid,0)[0]==pid
        try:unexpected=os.waitpid(-1,os.WNOHANG)
        except ChildProcessError:pass
        else:raise AssertionError(('Test orphan was not reaped',unexpected))
        results.append(dict(name='harness-orphaned-pipe-timeout',returnedPromptly=True,ownedDescendantReaped=True))
        receipt=dict(controlsPassed=len(results),rootAdmissionServiceImplemented=True,gpuAndKernelDevicesMocked=True,
            actualBrokerReplacedByCpuBroker=True,actualUWSMUnitTested=False,testCgroup=cgroup,noOrphanedBrokerChildren=True,
            normalGpuLoginVerified=False,vmLaunched=False,hostInputSent=False,controls=results,
            strictC11Build=True,privateUnitUmaskExercised=True,brokerInheritedOnlyExplicitFds=True,brokerStopSignalsUnblocked=True,sourceSha256=closure,compileCommands=commands,
            productionServiceSha256=hashlib.sha256((output/'normal-session-service').read_bytes()).hexdigest(),
            productionDevicePreparationSha256=hashlib.sha256((output/'normal-session-devices').read_bytes()).hexdigest())
        (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(json.dumps({key:receipt[key] for key in ('controlsPassed','rootAdmissionServiceImplemented','gpuAndKernelDevicesMocked','normalGpuLoginVerified')}))
    finally:
        for child in children:
            if child.poll() is None:child.kill()
            child.wait()
        # The subreaper owns any descendant orphaned by a failing test. These
        # kernel-reported child PIDs cannot be recycled before this waitpid.
        child_list=Path(f'/proc/self/task/{os.getpid()}/children').read_text().split()
        for value in child_list:
            pid=int(value)
            try:os.kill(pid,signal.SIGKILL)
            except ProcessLookupError:pass
            try:os.waitpid(pid,0)
            except ChildProcessError:pass
        # Only the isolated Linux directory created by this harness is removed.
        assert temporary.parent==Path(tempfile.gettempdir()) and temporary.name.startswith('omarchy-root-session-')
        shutil.rmtree(temporary)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True);parser.add_argument('--cc',default='cc')
    args=parser.parse_args();run(args.output,args.cc)
