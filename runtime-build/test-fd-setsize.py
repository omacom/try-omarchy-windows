#!/usr/bin/env python3
"""Poll a socket past the 64th through select() with the pinned FD_SETSIZE."""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
args = parser.parse_args()
if sys.platform != "win32":
    parser.error("This test requires native Windows")
meson = (args.source / "meson.build").read_text()
start = meson.index("elif host_os == 'windows'")
end = meson.index("\nendif", start)
matches = re.findall(r"^\s*qemu_common_flags \+= '-DFD_SETSIZE=(\d+)'\s*$",
                     meson[start:end], re.MULTILINE)
assert len(matches) == 1, "QEMU's Windows flags must set FD_SETSIZE exactly once"
size = int(matches[0])
sockets = 100
assert size >= sockets, f"FD_SETSIZE {size} is below the {sockets} sockets tested"

fixture = r'''
#include <winsock2.h>
#include <stdio.h>
#define SOCKETS 100
int main(void) {
    WSADATA data;
    SOCKET s[SOCKETS];
    struct sockaddr_in addr = {0};
    int len = sizeof(addr);
    fd_set rfds;
    struct timeval tv = {2, 0};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 2;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < SOCKETS; i++) {
        s[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (s[i] == INVALID_SOCKET) return 2;
        addr.sin_port = 0;
        if (bind(s[i], (struct sockaddr *)&addr, sizeof(addr))) return 2;
    }
    if (getsockname(s[SOCKETS - 1], (struct sockaddr *)&addr, &len)) return 2;
    if (sendto(s[0], "x", 1, 0, (struct sockaddr *)&addr, sizeof(addr)) != 1) return 2;
    FD_ZERO(&rfds);
    for (int i = 0; i < SOCKETS; i++) FD_SET(s[i], &rfds);
    if (select(0, &rfds, NULL, NULL, &tv) == SOCKET_ERROR) return 2;
    return FD_ISSET(s[SOCKETS - 1], &rfds) ? 0 : 1;
}
'''


def run(define):
    with tempfile.TemporaryDirectory() as temp:
        c = Path(temp) / "fd-setsize.c"
        exe = Path(temp) / "fd-setsize.exe"
        c.write_text(fixture)
        subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", *define,
                        str(c), "-o", str(exe), "-lws2_32"], check=True)
        return subprocess.run([str(exe)]).returncode


assert run([]) == 1, "the default fd_set unexpectedly polled the 100th socket"
assert run([f"-DFD_SETSIZE={size}"]) == 0, "select() missed a ready socket past the 64th"
print(f"ok - select() with FD_SETSIZE={size} sees a ready socket past the 64th")
