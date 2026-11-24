"""menuprobe: press SUPER+SPACE and print the compositor events of the next
1.5 s with their times, then Escape and print again."""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmpkeys import Keys  # noqa: E402

hs = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
hs.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'hypr',
                        os.environ['HYPRLAND_INSTANCE_SIGNATURE'], '.socket2.sock'))
hs.setblocking(False)
k = Keys()


def events(t0, secs):
    end = time.time() + secs
    buf = b''
    while time.time() < end:
        try:
            buf += hs.recv(65536)
        except BlockingIOError:
            time.sleep(0.002)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            print(f'{(time.time() - t0) * 1000:7.1f} ms  {line.decode(errors="replace")[:120]}')


t0 = time.time()
k.combo('meta_l', 'spc')
events(t0, 1.5)
print('-- escape')
t0 = time.time()
k.combo('esc')
events(t0, 1.0)
