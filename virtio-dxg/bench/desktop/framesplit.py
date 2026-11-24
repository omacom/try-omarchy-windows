"""framesplit.py [LOG] [SURFACE]: from a client's WAYLAND_DEBUG log (default
/tmp/navscroll.log, the surface with the most commits): per frame, the
client's part (frame callback done -> next commit) and the compositor's part
(commit -> the done of the callback requested with it)."""
import collections
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/navscroll.log'
lines = open(path, errors='replace').read().splitlines()


def ts(line):
    m = re.match(r'\[(\d+):(\d+):([\d.]+)\]', line)
    return (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3))) * 1000 if m else None


commits = collections.Counter(re.findall(r'-> wl_surface#(\d+)\.commit\(\)', '\n'.join(lines)))
sid = sys.argv[2] if len(sys.argv) > 2 else commits.most_common(1)[0][0]
pending = None          # callback id requested for the next commit
cb_commit = {}          # callback id -> commit time
events = []             # (kind, t)
for line in lines:
    t = ts(line)
    m = re.search(rf'-> wl_surface#{sid}\.frame\(new id wl_callback#(\d+)\)', line)
    if m:
        pending = m.group(1)
        continue
    if re.search(rf'-> wl_surface#{sid}\.commit\(\)', line):
        events.append(('commit', t))
        if pending:
            cb_commit[pending] = t
            pending = None
        continue
    m = re.search(r'wl_callback#(\d+)\.done\(', line)
    if m and m.group(1) in cb_commit:
        events.append(('done', t, cb_commit.pop(m.group(1))))
client, comp = [], []
last_done = None
for e in events:
    if e[0] == 'done':
        comp.append(e[1] - e[2])
        last_done = e[1]
    elif e[0] == 'commit' and last_done is not None:
        client.append(e[1] - last_done)
        last_done = None


def stat(v):
    v = sorted(v)
    if not v:
        return '-'
    q = lambda p: v[min(len(v) - 1, int(p * len(v)))]
    return f'n {len(v):4d}  p10 {q(.1):5.1f}  p50 {q(.5):5.1f}  p90 {q(.9):5.1f}  mean {sum(v) / len(v):5.1f} ms'


print(f'surface {sid}')
print(f'client  (done -> commit):   {stat(client)}')
print(f'compositor (commit -> done): {stat(comp)}')
