"""qmpkeys: key presses into the VM through QEMU's QMP (reachable from the
guest at the slirp host address), as a user would type them. QMP_PORT
picks another VM's QMP port (default 4444)."""
import json
import os
import socket

QMP = ('10.0.2.2', int(os.environ.get('QMP_PORT', '4444')))
NAMES = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash', '\n': 'ret'}


class Keys:
    def __init__(self, addr=QMP):
        self.s = socket.create_connection(addr, timeout=5)
        self.f = self.s.makefile('rwb')
        self._read()                       # greeting
        self.cmd('qmp_capabilities')

    def _read(self):
        while True:
            m = json.loads(self.f.readline())
            if 'event' not in m:
                return m

    def cmd(self, name, **args):
        self.f.write(json.dumps({'execute': name, 'arguments': args}).encode() + b'\n')
        self.f.flush()
        reply = self._read()
        if 'return' not in reply:
            raise RuntimeError(f'QMP {name} failed: {reply}')
        return reply

    def combo(self, *keys, hold=30):
        return self.cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in keys],
                        **{'hold-time': hold})

    def type(self, text, hold=15):
        for c in text.lower():
            k = NAMES.get(c, c)
            if len(k) == 1 and not k.isalnum():
                continue
            self.combo(k, hold=hold)
