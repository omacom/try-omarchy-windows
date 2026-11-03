// SPDX-License-Identifier: MIT
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <stdexcept>
#include <string>

namespace driver_qemu {
// A quiet, connected peer that does not reply is a deadline failure. Preserve
// the Winsock error separately when select itself fails; do not imply EOF.
inline void waitQmpSocket(SOCKET socket, bool reading, ULONGLONG deadline) {
    const auto now = GetTickCount64();
    if (now >= deadline) throw std::runtime_error("QMP control deadline exceeded");
    fd_set set; FD_ZERO(&set); FD_SET(socket, &set);
    const auto remaining = deadline - now;
    timeval timeout{static_cast<long>(remaining / 1000), static_cast<long>((remaining % 1000) * 1000)};
    const auto result = select(0, reading ? &set : nullptr, reading ? nullptr : &set, nullptr, &timeout);
    if (result == 0) throw std::runtime_error("QMP control deadline exceeded");
    if (result == SOCKET_ERROR) {
        const auto error = WSAGetLastError();
        throw std::runtime_error("QMP control socket failed: Winsock " + std::to_string(error));
    }
    if (result != 1 || !FD_ISSET(socket, &set))
        throw std::runtime_error("QMP control readiness result invalid");
}
}
