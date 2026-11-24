"""webapp.py BROWSER URL [RUNS] [--launcher CMD]: a web app window (--app=URL)
as Omarchy's menu opens it, timed from launch: window mapped (Hyprland, Linux
only), first contentful paint, largest contentful paint, load. Uses its own
profile (kept between runs, so runs after the first see warm caches as a
user does) with DevTools on a local port. Works on Windows too (Chrome/Edge),
without the window time. Standard library only.

--launcher CMD: start through CMD (e.g. omarchy-launch-webapp, which goes
through uwsm-app) instead of the browser binary."""
import json
import os
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pageload import WS  # noqa: E402  (pageload.py's argv parsing is guarded)

browser, url = sys.argv[1], sys.argv[2]
runs = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else 3
launcher = sys.argv[sys.argv.index('--launcher') + 1] if '--launcher' in sys.argv else None
PORT = 9334
PROF = os.path.join(tempfile.gettempdir(), 'webapp-prof')
LINUX = sys.platform.startswith('linux')

METRICS = '''(() => {
  if (!window.__obs) { window.__obs = 1;
    new PerformanceObserver(l => { const e = l.getEntries(); window.__lcp = e[e.length - 1].startTime; })
      .observe({type: 'largest-contentful-paint', buffered: true}); }
  const n = performance.getEntriesByType('navigation')[0];
  const p = performance.getEntriesByType('paint').find(e => e.name === 'first-contentful-paint');
  return JSON.stringify({origin: performance.timeOrigin, fcp: p && p.startTime,
    lcp: window.__lcp || null, load: n && n.loadEventEnd, dcl: n && n.domContentLoadedEventEnd});
})()'''


def hypr_socket():
    if not LINUX or 'HYPRLAND_INSTANCE_SIGNATURE' not in os.environ:
        return None
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(os.path.join(os.environ['XDG_RUNTIME_DIR'], 'hypr',
                           os.environ['HYPRLAND_INSTANCE_SIGNATURE'], '.socket2.sock'))
    s.setblocking(False)
    return s


def browser_gone():
    if LINUX:
        return subprocess.run(['pgrep', '-f', 'webapp-prof'], capture_output=True).returncode != 0
    out = subprocess.run(['powershell', '-NoProfile', '-Command',
                          "(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*webapp-prof*' }).Count"],
                         capture_output=True, text=True).stdout.strip()
    return out in ('', '0')


RESIDENT = os.environ.get('RESIDENT', '')
seen_targets = set()


def targets():
    return json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json', timeout=0.5))


def start_resident():
    # A browser kept running without windows (as Edge's startup boost does);
    # launches below hand their window to it.
    subprocess.Popen([browser, f'--user-data-dir={PROF}', f'--remote-debugging-port={PORT}',
                      '--no-first-run', '--no-default-browser-check', '--no-startup-window']
                     + shlex.split(RESIDENT if RESIDENT not in ('1',) else ''),
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(100):
        try:
            seen_targets.update(t['id'] for t in targets())
            return
        except Exception:
            time.sleep(0.1)


def one():
    hs = hypr_socket()
    args = [f'--user-data-dir={PROF}', f'--remote-debugging-port={PORT}', '--no-first-run',
            '--no-default-browser-check', f'--app={url}'] + shlex.split(os.environ.get('WEBAPP_ARGS', ''))
    if launcher:
        cmd = shlex.split(launcher) + [url] + [a for a in args if not a.startswith('--app=')]
    else:
        cmd = [browser] + args
    t0 = time.time()
    subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)
    t_win, addr, buf, ws, m = None, None, b'', None, {}
    deadline = time.time() + 40
    while time.time() < deadline:
        if hs and t_win is None:
            try:
                buf += hs.recv(65536)
            except BlockingIOError:
                pass
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                if line.startswith(b'openwindow>>') and t_win is None:
                    f = line[12:].decode(errors='replace').split(',', 3)
                    if len(f) >= 3 and ('youtube' in f[2].lower() or 'chrom' in f[2].lower()):
                        t_win, addr = time.time(), f[0]
        if ws is None:
            try:
                tabs = targets()
                tab = next(t for t in tabs if t.get('type') == 'page' and t['id'] not in seen_targets
                           and url.split('//')[1].split('/')[0] in t.get('url', ''))
                seen_targets.add(tab['id'])
                ws = WS(tab['webSocketDebuggerUrl'])
            except Exception:
                time.sleep(0.02)
                continue
        try:
            m = json.loads(ws.call('Runtime.evaluate', expression=METRICS)['result']['value'])
        except Exception:
            m = {}
        if m.get('load') and m.get('lcp') and (t_win or not hs):
            time.sleep(1.5)
            m = json.loads(ws.call('Runtime.evaluate', expression=METRICS)['result']['value'])
            break
        time.sleep(0.05)
    if ws and RESIDENT:
        try:
            ws.call('Page.close')
        except Exception:
            pass
        time.sleep(1.5)
    elif ws:
        try:
            ws.call('Browser.close')
        except Exception:
            pass
    for _ in range(0 if RESIDENT else 100):
        if browser_gone():
            break
        time.sleep(0.1)
    time.sleep(1)
    o = (m.get('origin') or t0 * 1000) - t0 * 1000

    def rel(k):
        return '-' if m.get(k) is None else f'{o + m[k]:.0f}'
    win = '-' if t_win is None else f'{(t_win - t0) * 1000:.0f}'
    return (f'window {win} | page start {o:.0f} fcp {rel("fcp")} lcp {rel("lcp")} '
            f'dcl {rel("dcl")} load {rel("load")} ms after launch')


if '--fresh' in sys.argv:
    import shutil
    shutil.rmtree(PROF, ignore_errors=True)
if RESIDENT:
    start_resident()
    time.sleep(float(os.environ.get('RESIDENT_SETTLE', '3')))
for i in range(runs):
    print(f'run {i + 1}: {one()}', flush=True)
if RESIDENT:
    try:
        b = json.load(urllib.request.urlopen(f'http://127.0.0.1:{PORT}/json/version', timeout=2))
        WS(b['webSocketDebuggerUrl']).call('Browser.close')
    except Exception:
        pass
