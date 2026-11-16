// SPDX-License-Identifier: MIT
// Windows-style hAsyncEvent reserves zero for synchronous waits. Normalize only
// an actual eventfd zero, without holding a bridge lock or changing its counter.
#pragma once
#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace guest_eventfd {
inline int create(unsigned initial, int flags, bool nonzeroHandle) noexcept {
    const auto created = syscall(SYS_eventfd2, initial, flags);
    if (created < 0 || created != 0 || !nonzeroHandle) return static_cast<int>(created);
    // Raw syscalls avoid reentering any preload shim while a runtime lock is held.
    const auto duplicate = syscall(SYS_fcntl, 0, (flags & EFD_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 3);
    const int saved = errno;
    syscall(SYS_close, 0); // No retry on EINTR: Linux may already reuse the number.
    if (duplicate < 0) errno = saved;
    return static_cast<int>(duplicate);
}
}
