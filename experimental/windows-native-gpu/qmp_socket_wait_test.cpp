// SPDX-License-Identifier: MIT
// Real connected loopback sockets, with no QEMU, GPU, desktop or input.
#include "qmp_socket_wait.h"
#include <cassert>
#include <cstdio>

struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
template<class F> static void failure(F action, const std::string& expected) {
    bool rejected = false;
    try { action(); }
    catch (const std::runtime_error& error) { rejected = error.what() == expected; }
    assert(rejected);
}
int main() {
    WSADATA data{}; assert(WSAStartup(MAKEWORD(2,2), &data) == 0);
    {
        Socket listener, client, peer;
        listener.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(listener.value != INVALID_SOCKET);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        int size = sizeof address;
        assert(getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        assert(listen(listener.value, 1) == 0);
        client.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        assert(client.value != INVALID_SOCKET);
        assert(connect(client.value, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        peer.value = accept(listener.value, nullptr, nullptr);
        assert(peer.value != INVALID_SOCKET);
        driver_qemu::waitQmpSocket(client.value, false, GetTickCount64() + 1000);
        failure([&] { driver_qemu::waitQmpSocket(client.value, true, GetTickCount64()); },
                "QMP control deadline exceeded");
        const auto before = GetTickCount64();
        failure([&] { driver_qemu::waitQmpSocket(client.value, true, before + 40); },
                "QMP control deadline exceeded");
        assert(GetTickCount64() >= before + 20); // Coarse Windows tick resolution.
        assert(send(peer.value, "x", 1, 0) == 1);
        driver_qemu::waitQmpSocket(client.value, true, GetTickCount64() + 1000);
        char byte = 0; assert(recv(client.value, &byte, 1, 0) == 1 && byte == 'x');
        const auto closed = peer.value;
        assert(closesocket(peer.value) == 0); peer.value = INVALID_SOCKET;
        failure([&] { driver_qemu::waitQmpSocket(closed, true, GetTickCount64() + 1000); },
                "QMP control socket failed: Winsock " + std::to_string(WSAENOTSOCK));
        // A clean peer close is readable; recv, rather than select, owns EOF.
        driver_qemu::waitQmpSocket(client.value, true, GetTickCount64() + 1000);
        assert(recv(client.value, &byte, 1, 0) == 0);
    }
    assert(WSACleanup() == 0);
    std::puts("QMP_SOCKET_WAIT_PASS quietDeadline=true ready=true writeReady=true socketError=true eofPreserved=true gpu=false");
}
