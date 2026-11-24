"""Host-only NVIDIA section registration and GPU copy acceptance; no guest rendering."""
import ctypes as c
from ctypes import wintypes as w
import argparse, hashlib, json, pathlib, socket, struct, subprocess, sys, uuid

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--bridge', type=pathlib.Path, required=True)
parser.add_argument('--report', type=pathlib.Path, required=True)
args = parser.parse_args()
if sys.platform != 'win32': parser.error('Run on a Windows NVIDIA host')
api = c.WinDLL('kernel32', use_last_error=True)
for name, result, arguments in (
    ('CreateFileMappingW',w.HANDLE,[w.HANDLE,c.c_void_p,w.DWORD,w.DWORD,w.DWORD,w.LPCWSTR]),
    ('MapViewOfFile',c.c_void_p,[w.HANDLE,w.DWORD,w.DWORD,w.DWORD,c.c_size_t]),
    ('UnmapViewOfFile',w.BOOL,[c.c_void_p]),('CloseHandle',w.BOOL,[w.HANDLE])):
    f = getattr(api,name); f.restype=result; f.argtypes=arguments

def receive(stream, count):
    value = bytearray()
    while len(value) != count:
        part = stream.recv(count-len(value))
        if not part: raise RuntimeError('Bridge disconnected')
        value.extend(part)
    return bytes(value)

def call(stream, op, handle=0, payload=b''):
    request = struct.pack('<IIII',op,handle,0,0)+payload
    stream.sendall(struct.pack('<I',len(request))+request)
    size, = struct.unpack('<I',receive(stream,4)); assert 16 <= size <= 4096
    packet = receive(stream,size); kind, result, status, reserved = struct.unpack('<IIiI',packet[:16])
    assert kind==op and status==0 and reserved==0, packet.hex()
    if op != 0x2000:
        native, reserved, value=struct.unpack('<iIQ',packet[16:32]); assert native >= 0 and reserved==0, packet.hex()
        return result, value
    assert struct.unpack('<IIII',packet[16:])[:3]==(1,31,0x10de)

def transport_checks(bridge, name, size):
    results = {}
    for truncated in (False, True):
        worker = subprocess.Popen([str(bridge),'--listen','0','--guest-section',name,'--guest-ram-bytes',str(size)],
            stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            port = json.loads(worker.stdout.readline())['port']
            with socket.create_connection(('127.0.0.1',port),timeout=15) as stream:
                call(stream,0x2000,payload=struct.pack('<I',1))
                adapter,_=call(stream,0x2001); device,_=call(stream,0x2004,adapter); call(stream,0x2006,device)
                if truncated: stream.sendall(struct.pack('<I',32)+b'\x00')
                stream.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('HH',1,0))
            out,errors=worker.communicate(timeout=10)
            cleanup=json.loads(errors.splitlines()[0])
            assert cleanup['driverCleanupVerified'] and cleanup['liveGpuCopyObjects']==0
            assert worker.returncode==(1 if truncated else 0)
            if truncated: assert 'Truncated frame' in errors
            results['truncatedFrameRejectedWithCleanup' if truncated else 'resetAtFrameBoundaryCleaned']=True
        finally:
            if worker.poll() is None: worker.kill(); worker.communicate(timeout=10)
    return results

name='Local\\7Wdev-WDDM-'+uuid.uuid4().hex
size=16*1024*1024
section=api.CreateFileMappingW(w.HANDLE(-1),None,4,0,size,name); assert section
view=api.MapViewOfFile(section,6,0,0,size); assert view
bridge=args.bridge.resolve()
worker=subprocess.Popen([str(bridge),'--listen','0','--guest-section',name,'--guest-ram-bytes',str(size)],
                        stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,creationflags=subprocess.CREATE_NO_WINDOW)
try:
    port=json.loads(worker.stdout.readline())['port']
    with socket.create_connection(('127.0.0.1',port),timeout=15) as stream:
        call(stream,0x2000,payload=struct.pack('<I',1))
        adapter,_=call(stream,0x2001); device,_=call(stream,0x2004,adapter); queue,_=call(stream,0x2006,device)
        first,_=call(stream,0x2017,device,struct.pack('<QII',0x200000,65536,0))
        second,_=call(stream,0x2017,device,struct.pack('<QII',0x210000,65536,0))
        for allocation in (first,second):
            call(stream,0x2013,allocation,struct.pack('<I',queue)); call(stream,0x2014,allocation,struct.pack('<I',queue))
        total=0
        for cycle,(source,destination,count) in enumerate(((0,0,65536),(7,31,73),(0,31,65505),(0,0,65536),(201,501,257),(0,0,65536))):
            pattern=bytes(((i*31+cycle*13)^(i>>8))&255 for i in range(65536))
            c.memmove(view+0x200000,pattern,65536); c.memset(view+0x210000,85,65536)
            _,written=call(stream,0x2018,second,struct.pack('<IIII',first,source,destination,count)); assert written==count
            expected=bytes([85])*destination+pattern[source:source+count]+bytes([85])*(65536-destination-count)
            assert c.string_at(view+0x210000,65536)==expected
            assert c.string_at(view+0x200000,65536)==pattern
            total+=count
    out,errors=worker.communicate(timeout=10)
    if worker.returncode: raise RuntimeError(errors)
    report=json.loads(errors); assert report['driverCleanupVerified'] and report['gpuCopiedBytes']==total and report['completedGpuCopies']==6
    transport=transport_checks(bridge,name,size)
    result = {'schema':1,'success':True,'testScope':'Windows host only','gpuCopiedBytesVerified':total,'copyCycles':6,'cleanup':report,'transportChecks':transport,'driverBridgeSha256':hashlib.sha256(bridge.read_bytes()).hexdigest(),'guestDesktopAcceleratedByThisBackend':False}
    args.report.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(result,indent=2))
except BaseException:
    out,errors=worker.communicate(timeout=10); print(errors)
    raise
finally:
    if worker.poll() is None: worker.kill(); worker.communicate(timeout=10)
    assert api.UnmapViewOfFile(view)
    assert api.CloseHandle(section)
