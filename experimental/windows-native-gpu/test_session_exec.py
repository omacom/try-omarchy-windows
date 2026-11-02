"""Root-owned CPU/IPC controls for the real persistent compositor exec wrapper.

Uses an isolated Linux temporary directory, a mocked GPU compositor and a mock
root activation service. No VM, GPU, display, user account or host input is used.
"""
import argparse
import array
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import shutil
import socket
import struct
import subprocess
import tempfile
import time

GPU=Path(__file__).resolve().parent
PACKET=struct.Struct('=IHHII32s')
REQUEST=0x51534157
READY=0x52534157
UID=GID=65534
ARGS=['--config','/home/test/My settings.lua','--socket','test socket']
SOURCES=('hyprland_session_exec.c','guest_session_activation.h','guest_session_activation_test.c',
         'hyprland_session_invocation.h','hyprland_session.c','hyprland_session_fixture.c','test_session_exec.py')

def run(output,cc='cc'):
    if os.getuid()!=0:
        raise RuntimeError('Use root only for isolated file ownership and Unix peer credential controls')
    output=Path(output).resolve();output.mkdir(parents=True,exist_ok=False)
    frozen=output/'source';frozen.mkdir()
    for name in SOURCES:(frozen/name).write_bytes((GPU/name).read_bytes())
    temporary=Path(tempfile.mkdtemp(prefix='omarchy-session-exec-'))
    assert temporary.parent==Path(tempfile.gettempdir()) and temporary.name.startswith('omarchy-session-exec-')
    temporary.chmod(0o755)
    config=temporary/'compositor.env';endpoint=temporary/'session.sock'
    target=temporary/'Hyprland';hook=temporary/'session.so';wrapper=temporary/'session-exec';client=temporary/'activation-client'
    flags=['-std=c11','-O2','-Wall','-Wextra','-Werror']
    commands=[
        [cc,*flags,'-rdynamic',str(frozen/'hyprland_session_fixture.c'),'-ldl','-o',str(target)],
        [cc,*flags,'-shared','-fPIC','-DWDDM_SESSION_COMPOSITOR_PATH='+json.dumps(str(target)),str(frozen/'hyprland_session.c'),'-ldl','-o',str(hook)],
        [cc,*flags,'-DWDDM_SESSION_EXEC_TARGET='+json.dumps(str(target)),
         '-DWDDM_SESSION_ENVIRONMENT='+json.dumps(str(config)),'-DWDDM_SESSION_ACTIVATION_SOCKET='+json.dumps(str(endpoint)),
         str(frozen/'hyprland_session_exec.c'),'-o',str(wrapper)],
        [cc,*flags,str(frozen/'guest_session_activation_test.c'),'-o',str(client)],
        [cc,*flags,str(frozen/'hyprland_session_exec.c'),'-o',str(output/'hyprland-session-exec')],
    ]
    results=[];nonces=set();children=[];servers=[]
    try:
        for index,command in enumerate(commands):
            result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            (output/f'compile-{index}.log').write_text(result.stdout)
            if result.returncode:raise RuntimeError(f'Compile failed: {output}/compile-{index}.log')
        base=os.environ.copy()
        for name in list(base):
            if name.startswith(('WDDM_','SESSION_TEST_','D3D12_EXPERIMENTAL_')) or name in ('LD_PRELOAD','LD_LIBRARY_PATH'):
                base.pop(name)
        base.update(SESSION_UNCHANGED='literal value with spaces',SESSION_TEST_CASE='roundtrip',
                    SESSION_EXPECT_HOME='/home/custom-owner',HOME='/home/custom-owner',EGL_PLATFORM='wayland',
                    WDDM_BRIDGE_PORT='/must-not-inherit',WDDM_BRIDGE_WINDOWS_WORKER='must-not-inherit',SESSION_TEST_CLEAN_ENV='1')
        normal=('WDDM_SESSION_VERSION=1\n'+f'WDDM_SESSION_UID={UID}\nWDDM_SESSION_GID={GID}\n'+
                f'LD_PRELOAD="{hook}"\nLD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu"\n'+
                'D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE="true"\nD3D12_EXPERIMENTAL_NATIVE_BUFFERS="true"\n'+
                'WDDM_BRIDGE_COMPOSITOR_SESSION="1"\nWDDM_BRIDGE_SESSION_SIZE="3840x2160"\n'+
                'GBM_BACKENDS_PATH="/opt/wddm-mesa/lib/gbm"\n')
        def write_config(data=normal,mode=0o644,owner=0):
            if config.is_symlink() or config.exists():config.unlink()
            config.write_bytes(data.encode() if isinstance(data,str) else data)
            os.chown(config,owner,0);config.chmod(mode)
        def listen(path=endpoint):
            if path.exists():path.unlink()
            channel=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
            channel.bind(str(path));os.chmod(path,0o666);channel.listen(4);channel.settimeout(4)
            return channel
        def environment(arguments,query=False):
            env=base.copy();env['SESSION_TEST_ARGUMENTS']=''.join(arg+'\n' for arg in arguments)
            if query:
                env.update(LD_PRELOAD=str(hook),WDDM_BRIDGE_COMPOSITOR_SESSION='1',SESSION_TEST_QUERY='1')
                env.pop('SESSION_TEST_CLEAN_ENV',None)
            else:
                env.update(GBM_BACKENDS_PATH='/must-not-inherit',SESSION_EXPECT_GBM_BACKENDS_PATH='/opt/wddm-mesa/lib/gbm',
                           D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE='false',D3D12_EXPERIMENTAL_NATIVE_BUFFERS='false',SESSION_EXPECT_D3D12_INTEROP='1')
            return env
        def spawn(binary,arguments,env=None,pass_fds=(),root=False,groups=()):
            child=subprocess.Popen([str(binary),*arguments],env=env or base,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                                   text=True,pass_fds=pass_fds,user=0 if root else UID,group=0 if root else GID,extra_groups=list(groups))
            children.append(child);return child
        def finish(child,name,expected,timeout=15):
            try:stdout,stderr=child.communicate(timeout=timeout)
            except BaseException:
                child.kill();child.communicate();raise
            (output/(name+'.log')).write_text(stdout+stderr)
            assert child.returncode==expected,(name,child.returncode,stdout,stderr)
            results.append(dict(name=name,exit=child.returncode,gpuMock=True))
            return stdout,stderr
        def request(connection):
            data=connection.recv(128)
            assert len(data)==PACKET.size
            magic,version,size,status,reserved,nonce=PACKET.unpack(data)
            assert (magic,version,size,status,reserved)==(REQUEST,1,48,0,0)
            assert nonce!=bytes(32) and nonce not in nonces
            nonces.add(nonce);return [READY,1,48,0,0,nonce]
        # Information queries cannot depend on a runtime file or activation server.
        for root in (False,True):
            args=['--version-json'];child=spawn(wrapper,args,environment(args,query=True),root=root)
            out,_=finish(child,'query-root' if root else 'query-unprivileged',17)
            assert 'nativeBegins=0' in out
        # A real peer pidfd follows the exec without replacing the watchdog PID.
        write_config()
        with listen() as listener:
            gate_read,gate_write=os.pipe2(os.O_CLOEXEC)
            watchdog_parent,watchdog_child=socket.socketpair()
            args=['--watchdog-fd',str(watchdog_child.fileno()),'--safe-mode','--locked',*ARGS]
            env=environment(args);env.update(SESSION_TEST_PAUSE_FD=str(gate_read),SESSION_TEST_WATCHDOG_FD=str(watchdog_child.fileno()),SESSION_TEST_EXPECT_GROUPS='1')
            child=spawn(wrapper,args,env,pass_fds=(gate_read,watchdog_child.fileno()),groups=(65532,65533))
            process=-1
            try:
                connection,_=listener.accept()
                with connection:
                    peer_pid,peer_uid,peer_gid=struct.unpack('3i',connection.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
                    assert (peer_pid,peer_uid,peer_gid)==(child.pid,UID,GID)
                    process=connection.getsockopt(socket.SOL_SOCKET,77)
                    os.set_inheritable(process,False)
                    connection.sendall(PACKET.pack(*request(connection)))
                    assert select.select([child.stdout],[],[],5)[0]
                    identity=child.stdout.readline();assert f'pid={peer_pid} ' in identity,identity
                    assert os.readlink(f'/proc/{peer_pid}/exe')==str(target)
                    poller=select.poll();poller.register(process,select.POLLIN)
                    assert not poller.poll(0)
                    watchdog_parent.settimeout(2);assert watchdog_parent.recv(1)==b'W'
                    assert os.write(gate_write,b'G')==1
                    out,_=finish(child,'normal-exec-same-pid-and-watchdog',0)
                    assert 'frames=10000' in out and 'fdBalanced=true' in out
                    results[-1].update(sameKernelProcessThroughExec=True,rootHeldPeerPidfd=True,watchdogDescriptorPreserved=True,normalHomePreserved=True,supplementaryGroupsPreserved=True)
            finally:
                if child.poll() is None:child.kill();child.communicate()
                if process>=0:os.close(process)
                os.close(gate_write);os.close(gate_read);watchdog_child.close();watchdog_parent.close()
        # A bad runtime file must fail before any activation connection is made.
        invalid={
            'wrong-owner-uid':normal.replace(f'UID={UID}',f'UID={UID-1}'),
            'wrong-owner-gid':normal.replace(f'GID={GID}',f'GID={GID-1}'),
            'missing-owner':normal.replace(f'WDDM_SESSION_UID={UID}\n',''),
            'duplicate-version':normal+'WDDM_SESSION_VERSION=1\n',
            'duplicate-variable':normal+'LD_LIBRARY_PATH="/tmp"\n',
            'duplicate-gbm-backend-path':normal+'GBM_BACKENDS_PATH="/tmp"\n',
            'reject-home-override':normal+'HOME="/root"\n',
            'reject-unknown-variable':normal+'ARBITRARY="value"\n',
            'missing-newline':normal.rstrip('\n'),
            'embedded-null':normal.encode()+b'\0\n',
            'wrong-native-sync':normal.replace('NATIVE_SYNC_FILE="true"','NATIVE_SYNC_FILE="false"'),
            'missing-native-sync':normal.replace('D3D12_EXPERIMENTAL_NATIVE_SYNC_FILE="true"\n',''),
            'wrong-native-buffers':normal.replace('NATIVE_BUFFERS="true"','NATIVE_BUFFERS="false"'),
            'missing-native-buffers':normal.replace('D3D12_EXPERIMENTAL_NATIVE_BUFFERS="true"\n',''),
            'wrong-session-selection':normal.replace('COMPOSITOR_SESSION="1"','COMPOSITOR_SESSION="0"'),
            'wrong-version':normal.replace('VERSION=1','VERSION=2'),
            'control-character':normal.replace('/usr/lib/x86_64-linux-gnu','/usr/lib/\x1f'),
            'empty-file':'','oversize-file':'x'*16384,
        }
        for name,data in invalid.items():
            write_config(data)
            with listen() as listener:
                child=spawn(wrapper,ARGS,environment(ARGS));finish(child,name,125,timeout=2)
                listener.setblocking(False)
                try:unexpected,_=listener.accept()
                except BlockingIOError:pass
                else:unexpected.close();raise AssertionError('Invalid file reached activation')
        for name in ('group-writable-file','untrusted-owner-file','symlink-file','hardlinked-file'):
            write_config(mode=0o664 if name=='group-writable-file' else 0o644,owner=UID if name=='untrusted-owner-file' else 0)
            other=temporary/'other.env'
            if other.exists():other.unlink()
            if name=='symlink-file':config.rename(other);config.symlink_to(other)
            if name=='hardlinked-file':os.link(config,other)
            with listen() as listener:
                child=spawn(wrapper,ARGS,environment(ARGS));finish(child,name,125,timeout=2)
                listener.setblocking(False)
                try:unexpected,_=listener.accept()
                except BlockingIOError:pass
                else:unexpected.close();raise AssertionError('Untrusted metadata reached activation')
            if other.exists():other.unlink()
        write_config()
        child=spawn(wrapper,ARGS,environment(ARGS),root=True);finish(child,'root-render-rejected',125)
        # Exact shared client: malformed packets and SCM_RIGHTS must not leak FDs,
        # verified INSIDE the client before it exits, not by process destruction.
        for kind in ('valid','magic','version','size','status','reserved','nonce','short','long','rights','truncated-rights','eof','deadline'):
            with listen() as listener:
                child=spawn(client,[str(endpoint),'150' if kind=='deadline' else '3000','0' if kind=='valid' else '1'])
                connection,_=listener.accept()
                with connection:
                    values=request(connection)
                    if kind in ('magic','version','size','status','reserved'):
                        index={'magic':0,'version':1,'size':2,'status':3,'reserved':4}[kind];values[index]+=1
                    if kind=='nonce':values[-1]=bytes(32)
                    packet=PACKET.pack(*values)
                    if kind=='short':packet=packet[:-1]
                    if kind=='long':packet+=b'x'
                    if kind=='eof':connection.shutdown(socket.SHUT_RDWR)
                    elif kind=='deadline':pass
                    elif kind in ('rights','truncated-rights'):
                        read_fd,write_fd=os.pipe2(os.O_CLOEXEC)
                        try:connection.sendmsg([packet],[(socket.SOL_SOCKET,socket.SCM_RIGHTS,array.array('i',[write_fd]*(40 if kind=='truncated-rights' else 1)))])
                        finally:os.close(write_fd);os.close(read_fd)
                    else:connection.sendall(packet)
                    out,_=finish(child,'activation-'+kind,0)
                    assert 'fdBalancedBeforeExit=true' in out
        # The actual wrapper retains the production ten-second activation budget.
        with listen() as listener:
            child=spawn(wrapper,ARGS,environment(ARGS));started=time.monotonic()
            connection,_=listener.accept()
            with connection:
                request(connection)
                out,_=finish(child,'wrapper-production-deadline',125,timeout=13)
                elapsed=time.monotonic()-started
                assert 9<=elapsed<13 and 'frames=' not in out,elapsed
                results[-1]['elapsedSeconds']=elapsed
        # A separately owned root peer really exits; another peer has nonroot
        # credentials. Both must be rejected with every client FD retired.
        for mode in ('root-death','nonroot-peer'):
            sub=temporary/mode;sub.mkdir();sub.chmod(0o777)
            path=sub/'peer.sock';ready_read,ready_write=os.pipe2(os.O_CLOEXEC)
            server=os.fork()
            if not server:
                try:
                    os.close(ready_read)
                    if mode=='nonroot-peer':os.setgroups([]);os.setgid(GID);os.setuid(UID)
                    with listen(path) as listener:
                        os.write(ready_write,b'R');os.close(ready_write)
                        connection,_=listener.accept()
                        with connection:
                            if mode=='root-death':connection.recv(128)
                            else:connection.settimeout(2);connection.recv(128)
                    os._exit(0)
                except BaseException:os._exit(1)
            servers.append(server)
            os.close(ready_write)
            try:
                assert os.read(ready_read,1)==b'R'
                child=spawn(client,[str(path),'3000','1']);out,_=finish(child,'activation-'+mode,0)
                assert 'fdBalancedBeforeExit=true' in out
                assert os.waitpid(server,0)==(server,0)
            finally:os.close(ready_read)
        receipt=dict(controlsPassed=len(results),gpuMock=True,productionRootServiceImplemented=False,
                     realNormalLoginVerified=False,vmLaunched=False,hostInputSent=False,controls=results,
                     sourceSha256={name:hashlib.sha256((frozen/name).read_bytes()).hexdigest() for name in SOURCES},
                     strictC11Build=True,productionWrapperSha256=hashlib.sha256((output/'hyprland-session-exec').read_bytes()).hexdigest(),
                     compileCommands=commands)
        (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(json.dumps({key:receipt[key] for key in ('controlsPassed','gpuMock','productionRootServiceImplemented','realNormalLoginVerified')}))
    finally:
        for child in children:
            if child.poll() is None:child.kill()
            child.wait()
        for server in servers:
            try:
                ended,_=os.waitpid(server,os.WNOHANG)
                if not ended:os.kill(server,signal.SIGKILL);os.waitpid(server,0)
            except ChildProcessError:pass
        assert temporary.parent==Path(tempfile.gettempdir()) and temporary.name.startswith('omarchy-session-exec-')
        shutil.rmtree(temporary)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True);parser.add_argument('--cc',default='cc')
    args=parser.parse_args();run(args.output,args.cc)
