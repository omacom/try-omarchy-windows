"""Exercise refused, late and repeated socket connections in paused, disk-free QEMU."""
import argparse
import io
import json
from pathlib import Path
import socket
import threading
import time
from test_section import Qmp


def verify(qemu, firmware):
    # Bind without listening: every early connection is refused, and another
    # process cannot steal the port before this same socket becomes a listener.
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    listener.bind(('127.0.0.1', 0))
    listener.settimeout(5)
    port = listener.getsockname()[1]
    label = 'owned-reconnect-probe'
    command = [str(qemu.resolve()), '-L', str(firmware.resolve()),
        '-machine', 'q35,accel=tcg', '-m', '64M', '-nodefaults',
        '-display', 'none', '-monitor', 'none', '-serial', 'none', '-S', '-qmp', 'stdio',
        '-chardev', f'socket,id={label},host=127.0.0.1,port={port},reconnect-ms=100',
        # A control monitor keeps its frontend open without a guest driver.
        # A paused virtserialport has no guest consumer and cannot certify
        # ordinary disconnect notification in this display-free test.
        '-mon', f'chardev={label},mode=control']
    monitor = None
    stderr = reader = None
    refused = threading.Event()
    errors = []
    connections = []
    observations = []
    try:
        monitor = Qmp(command)
        stderr = monitor.process.stderr
        # One owner drains this pipe. Qmp.close still reaps the process and
        # stdout reader, but must not compete with the diagnostic reader.
        monitor.process.stderr = io.StringIO()

        def read_errors():
            for line in stderr:
                if len(errors) < 16:
                    errors.append(line.strip())
                if f'Unable to connect character device {label}:' in line:
                    refused.set()

        reader = threading.Thread(target=read_errors, daemon=True)
        reader.start()

        def connected():
            return monitor.call('qom-get', {'path': '/chardevs/'+label, 'property': 'connected'})

        def wait_connected(expected):
            deadline = time.monotonic()+5
            while True:
                state = connected()
                if type(state) is not bool:
                    raise RuntimeError('Socket did not expose its actual connected state')
                if state == expected:
                    return
                if time.monotonic() >= deadline:
                    raise TimeoutError('Socket connection state did not converge')
                time.sleep(0.01)

        def accept_greeting():
            connection, _ = listener.accept()
            connections.append(connection)
            connection.settimeout(5)
            greeting = bytearray()
            while not greeting.endswith(b'\n'):
                byte = connection.recv(1)
                if not byte or len(greeting) >= 65536:
                    raise RuntimeError('Reconnect did not deliver a bounded QMP greeting')
                greeting.extend(byte)
            if not isinstance(json.loads(greeting).get('QMP'), dict):
                raise RuntimeError('Reconnect did not deliver a new QMP monitor')
            wait_connected(True)
            return connection

        # Wait for the actual refused-connect callback, not just a timer:
        # Winsock can keep connect() pending longer than reconnect-ms.
        wait_connected(False)
        deadline = time.monotonic()+10
        while not refused.wait(0.05):
            if monitor.process.poll() is not None:
                raise RuntimeError('QEMU exited before reporting its refused connection')
            if time.monotonic() >= deadline:
                raise TimeoutError('No actual refused connection callback observed')
        if connected() is not False or monitor.call('query-status')['status'] != 'prelaunch':
            raise RuntimeError('QEMU did not survive the refused asynchronous connection')
        observations.append('refused connection survives with QMP responsive')
        listener.listen(4)
        connection = accept_greeting()
        observations.append('late listener connects')
        instance = {'type': 'chardev', 'id': label}
        if instance not in monitor.call('query-yank'):
            raise RuntimeError('Connected chardev lacks its yank instance')

        # A real peer disconnect exercises the normal unregister path. The
        # following accepted connection witnesses a new reconnect, not a
        # cached success flag from the original socket.
        connection.shutdown(socket.SHUT_RDWR)
        connection.close()
        connection = accept_greeting()
        observations.append('peer disconnect reconnects')

        # The registered cancellation function must still work after the
        # backport. Yank only this test's socket, then accept its next retry.
        monitor.call('yank', {'instances': [instance]})
        connection.settimeout(5)
        if connection.recv(1) != b'':
            raise RuntimeError('Yank did not shut down the owned peer connection')
        connection.close()
        connection = accept_greeting()
        if instance not in monitor.call('query-yank'):
            raise RuntimeError('Reconnect lost its yank instance')
        observations.append('yank disconnect reconnects')
        monitor.call('quit')
        monitor.process.wait(timeout=10)
        if monitor.process.returncode:
            raise RuntimeError('Reconnect probe did not exit naturally with status zero')
        return dict(schema=1, display='none', sourceDiskAttached=False,
            guestBooted=False, hostInputSent=False, privateLoopbackOnly=True,
            refusedConnectionMessageObserved=True,
            frontend='qmp-monitor', greetingsObserved=len(connections),
            connectionCount=len(connections), observations=observations,
            naturalQemuExit=0, normalLauncherBootVerified=False,
            desktopRenderingVerified=False, dailyUseAccepted=False)
    except BaseException as error:
        code = monitor.process.poll() if monitor is not None else None
        raise RuntimeError(f'Reconnect probe failed; qemuExitBeforeCleanup={code}; stderr={errors}') from error
    finally:
        for connection in connections:
            connection.close()
        listener.close()
        if monitor is not None:
            monitor.close()
        if reader is not None:
            reader.join(timeout=5)
            if reader.is_alive():
                raise RuntimeError('Reconnect diagnostic reader did not finish after QEMU exit')
            stderr.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    proof = verify(args.qemu, args.firmware)
    with args.report.open('x', encoding='utf-8') as output:
        json.dump(proof, output, indent=2)
        output.write('\n')
    print('PASS: refused connection, late listener, peer/yank reconnect and natural exit; no desktop acceptance')
