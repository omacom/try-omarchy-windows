"""Decode a complete live trace and preserve its explicit graphics-session boundary."""
import hashlib
import re

MAXIMUM=64*1024*1024
HEADER=f'WDDM_VIRTIO_TRACE_BEGIN version=1 maxBytes={MAXIMUM}\n'.encode('ascii')


def decode_trace(raw):
    if not raw.startswith(HEADER) or len(raw)>MAXIMUM+512:raise ValueError('Invalid/bounded virtio trace header')
    end=re.search(rb'WDDM_VIRTIO_TRACE_END bytes=(0|[1-9][0-9]*) fnv1a=([0-9a-f]{16}) complete=true\n\Z',raw)
    if not end:raise ValueError('Incomplete virtio trace')
    payload=raw[len(HEADER):end.start()]
    if not payload or len(payload)!=int(end[1]) or len(payload)>MAXIMUM:raise ValueError('Virtio trace byte count differs')
    value=14695981039346656037
    for byte in payload:value=((value^byte)*1099511628211)&0xffffffffffffffff
    if value!=int(end[2],16):raise ValueError('Virtio trace checksum differs')
    return payload,dict(payloadBytes=len(payload),fnv1a=f'{value:016x}',rawTraceSha256=hashlib.sha256(raw).hexdigest(),complete=True)


def merge_session(uart_raw,trace_raw):
    payload,proof=decode_trace(trace_raw)
    uart=uart_raw.decode('utf-8',errors='replace').replace('\r\n','\n')
    begin=list(re.finditer(r'NORMAL_ROOT_VIRTIO_TRACE_BEGIN port=vport0p2 namedIdentityVerified=true liveHostCapture=true inputChanged=false gpuProtocolChanged=false uid=1000\n',uart))
    end=list(re.finditer(r'NORMAL_ROOT_VIRTIO_TRACE_END pumpExited=true complete=true inputChanged=false\n',uart))
    if len(begin)!=1 or len(end)!=1 or begin[0].end()>end[0].start():raise ValueError('Missing/reordered live trace session boundaries')
    # All graphics actors, controller and broker share the ordered FIFO.
    # Ambient UART records remain intact; this is not a total clock ordering
    # of unrelated kernel/systemd messages across two carriers.
    text=(uart[:begin[0].end()]+payload.decode('utf-8',errors='replace').replace('\r\n','\n')+
          '\n'+uart[begin[0].end():end[0].start()]+uart[end[0].start():])
    return text,dict(proof,uartSha256=hashlib.sha256(uart_raw).hexdigest(),
                     wholeGraphicsSession=True,ambientUartTotalOrderingVerified=False,
                     inputImplementationChanged=False,gpuProtocolChanged=False)
