"""Package the guest-only read-only normal-root bootstrap and matching overlay module."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import stat
import struct


def package(init,module,kernel_release):
    if not re.fullmatch(r'[A-Za-z0-9._+-]{1,80}',kernel_release):
        raise ValueError('Invalid expected kernel release')
    def elf(value,types):
        return len(value)>=64 and value[:7]==b'\x7fELF\x02\x01\x01' and struct.unpack_from('<HH',value,16)[0] in types and struct.unpack_from('<H',value,18)[0]==62
    if not elf(init,(2,3)) or not elf(module,(1,)):
        raise ValueError('Expected x86-64 ELF executable and uncompressed relocatable module')
    versions=re.findall(rb'vermagic=([^\0]{1,255})\0',module)
    if len(versions)!=1 or versions[0].split(b' ',1)[0]!=kernel_release.encode():
        raise ValueError('Overlay module kernel release mismatch')
    if len(init)>2*1024*1024 or len(module)>1024*1024:
        raise ValueError('Bootstrap input exceeds its private-image bound')
    entries={'dev':(stat.S_IFDIR|0o755,b'',0,0),'dev/console':(stat.S_IFCHR|0o600,b'',5,1),
        'init':(stat.S_IFREG|0o755,init,0,0),'overlay.ko':(stat.S_IFREG|0o644,module,0,0)}
    result=bytearray()
    def entry(name,mode,data,major,minor):
        encoded=name.encode()+b'\0'
        fields=[1,mode,0,0,1,0,len(data),0,0,major,minor,len(encoded),0]
        result.extend(b'070701'+''.join(f'{value:08x}' for value in fields).encode()+encoded)
        result.extend(b'\0'*(-len(result)%4));result.extend(data);result.extend(b'\0'*(-len(result)%4))
    for name,(mode,data,major,minor) in entries.items():entry(name,mode,data,major,minor)
    entry('TRAILER!!!',0,b'',0,0)
    if len(result)>2*1024*1024:raise ValueError('Bootstrap archive exceeds 2 MiB')
    return gzip.compress(bytes(result),mtime=0),len(result)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--init',type=Path,required=True)
    parser.add_argument('--overlay-module',type=Path,required=True)
    parser.add_argument('--kernel-release',required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    init=args.init.read_bytes();module=args.overlay_module.read_bytes()
    image,raw_bytes=package(init,module,args.kernel_release)
    sha=lambda data:hashlib.sha256(data).hexdigest()
    with args.output.open('xb') as stream:stream.write(image)
    manifest=dict(schema=1,privateImage=True,normalGraphicsVerified=False,normalDesktopVerified=False,
        expectedKernelRelease=args.kernel_release,initSha256=sha(init),overlayModuleSha256=sha(module),
        initramfsSha256=sha(image),uncompressedBytes=raw_bytes,compressedBytes=len(image))
    with args.output.with_suffix('.manifest.json').open('x',newline='\n') as stream:
        json.dump(manifest,stream,indent=2);stream.write('\n')
    print(json.dumps(manifest))


if __name__=='__main__':main()
