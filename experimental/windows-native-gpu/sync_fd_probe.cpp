// SPDX-License-Identifier: MIT
// Runs inside the owned QEMU/OpenGL process, using the actual Linux ioctl
// layouts and kernel object FDs. The child transfers the FD unprivileged;
// the parent's existing broker session performs native imports.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <grp.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int transfer(int socket, int fd, bool sending) {
    char byte = 'F';
    iovec vector{&byte, 1};
    union { cmsghdr alignment; char bytes[CMSG_SPACE(sizeof(int))]; } control{};
    msghdr message{}; message.msg_iov = &vector; message.msg_iovlen = 1;
    message.msg_control = control.bytes; message.msg_controllen = sizeof control;
    if (sending) {
        auto header = CMSG_FIRSTHDR(&message); header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS; header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &fd, sizeof fd);
        return syscall(SYS_sendmsg, socket, &message, MSG_NOSIGNAL) == 1 ? fd : -1;
    }
    if (syscall(SYS_recvmsg, socket, &message, MSG_CMSG_CLOEXEC) != 1 || (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) return -1;
    auto header = CMSG_FIRSTHDR(&message);
    if (!header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len != CMSG_LEN(sizeof(int))) return -1;
    memcpy(&fd, CMSG_DATA(header), sizeof fd);
    return fd;
}
template<class T> static bool call(int fd, unsigned nr, T& value) {
    if (!ioctl(fd, _IOC(_IOC_READ | _IOC_WRITE, 'G', nr, sizeof value), &value)) return true;
    std::printf("SYNC_FD_FAILURE nr=%u errno=%d\n", nr, errno); return false;
}
extern "C" int wddm_sync_fd_test() {
    const int fd = open("/dev/dxg", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
    if (!call(fd, 1, adapter)) return 1;
    D3DKMT_CREATEDEVICE sourceDevice{}; sourceDevice.hAdapter = adapter.hAdapter; sourceDevice.Flags.RequestVSync = 1;
    D3DKMT_CREATEDEVICE recipientDevice = sourceDevice;
    if (!call(fd, 2, sourceDevice) || !call(fd, 2, recipientDevice)) return 1;
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 source{}; source.hDevice = sourceDevice.hDevice;
    source.Info.Type = D3DDDI_MONITORED_FENCE;
    source.Info.Flags.Shared = source.Info.Flags.NtSecuritySharing = source.Info.Flags.NoGPUAccess = 1;
    source.Info.MonitoredFence.InitialFenceValue = 57; source.Info.MonitoredFence.EngineAffinity = 1;
    if (!call(fd, 16, source) || !source.Info.MonitoredFence.FenceValueCPUVirtualAddress ||
        source.Info.MonitoredFence.FenceValueGPUVirtualAddress ||
        *static_cast<volatile std::uint64_t*>(source.Info.MonitoredFence.FenceValueCPUVirtualAddress) != 57) return 1;
    HANDLE exported = nullptr;
    D3DKMT_SHAREOBJECTS share{}; share.ObjectCount = 1; share.ObjectHandleArray = &source.hSyncObject;
    share.DesiredAccess = UINT32_C(0x10000000); share.pSharedNtHandle = &exported;
    if (!call(fd, 63, share)) return 1;
    int shared = static_cast<int>(reinterpret_cast<std::intptr_t>(exported));
    if (shared < 0 || !(fcntl(shared, F_GETFD) & FD_CLOEXEC)) return 1;
    int sockets[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets)) return 1;
    const auto child = fork();
    if (child < 0) return 1;
    if (!child) {
        // Avoid the inherited process interposer/mutex after a multithreaded
        // Mesa fork. Only raw syscalls and the supplied socket/FD are used.
        syscall(SYS_close, shared); syscall(SYS_close, sockets[0]);
        if (syscall(SYS_setgroups, 0, nullptr) || syscall(SYS_setgid, 65534) || syscall(SYS_setuid, 65534)) _exit(2);
        const auto received = transfer(sockets[1], -1, false);
        if (received < 0 || transfer(sockets[1], received, true) < 0) _exit(3);
        syscall(SYS_close, received); syscall(SYS_close, sockets[1]); _exit(0);
    }
    close(sockets[1]);
    if (transfer(sockets[0], shared, true) < 0) return 1;
    close(shared);
    shared = transfer(sockets[0], -1, false); close(sockets[0]);
    int childStatus = 0;
    if (shared < 0 || waitpid(child, &childStatus, 0) != child || !WIFEXITED(childStatus) || WEXITSTATUS(childStatus)) return 1;
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy{}; destroy.hSyncObject = source.hSyncObject;
    D3DKMT_DESTROYDEVICE destroyDevice{}; destroyDevice.hDevice = sourceDevice.hDevice;
    if (!call(fd, 29, destroy) || !call(fd, 25, destroyDevice)) return 1;
    D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 imported{}; imported.hNtHandle = reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(shared));
    imported.hDevice = recipientDevice.hDevice; imported.Flags.NoGPUAccess = 1; imported.MonitoredFence.EngineAffinity = 1;
    if (!call(fd, 64, imported) || !imported.hSyncObject || !imported.MonitoredFence.FenceValueCPUVirtualAddress ||
        imported.MonitoredFence.FenceValueGPUVirtualAddress ||
        *static_cast<volatile std::uint64_t*>(imported.MonitoredFence.FenceValueCPUVirtualAddress) != 57) return 1;
    close(shared);
    // Re-export the imported alias after the original object/device/FD have
    // gone. That ioctl also reaps the original native NT reference.
    exported = nullptr; share.ObjectHandleArray = &imported.hSyncObject;
    if (!call(fd, 63, share)) return 1;
    shared = static_cast<int>(reinterpret_cast<std::intptr_t>(exported));
    if (shared < 0 || *static_cast<volatile std::uint64_t*>(imported.MonitoredFence.FenceValueCPUVirtualAddress) != 57) return 1;
    close(shared);
    destroy.hSyncObject = imported.hSyncObject;
    if (!call(fd, 29, destroy)) return 1;
    destroyDevice.hDevice = recipientDevice.hDevice;
    if (!call(fd, 25, destroyDevice)) return 1;
    D3DKMT_CLOSEADAPTER closeAdapter{}; closeAdapter.hAdapter = adapter.hAdapter;
    if (!call(fd, 21, closeAdapter)) return 1;
    close(fd);
    puts("SYNC_FD_TEST_COMPLETE nativeExports=2 nativeImports=1 sourceDestroyedBeforeImport=true unprivilegedTransfer=true reexport=true initialValue=57 gpuSignals=false cleanup=true");
    return 0;
}
