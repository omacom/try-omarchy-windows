// SPDX-License-Identifier: MIT
// Exercise real interposed descriptor cleanup with mock native/kernel peers.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
int main() {
    const int fd=open("/dev/dxg",O_RDWR|O_CLOEXEC);
    if (fd<0) return 1;
    D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart=0x57475055;
    if (ioctl(fd,_IOWR('G',1,D3DKMT_OPENADAPTERFROMLUID),&adapter)) return 2;
    D3DKMT_CREATEDEVICE device{}; device.hAdapter=adapter.hAdapter; device.Flags.RequestVSync=1;
    if (ioctl(fd,_IOWR('G',2,D3DKMT_CREATEDEVICE),&device)) return 3;
    D3DKMT_CREATEPAGINGQUEUE queue{}; queue.hDevice=device.hDevice; queue.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    if (ioctl(fd,_IOWR('G',7,D3DKMT_CREATEPAGINGQUEUE),&queue) ||
        !queue.FenceValueCPUVirtualAddress || *static_cast<volatile UINT64*>(queue.FenceValueCPUVirtualAddress)!=57) return 4;
    D3DDDI_DESTROYPAGINGQUEUE destroy{}; destroy.hPagingQueue=queue.hPagingQueue;
    if (ioctl(fd,_IOWR('G',28,D3DDDI_DESTROYPAGINGQUEUE),&destroy) || close(fd)) return 5;
    std::puts("BROKER_SHIM_PROBE mapping=true explicitRelease=true interposedCloseDeadlock=false gpu=false"); return 0;
}
