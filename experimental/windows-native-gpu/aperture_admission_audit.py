"""Decode guest-kernel snapshots taken at the actual failed lease admission."""
import re
from verify_hyprland_lifecycle_capture import require


def admission_snapshots(text):
    pattern=(r'WDDM_APERTURE_ADMISSION_REJECTED kind=(\d+) requestedBytes=(\d+) activeLeases=(\d+) '
             r'leaseLimit=(\d+) activeCpuBytes=(\d+) cpuByteLimit=(\d+) leaseExhausted=([01]) '
             r'cpuBytesExceeded=([01]) cookieExhausted=([01]) atomicSnapshot=true')
    fields=('kind','requestedBytes','activeLeases','leaseLimit','activeCpuBytes','cpuByteLimit',
            'leaseExhausted','cpuBytesExceeded','cookieExhausted')
    rows=[]
    for match in re.finditer(pattern,text):
        row=dict(zip(fields,map(int,match.groups())))
        require(row['kind'] in (1,2) and row['requestedBytes']>0 and row['requestedBytes']%4096==0 and
                0<=row['activeLeases']<=row['leaseLimit'] and row['leaseLimit'] in (256,512,768,1536) and
                0<=row['activeCpuBytes']<=row['cpuByteLimit'] and row['cpuByteLimit'] in (100663296,201326592),
                'Invalid atomic aperture admission snapshot')
        require(row['leaseExhausted']==int(row['activeLeases']>=row['leaseLimit']) and
                row['cpuBytesExceeded']==int(row['kind']==2 and row['requestedBytes']>row['cpuByteLimit']-row['activeCpuBytes']) and
                any(row[k] for k in ('leaseExhausted','cpuBytesExceeded','cookieExhausted')),
                'Atomic admission flags contradict the recorded limits')
        rows.append(row)
    require(len(rows)==text.count('WDDM_APERTURE_ADMISSION_REJECTED '),
            'Missing, malformed or non-atomic aperture admission record')
    return rows
