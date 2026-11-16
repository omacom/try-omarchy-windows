"""Exercise diagnostic transport failures without WSL or NVIDIA hardware."""
import argparse
import os
import pathlib
import subprocess
import tempfile


WORKER = r'''#!/usr/bin/env python3
import os, struct, sys
def get(n):
    data = sys.stdin.buffer.read(n)
    if len(data) != n:
        raise EOFError
    return data
def send(op, handle, value=0, data=b'', padding=0, hello=False, ntstatus=0):
    packet = struct.pack('<IIiI',op,handle,0,padding)
    caps = 103 | (128 if mode.startswith('paging-') and mode != 'paging-disabled' else 0)
    if mode == 'priority-disabled': caps &= ~32
    caps |= 536870912 if mode.startswith('consume-') and mode != 'consume-disabled' else 0
    caps |= 256 if mode.startswith('allocation-') and mode != 'allocation-disabled' else 0
    caps |= 256 if mode.startswith('resource-') else 0
    caps |= 524288 if mode.startswith('resource-') and mode != 'resource-disabled' else 0
    caps |= 256 | 524288 | 268435456 if mode.startswith('native-resource-') else 0
    caps |= 1073741824 if mode.startswith('native-resource-') and mode != 'native-resource-disabled' else 0
    caps |= 256 | 524288 if mode.startswith('shared-resource-') else 0
    caps |= 268435456 if mode.startswith('shared-resource-') and mode != 'shared-resource-disabled' else 0
    caps |= 1048576 if mode.startswith('context-signal-') and mode != 'context-signal-disabled' else 0
    caps |= 256 if mode.startswith('gpuva-') else 0
    caps |= 512 if mode == 'gpuva-invalid' else 0
    caps |= 1024 if mode == 'resident-invalid' else 0
    caps |= 256 if mode.startswith('cpu-') else 0
    caps |= 2048 if mode.startswith('cpu-') and mode != 'cpu-disabled' else 0
    caps |= 256 if mode.startswith('translation-') else 0
    caps |= 4096 if mode.startswith('translation-') and mode != 'translation-disabled' else 0
    caps |= 8192 if mode.startswith('hwqueue-') and mode != 'hwqueue-disabled' else 0
    caps |= 2097152 if mode.startswith('hwqueue-nobroadcast-') and mode != 'hwqueue-nobroadcast-disabled' else 0
    caps |= 2097152 if mode.startswith('hwqueue-internal-') and mode != 'hwqueue-internal-missing-signal' else 0
    caps |= 16777216 if mode.startswith('hwqueue-internal-') and mode != 'hwqueue-internal-disabled' else 0
    caps |= 16384 if mode.startswith('sync-') and mode != 'sync-disabled' else 0
    caps |= 32768 if mode.startswith('submit-') and mode != 'submit-disabled' else 0
    caps |= 134217728 if mode.startswith('hw-signal-') and mode != 'hw-signal-disabled' else 0
    caps |= 8192 | 16384 | 33554432 if mode.startswith('hw-wait-') and mode != 'hw-wait-disabled' else 0
    caps |= 32 | 16384 | 33554432 if mode.startswith('context-wait-') and mode != 'context-wait-disabled' else 0
    caps |= 131072 if mode.startswith('reservation-') and mode != 'reservation-disabled' else 0
    caps |= 512 if mode.startswith('gpu-state-') else 0
    caps |= 262144 if mode.startswith('gpu-state-') and mode != 'gpu-state-disabled' else 0
    packet += struct.pack('<IIII',1,caps,4318,11352) if hello else struct.pack('<iIQ',ntstatus,0,value) + data
    sys.stdout.buffer.write(struct.pack('<I',len(packet)) + packet)
    sys.stdout.buffer.flush()
mode = os.environ['BRIDGE_FAULT_TEST']
operations = []
try:
    while True:
        size, = struct.unpack('<I',get(4))
        packet = get(size)
        op, handle, status, padding = struct.unpack_from('<IIiI',packet)
        assert not status and not padding
        operations.append(op)
        if op == 0x2000:
            if mode == 'wrong-version':
                raw = struct.pack('<IIiIIIII',op,0,0,0,99,103,4318,11352)
                sys.stdout.buffer.write(struct.pack('<I',len(raw)) + raw); sys.stdout.buffer.flush()
            else:
                send(op,0,hello=True)
            if mode == 'closed-worker': break
        elif mode == 'bad-reply': send(op,1,padding=1)
        elif op == 0x2001:
            send(op,0,ntstatus=-1073741801) if mode == 'errno-log-failure' else send(op,1)
        elif op == 0x2003: send(op,handle)
        elif op == 0x2004: send(op,2)
        elif op == 0x2005: send(op,handle)
        elif op == 0x20b0:
            assert handle == 2 and len(packet) == 16
            if mode == 'device-state-unsupported-peer':
                raw=struct.pack('<IIiI',op,handle,-95,0)
                sys.stdout.buffer.write(struct.pack('<I',len(raw))+raw);sys.stdout.buffer.flush()
            else:
                state=5 if mode=='device-state-error-state' else 0 if mode=='device-state-zero' else 1
                status=(0xc00002b6-(1<<32)) if mode in ('device-state-removed','device-state-bad-error') else \
                    -1073741801 if mode=='device-state-oom' else (0xc00000bb-(1<<32)) if mode=='device-state-unsupported-native' else \
                    259 if mode=='device-state-positive' else 0
                if status<0 and mode!='device-state-bad-error':state=0
                send(op,99 if mode=='device-state-bad-handle' else handle,value=state,
                     ntstatus=status,data=b'x' if mode=='device-state-long' else b'')
        elif op == 0x2022:
            assert handle == 3 and packet[16:] == struct.pack('<i',1)
            send(op,99 if mode == 'priority-bad-id' else handle,
                 value=1 if mode == 'priority-bad-value' else 0,
                 data=b'X' if mode == 'priority-long' else b'',
                 ntstatus=-1073741811 if mode == 'priority-nt-failure' else 259 if mode == 'priority-bad-status' else 0)
        elif op == 0x2080:
            assert handle == 1 and len(packet) == 48
            base, minimum, maximum, size = struct.unpack_from('<4Q',packet,16)
            assert base == 0 and minimum == 67108864 and maximum == 1 << 40
            assert size == (4 << 30 if mode == 'reservation-byte-quota' else 65536)
            count = operations.count(op)
            failed = mode in ('reservation-nt-failure','reservation-bad-failure')
            identity = 0 if failed or mode == 'reservation-bad-id' else count + 1
            address = minimum + count * (4 << 30)
            if failed or mode == 'reservation-zero': address = 0
            if mode == 'reservation-alignment': address += 1
            if mode == 'reservation-range': address = 1 << 48
            if mode == 'reservation-minimum': address = minimum - 65536
            if mode == 'reservation-maximum': address = maximum
            if mode == 'reservation-bad-failure': address = minimum
            if count == 2 and mode == 'reservation-duplicate': identity = 2
            if count == 2 and mode == 'reservation-reply-overlap': address = minimum + (4 << 30)
            send(op,identity,value=address,data=b'X' if mode == 'reservation-long' else b'',
                 ntstatus=-1073741811 if failed else 259 if mode == 'reservation-bad-status' else 0)
        elif op == 0x2081:
            assert handle >= 2 and packet[16:] == struct.pack('<II',1,0)
            failed = mode == 'reservation-failed-free' and operations.count(op) == 1
            send(op,0 if mode == 'reservation-free-id' else handle,
                 value=1 if mode == 'reservation-free-value' else 0,
                 data=b'X' if mode == 'reservation-free-long' else b'',
                 ntstatus=-1073741811 if failed else 259 if mode == 'reservation-free-status' else 0)
        elif op == 0x2070:
            mutex = mode in ('sync-mutex', 'sync-failed-destroy', 'sync-mutex-bad-map') or mode.startswith('sync-destroy-')
            no_gpu = mode.startswith(('sync-nogpu-', 'sync-ntshared-'))
            no_max = mode.startswith('sync-nomax-')
            flags = (1155 if '-lifetime-' in mode else 131) if mode.startswith('sync-ntshared-') else 128 if no_gpu else 64 if no_max else 0
            assert handle == 2 and packet[16:] == struct.pack('<IIIIQ',1 if mutex else 5,flags,0 if mutex else 1,0,1 if mutex else 42)
            failed = mode in ('sync-nt-failure','sync-bad-failure')
            identity = 0 if failed or mode == 'sync-bad-id' else 3
            offset = 0 if failed or mutex else 1 if mode == 'sync-bad-alignment' else 1048576 if mode == 'sync-bad-offset' else 8192
            gpu = 0 if failed or mutex or mode in ('sync-zero-gpu','sync-nomax-zero-gpu') else 65537 if mode == 'sync-bad-gpu-alignment' else 1 << 48 if mode == 'sync-bad-gpu-range' else 65536
            if no_gpu: gpu = 65536 if mode.endswith('-nonzero-gpu') else 0
            if mode == 'sync-nogpu-bad-offset': offset = 1048576
            if mode in ('sync-bad-failure', 'sync-mutex-bad-map'): offset = 8192
            data = struct.pack('<QQ',offset,gpu)
            if mode == 'sync-short': data = data[:-1]
            send(op,identity,value=1 if mode == 'sync-bad-value' else 0,data=data,
                 ntstatus=-1073741811 if failed else 259 if mode == 'sync-bad-status' else 0)
        elif op == 0x2071:
            assert handle == 3 and len(packet) == 16
            failed = mode == 'sync-failed-destroy' and operations.count(op) == 1
            send(op,0 if mode == 'sync-destroy-bad-id' else handle,
                 value=1 if mode == 'sync-destroy-bad-value' else 0,
                 data=b'X' if mode == 'sync-destroy-long' else b'',
                 ntstatus=-1073741811 if failed else 259 if mode == 'sync-destroy-bad-status' else 0)
        elif op == 0x2020:
            assert handle == 2
            desc = struct.unpack_from('<6I',packet,16)
            assert desc == ((0,0,8,0,0,0) if mode == 'hwqueue-sync-context' else (0,1,16,16 if mode == 'glon12-normal' else 12,4,0))
            send(op,3,data=b'' if mode == 'hwqueue-sync-context' else bytes([37 ^ 255,38,39,40]))
        elif op == 0x2021:
            assert mode == 'glon12-normal' and handle == 3 and len(packet) == 16
            send(op,handle)
        elif op == 0x2060:
            assert handle == 3 and packet[16:32] == struct.pack('<4I',6 if mode.startswith('hwqueue-internal-') else 2 if mode.startswith('hwqueue-nobroadcast-') else 0,4,0,0)
            assert packet[32:] == bytes([37,38,39,40])
            failed = mode in ('hwqueue-nt-failure','hwqueue-bad-failure','hwqueue-nobroadcast-nt-failure','hwqueue-internal-nt-failure')
            queue = 0 if failed or mode == 'hwqueue-bad-id' else 4
            sync = 0 if failed or mode == 'hwqueue-bad-sync' else 4 if mode == 'hwqueue-same-sync' else 5
            offset = 0 if failed else 1 if mode == 'hwqueue-bad-alignment' else 1048576 if mode == 'hwqueue-bad-offset' else 8192
            gpu = 0 if failed or mode == 'hwqueue-zero-gpu' else 65537 if mode == 'hwqueue-bad-gpu-alignment' else 1 << 48 if mode == 'hwqueue-bad-gpu-range' else 65536
            if mode == 'hwqueue-bad-failure': sync = 5
            data = struct.pack('<IIQQ',sync,1 if mode == 'hwqueue-bad-reserved' else 0,offset,gpu) + bytes([37 ^ 255,38,39,40])
            if mode == 'hwqueue-short': data = data[:-1]
            send(op,queue,value=1 if mode == 'hwqueue-bad-value' else 0,data=data,
                 ntstatus=-1073741811 if failed else 259 if mode in ('hwqueue-bad-status','hwqueue-nobroadcast-bad-status','hwqueue-internal-bad-status') else 0)
        elif op == 0x2061:
            assert handle == 4 and len(packet) == 16
            send(op,handle)
        elif op == 0x2040:
            sync = 0 if mode == 'paging-bad-sync' else 4
            offset = 1048576 if mode == 'paging-bad-offset' else 0
            reserved = 1 if mode == 'paging-bad-reserved' else 0
            data = struct.pack('<IIQ',sync,reserved,offset)
            if mode == 'paging-short': data = data[:-1]
            send(op,3,value=1 if mode == 'paging-bad-value' else 0,data=data)
        elif op == 0x2008: send(op,handle)
        elif op in (0x2059, 0x205b):
            assert handle == 2 and struct.unpack_from('<8I',packet,16) == (4,0x78100000,0,4,0,0,4,0)
            if op == 0x205b:
                assert mode.startswith('native-resource-')
                assert struct.unpack_from('<4I',packet,48) == (130,73,28,1)
            assert packet[64 if op == 0x205b else 48:] == bytes([37,38,39,40,5,6,7,8])
            failed = mode == 'shared-resource-nt-failure'
            data = struct.pack('<II',0 if failed else 4,0) + bytes([37 ^ 255,38,39,40,5,6,7,8])
            if mode == 'shared-resource-short': data = data[:-1]
            if mode == 'shared-resource-runtime-changed': data = data[:-1] + b'X'
            send(op,0 if failed else 3,data=data,ntstatus=-1073741811 if failed else 0)
        elif op == 0x2057:
            assert handle == 2 and struct.unpack_from('<6I',packet,16) == (4,0x78100000,0,4,0,0)
            assert packet[40:] == bytes([37,38,39,40])
            failed = mode in ('resource-nt-failure','resource-bad-failure')
            resource = 0 if failed or mode == 'resource-zero' else 3 if mode == 'resource-same-id' else 4
            if mode == 'resource-bad-failure': resource = 4
            data = struct.pack('<II', resource, 1 if mode == 'resource-reserved' else 0) + bytes([37 ^ 255,38,39,40])
            if mode == 'resource-short': data = data[:-1]
            send(op, 0 if failed or mode == 'resource-no-allocation' else 3, data=data,
                 value=1 if mode == 'resource-bad-va' else 0,
                 ntstatus=-1073741811 if failed else 259 if mode == 'resource-bad-status' else 0)
        elif op == 0x2058:
            assert handle == 4 and packet[16:] == struct.pack('<I',2)
            send(op, 0 if mode == 'resource-destroy-id' else handle,
                 value=1 if mode == 'resource-destroy-value' else 0,
                 data=b'X' if mode == 'resource-destroy-long' else b'',
                 ntstatus=-1073741811 if mode == 'resource-failed-destroy' and operations.count(op) == 1 else
                          259 if mode == 'resource-destroy-status' else 0)
        elif op == 0x2050:
            priority = 0xc8000000 if mode == 'allocation-maximum' else 0x78100000
            source = 0xffffffff if mode == 'allocation-uninitialized-source' else 0
            assert handle == 2 and struct.unpack_from('<6I',packet,16) == (4,priority,source,4,0,0)
            assert packet[40:] == bytes([37,38,39,40])
            data = bytes([37 ^ 255,38,39,40])
            if mode == 'allocation-short': data = data[:-1]
            send(op,0 if mode in ('allocation-bad-id','allocation-nt-failure') else 3,
                 value=1 if mode == 'allocation-bad-va' else 0,data=data,
                 ntstatus=-1073741811 if mode == 'allocation-nt-failure' else 0)
        elif op == 0x2051:
            assert handle == 2 and packet[16:] == struct.pack('<III',1,0,3)
            send(op,0 if mode == 'allocation-destroy-bad-id' else handle,
                 value=1 if mode == 'allocation-destroy-bad-value' else 0,
                 data=b'X' if mode == 'allocation-destroy-long' else b'',
                 ntstatus=-1073741811 if (mode == 'allocation-failed-destroy' and operations.count(op) == 1) or mode == 'translation-failed-cleanup' else
                          259 if mode == 'allocation-destroy-bad-status' else 0)
        elif op == 0x2056:
            assert handle == 3 and packet[16:] == struct.pack('<IIII',2,1,0,0)
            failed = mode in ('translation-nt-failure','translation-bad-failure','translation-failed-cleanup')
            token = 0 if failed or mode == 'translation-zero' else 1 << 32 if mode == 'translation-overflow' else 0x10000003
            if mode == 'translation-bad-failure': token = 1
            send(op,0 if mode == 'translation-bad-id' else handle,value=token,
                 data=b'X' if mode == 'translation-long' else b'',
                 ntstatus=-1073741811 if failed else 259 if mode == 'translation-bad-status' else 0)
        elif op == 0x2054:
            assert handle == 3 and packet[16:] == struct.pack('<II',2,0)
            native_failure = mode in ('cpu-nt-failure','cpu-bad-failure')
            size = 0 if native_failure or mode == 'cpu-bad-bytes' else 65537 if mode == 'cpu-bad-alignment' else 65536
            reserved = 1 if mode == 'cpu-bad-reserved' else 0
            generation = 0 if native_failure or mode == 'cpu-bad-generation' else 1
            offset = 256 * 1048576 if mode == 'cpu-bad-offset' else 1 if mode == 'cpu-bad-slot' else 0
            if mode == 'cpu-bad-failure': size = 4096
            data = struct.pack('<IIQ',size,reserved,generation)
            if mode == 'cpu-short': data = data[:-1]
            send(op,0 if mode == 'cpu-bad-id' else handle,value=offset,data=data,
                 ntstatus=-1073741811 if native_failure else 259 if mode == 'cpu-bad-status' else 0)
        elif op in (0x2030,0x2031,0x2032): send(op,handle,4)
        elif op == 0x2033: send(op,handle,4,struct.pack('<I',0x231b))
        elif op == 0x2034: send(op,handle)
        else: raise AssertionError('Unexpected host operation')
except EOFError:
    pass
if mode == 'reuse': assert operations == [0x2000], operations
if mode in ('paging-disabled','paging-invalid'): assert 0x2040 not in operations, operations
if mode == 'paging-no-hub': assert operations[-2:] == [0x2040,0x2008], operations
if mode in ('allocation-disabled','allocation-invalid'): assert 0x2050 not in operations, operations
if mode in ('allocation-normal','allocation-maximum'): assert operations.count(0x2051) == 1, operations
if mode == 'allocation-failed-destroy': assert operations.count(0x2051) == 2, operations
if mode.startswith('allocation-destroy-'): assert operations.count(0x2051) == 1, operations
if mode.startswith('gpuva-'): assert operations == [0x2000], operations
if mode.startswith('cpu-wait-'): assert operations == [0x2000], operations
if mode.startswith('context-signal-'): assert operations == [0x2000], operations
if mode.startswith(('hw-wait-','context-wait-')) or mode == 'share-attributes': assert operations == [0x2000], operations
if mode == 'glon12-normal': assert operations == [0x2000,0x2001,0x2004,0x2020,0x2021], operations
if mode in ('resource-disabled','resource-invalid'): assert 0x2057 not in operations, operations
if mode in ('shared-resource-disabled','shared-resource-invalid'): assert 0x2059 not in operations, operations
if mode in ('native-resource-disabled','native-resource-invalid','native-resource-unowned'): assert 0x205b not in operations, operations
if mode in ('native-resource-normal','native-resource-duplicate','native-resource-retry','native-resource-thread'): assert operations.count(0x205b) == operations.count(0x2058) == 1 and 0x2059 not in operations, operations
if mode == 'shared-resource-normal': assert operations.count(0x2059) == operations.count(0x2058) == 1, operations
if mode == 'resource-normal' or mode.startswith('resource-destroy-'): assert operations.count(0x2058) == 1, operations
if mode == 'resource-failed-destroy': assert operations.count(0x2058) == 2, operations
if mode == 'resource-list-destroy': assert operations.count(0x2051) == 1, operations
if mode.startswith('gpu-state-'): assert operations == [0x2000], operations
if mode.startswith('resident-'): assert operations == [0x2000], operations
if mode == 'errno-log-failure': assert operations == [0x2000,0x2001], operations
if mode.startswith('device-state-'):
    assert operations == ([0x2000] if mode=='device-state-unowned' else [0x2000,0x2001,0x2004,0x20b0]), operations
if mode in ('cpu-disabled','cpu-invalid'): assert 0x2054 not in operations, operations
if mode.startswith('cpu-'): assert 0x2055 not in operations, operations
if mode == 'translation-disabled': assert 0x2056 not in operations, operations
if mode in ('translation-normal','translation-invalid','translation-disabled','translation-nt-failure','translation-failed-cleanup'):
    assert operations.count(0x2051) == 1, operations
if mode.startswith('translation-'): assert 0x2016 not in operations, operations
if mode in ('hwqueue-disabled','hwqueue-invalid','hwqueue-sync-context','hwqueue-nobroadcast-disabled'): assert 0x2060 not in operations, operations
if mode in ('hwqueue-no-hub','hwqueue-nobroadcast-no-hub'): assert operations[-2:] == [0x2060,0x2061], operations
if mode in ('sync-disabled','sync-invalid'): assert 0x2070 not in operations and 0x2071 not in operations, operations
if mode.startswith('sync-') and mode.endswith('-no-hub'): assert operations[-2:] == [0x2070,0x2071], operations
if mode == 'sync-mutex' or mode.startswith('sync-destroy-'): assert operations.count(0x2071) == 1, operations
if mode == 'sync-failed-destroy': assert operations.count(0x2071) == 2, operations
if mode.startswith('submit-'): assert 0x2062 not in operations, operations
if mode.startswith('hw-signal-'): assert 0x2073 not in operations, operations
if mode in ('reservation-disabled','reservation-invalid'): assert 0x2080 not in operations and 0x2081 not in operations, operations
if mode in ('reservation-normal','reservation-overlap') or mode.startswith('reservation-free-'): assert operations.count(0x2080) == operations.count(0x2081) == 1, operations
if mode == 'reservation-failed-free': assert operations.count(0x2081) == 2, operations
if mode == 'reservation-quota': assert operations.count(0x2080) == 8, operations
if mode == 'reservation-byte-quota': assert operations.count(0x2080) == 4, operations
if mode in ('reservation-duplicate','reservation-reply-overlap'): assert operations.count(0x2080) == 2, operations
print('FAKE_WORKER_EOF=true',file=sys.stderr)
if mode == 'exit-lifetime-fd': assert operations == [0x2000], operations
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--shim', required=True, type=pathlib.Path)
    parser.add_argument('--probe', required=True, type=pathlib.Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='wddm-ioctl-fault-') as directory:
        worker = pathlib.Path(directory) / 'owned-worker'
        worker.write_text(WORKER, encoding='utf-8'); worker.chmod(0o700)
        for mode in ('device-state-normal', 'device-state-error-state', 'device-state-unowned', 'device-state-removed', 'device-state-oom',
                     'device-state-unsupported-peer', 'device-state-unsupported-native', 'device-state-zero', 'device-state-positive',
                     'device-state-bad-handle', 'device-state-bad-error', 'device-state-long',
                     'errno-log-failure', 'exit-lifetime-fd', 'context-wait-disabled', 'context-wait-invalid', 'context-wait-unowned', 'hw-wait-disabled', 'hw-wait-invalid', 'hw-wait-unowned', 'share-attributes', 'native-resource-normal', 'native-resource-disabled', 'native-resource-invalid',
                     'native-resource-unowned', 'native-resource-duplicate', 'native-resource-retry', 'native-resource-thread', 'consume-disabled', 'consume-unowned', 'consume-null', 'no-worker', 'wrong-version', 'reuse', 'bad-reply', 'closed-worker', 'normal',
                     'cpu-wait-invalid', 'cpu-wait-unowned',
                     'shared-resource-normal', 'shared-resource-disabled', 'shared-resource-invalid',
                     'shared-resource-nt-failure', 'shared-resource-short', 'shared-resource-runtime-changed',
                     'glon12-normal', 'context-signal-invalid', 'context-signal-disabled', 'context-signal-unowned',
                     'hw-signal-invalid', 'hw-signal-disabled', 'hw-signal-unowned',
                     'resource-normal', 'resource-disabled', 'resource-invalid', 'resource-nt-failure', 'resource-bad-failure',
                     'resource-zero', 'resource-same-id', 'resource-reserved', 'resource-short', 'resource-no-allocation',
                     'resource-bad-va', 'resource-bad-status', 'resource-failed-destroy', 'resource-list-destroy',
                     'resource-destroy-id', 'resource-destroy-value', 'resource-destroy-long', 'resource-destroy-status',
                     'paging-disabled', 'paging-invalid', 'paging-bad-sync', 'paging-bad-offset',
                     'paging-bad-reserved', 'paging-bad-value', 'paging-short', 'paging-no-hub',
                     'allocation-normal', 'allocation-maximum', 'allocation-uninitialized-source', 'allocation-disabled', 'allocation-invalid', 'allocation-nt-failure',
                     'allocation-bad-id', 'allocation-bad-va', 'allocation-short', 'allocation-failed-destroy',
                     'allocation-destroy-bad-id', 'allocation-destroy-bad-status', 'allocation-destroy-bad-value', 'allocation-destroy-long',
                     'gpuva-disabled', 'gpuva-invalid', 'gpu-state-disabled', 'gpu-state-invalid', 'gpu-state-unowned', 'resident-disabled', 'resident-invalid',
                     'cpu-disabled', 'cpu-invalid', 'cpu-nt-failure', 'cpu-bad-failure', 'cpu-bad-id',
                     'cpu-bad-bytes', 'cpu-bad-alignment', 'cpu-bad-reserved', 'cpu-bad-generation',
                     'cpu-bad-offset', 'cpu-bad-slot', 'cpu-short', 'cpu-bad-status', 'cpu-no-hub',
                     'translation-normal', 'translation-invalid', 'translation-disabled', 'translation-nt-failure',
                     'translation-failed-cleanup', 'translation-zero', 'translation-overflow', 'translation-bad-failure',
                     'translation-bad-status', 'translation-bad-id', 'translation-long',
                     'hwqueue-disabled', 'hwqueue-invalid', 'hwqueue-sync-context', 'hwqueue-nt-failure', 'hwqueue-bad-failure',
                     'hwqueue-bad-id', 'hwqueue-bad-sync', 'hwqueue-same-sync', 'hwqueue-bad-alignment', 'hwqueue-bad-offset',
                     'hwqueue-zero-gpu', 'hwqueue-bad-gpu-alignment', 'hwqueue-bad-gpu-range', 'hwqueue-bad-reserved',
                     'hwqueue-short', 'hwqueue-bad-value', 'hwqueue-bad-status', 'hwqueue-no-hub',
                     'hwqueue-nobroadcast-disabled', 'hwqueue-nobroadcast-no-hub', 'hwqueue-nobroadcast-nt-failure', 'hwqueue-nobroadcast-bad-status',
                     'hwqueue-internal-disabled', 'hwqueue-internal-missing-signal', 'hwqueue-internal-no-hub', 'hwqueue-internal-nt-failure', 'hwqueue-internal-bad-status',
                     'sync-mutex', 'sync-failed-destroy', 'sync-disabled', 'sync-invalid', 'sync-nt-failure', 'sync-bad-failure',
                     'sync-bad-id', 'sync-bad-alignment', 'sync-bad-offset', 'sync-zero-gpu', 'sync-bad-gpu-alignment',
                     'sync-bad-gpu-range', 'sync-short', 'sync-bad-value', 'sync-bad-status', 'sync-no-hub', 'sync-mutex-bad-map',
                     'sync-nogpu-no-hub', 'sync-nogpu-nonzero-gpu', 'sync-nogpu-bad-offset',
                     'sync-nomax-no-hub', 'sync-nomax-zero-gpu',
                     'sync-ntshared-no-hub', 'sync-ntshared-nonzero-gpu',
                     'sync-ntshared-lifetime-no-hub', 'sync-ntshared-lifetime-nonzero-gpu',
                     'priority-normal', 'priority-disabled', 'priority-unowned', 'priority-invalid', 'priority-nt-failure',
                     'priority-bad-id', 'priority-bad-value', 'priority-long', 'priority-bad-status',
                     'sync-destroy-bad-id', 'sync-destroy-bad-value', 'sync-destroy-long', 'sync-destroy-bad-status',
                     'submit-disabled', 'submit-invalid', 'submit-unowned', 'submit-ignored-pointer',
                     'reservation-normal', 'reservation-failed-free', 'reservation-disabled', 'reservation-invalid',
                     'reservation-nt-failure', 'reservation-bad-failure', 'reservation-bad-id', 'reservation-zero',
                     'reservation-alignment', 'reservation-range', 'reservation-minimum', 'reservation-maximum',
                     'reservation-bad-status', 'reservation-long', 'reservation-free-id', 'reservation-free-status',
                     'reservation-free-value', 'reservation-free-long', 'reservation-quota', 'reservation-byte-quota',
                     'reservation-overlap', 'reservation-duplicate', 'reservation-reply-overlap'):
            environment = {k: v for k, v in os.environ.items() if not k.startswith('WDDM_BRIDGE_') and k != 'LD_PRELOAD'}
            environment['LD_PRELOAD'] = str(args.shim.resolve())
            environment['BRIDGE_FAULT_TEST'] = mode
            if mode != 'no-worker': environment['WDDM_BRIDGE_WINDOWS_WORKER'] = str(worker)
            result = subprocess.run([str(args.probe.resolve()), mode], env=environment,
                                    capture_output=True, text=True, timeout=20)
            if result.returncode or 'realDxgForwarding=true' in result.stderr:
                raise RuntimeError(f'{mode} failed ({result.returncode}): {result.stderr}')
            if mode == 'exit-lifetime-fd' and 'EXIT_DESCRIPTOR_FINALIZER_PASS liveUntilFinalizer=true requestsRejected=true close=true' not in result.stderr:
                raise RuntimeError('Late descriptor finalizer did not execute')
            if mode != 'no-worker' and ('workerReaped=true exit=0' not in result.stderr or 'FAKE_WORKER_EOF=true' not in result.stderr):
                raise RuntimeError(f'{mode}: owned worker did not terminate cleanly: {result.stderr}')
            print('PASS: Linux ioctl diagnostic ' + mode)


if __name__ == '__main__':
    main()
