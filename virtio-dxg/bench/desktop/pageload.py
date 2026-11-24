"""pageload.py BROWSER URL [RUNS] [-- EXTRA_ARGS...]: start the browser with a
fresh profile and DevTools on a local port, load URL in a window, and report
(ms from navigation start) first contentful paint, largest contentful paint,
DOMContentLoaded and load, plus browser start to LCP. Works on Linux
(chromium) and Windows (msedge.exe / chrome.exe). Standard library only."""
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

PORT = 9333
if __name__ == '__main__':
    browser, url = sys.argv[1], sys.argv[2]
    runs = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3] != '--' else 3
    extra = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []


class WS:
    def __init__(self, ws_url):
        hostport, path = ws_url[len('ws://'):].split('/', 1)
        host, port = hostport.split(':')
        self.s = socket.create_connection((host, int(port)))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall((f'GET /{path} HTTP/1.1\r\nHost: {hostport}\r\nUpgrade: websocket\r\n'
                        f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                        'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            resp += self.s.recv(4096)
        self.buf = resp.split(b'\r\n\r\n', 1)[1]
        self.id = 0

    def _recv_exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        msg = b''
        while True:
            b0, b1 = self._recv_exact(2)
            n = b1 & 0x7f
            if n == 126:
                n = struct.unpack('>H', self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack('>Q', self._recv_exact(8))[0]
            msg += self._recv_exact(n)
            if b0 & 0x80:
                return json.loads(msg)

    def call(self, method, **params):
        self.id += 1
        data = json.dumps({'id': self.id, 'method': method, 'params': params}).encode()
        hdr = bytes([0x81])
        if len(data) < 126:
            hdr += bytes([0x80 | len(data)])
        elif len(data) < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack('>H', len(data))
        else:
            hdr += bytes([0x80 | 127]) + struct.pack('>Q', len(data))
        mask = os.urandom(4)
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
        while True:
            m = self.recv()
            if m.get('id') == self.id:
                return m.get('result', m.get('error'))


METRICS = '''(() => {
  const n = performance.getEntriesByType('navigation')[0];
  const p = performance.getEntriesByType('paint').find(e => e.name === 'first-contentful-paint');
  return JSON.stringify({ready: document.readyState,
    ttfb: n && n.responseStart, dcl: n && n.domContentLoadedEventEnd,
    load: n && n.loadEventEnd, fcp: p && p.startTime, lcp: window.__lcp || null});
})()'''
LCP_OBS = '''new PerformanceObserver(l => { const e = l.getEntries(); window.__lcp = e[e.length - 1].startTime; })
  .observe({type: 'largest-contentful-paint', buffered: true}); 1'''


def one():
    prof = tempfile.mkdtemp(prefix='pl-')
    t0 = time.monotonic()
    p = subprocess.Popen([browser, f'--remote-debugging-port={PORT}', f'--user-data-dir={prof}',
                          '--no-first-run', '--no-default-browser-check', '--new-window',
                          *extra, 'about:blank'],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(200):
            try:
                tabs = json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json'))
                tab = next(t for t in tabs if t.get('type') == 'page')
                break
            except Exception:
                time.sleep(0.05)
        ws = WS(tab['webSocketDebuggerUrl'])
        t_ready = time.monotonic()
        ws.call('Page.enable')
        ws.call('Page.addScriptToEvaluateOnNewDocument', source=LCP_OBS)
        t_nav = time.monotonic()
        ws.call('Page.navigate', url=url)
        m = {}
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            r = ws.call('Runtime.evaluate', expression=METRICS)
            try:
                m = json.loads(r['result']['value'])
            except Exception:
                m = {}
            if m.get('load') and m.get('lcp') and time.monotonic() - t_nav > 1.0:
                time.sleep(1.0)    # LCP can still move after load
                m = json.loads(ws.call('Runtime.evaluate', expression=METRICS)['result']['value'])
                break
            time.sleep(0.1)
        startup = (t_ready - t0) * 1000
        lcp_total = startup + (t_nav - t_ready) * 1000 + (m.get('lcp') or 0)
        return startup, m, lcp_total
    finally:
        p.terminate()
        try:
            p.wait(timeout=10)
        except subprocess.TimeoutExpired:
            p.kill()
        time.sleep(1)
        shutil.rmtree(prof, ignore_errors=True)


def f(v):
    return '-' if v is None else f'{v:.0f}'


if __name__ == '__main__':
    for i in range(runs):
        startup, m, total = one()
        print(f'run {i + 1}: browser start {startup:.0f} ms | ttfb {f(m.get("ttfb"))} fcp {f(m.get("fcp"))} '
              f'dcl {f(m.get("dcl"))} lcp {f(m.get("lcp"))} load {f(m.get("load"))} ms | '
              f'start to lcp {total:.0f} ms', flush=True)
