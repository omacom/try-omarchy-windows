#!/usr/bin/env python3
"""patch_loader.py IN OUT: the Khronos Vulkan loader for Wine/Proton.

On Windows the loader loads system32\\dxgi.dll and creates a DXGI factory
while it reads the registered drivers and while it sorts physical devices.
Under Proton dxgi.dll is DXVK's, whose factory creates a Vulkan instance
through this same loader (installed as winevulkan.dll as well): the loader
re-enters itself and the process deadlocks before any driver loads.

The loader keeps its DXGI path as one UTF-16 string; this points it at
system32\\dxgs.dll (dxgs.c), a factory without adapters, instead.
"""
import sys

DXGI = '\\dxgi.dll'.encode('utf-16-le')
STUB = '\\dxgs.dll'.encode('utf-16-le')


def main(src, dst):
    data = bytearray(open(src, 'rb').read())
    if data.count(DXGI) != 1:
        sys.exit('patch_loader: expected one L"\\dxgi.dll" in ' + src)
    i = data.find(DXGI)
    data[i:i + len(DXGI)] = STUB
    open(dst, 'wb').write(data)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
