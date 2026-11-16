// SPDX-License-Identifier: MIT
// A CPU-only connected peer replies after the old outer deadline, simulating a
// valid five-second child wait plus reply work. No QEMU/GPU/display is used.
#include "qmp_deadline_budget.h"
#include <cassert>
#include <cstdio>
#ifdef _WIN32
#include "qmp_socket_wait.h"
#include <thread>

struct Socket {
    SOCKET value = INVALID_SOCKET;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
static void delayedReply(driver_qmp::ChannelPurpose purpose, bool timeoutExpected) {
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
    // Keep the client alive until the sender joins even for the timeout case.
    const auto began = GetTickCount64();
    std::thread sender([&] {
        Sleep(driver_qmp::SdlRetirementFenceTimeoutMs + 200);
        assert(send(peer.value, "r", 1, 0) == 1);
    });
    bool timedOut = false;
    try {
        driver_qemu::waitQmpSocket(client.value, true,
            began + driver_qmp::commandTimeoutMs(purpose));
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "QMP control deadline exceeded");
        timedOut = true;
    }
    assert(timedOut == timeoutExpected);
    if (!timedOut) {
        char byte = 0;
        assert(recv(client.value, &byte, 1, 0) == 1 && byte == 'r');
    }
    sender.join();
}
#endif

int main() {
    using namespace driver_qmp;
    assert(commandTimeoutMs(ChannelPurpose::Control) == 5000);
    assert(commandTimeoutMs(ChannelPurpose::SdlGraphics) == 7000);
    const std::uint64_t began = 123;
    const auto childReply = began + SdlRetirementFenceTimeoutMs + 200;
    assert(childReply > began + commandTimeoutMs(ChannelPurpose::Control));
    assert(childReply < began + commandTimeoutMs(ChannelPurpose::SdlGraphics));
    assert(began + 7001 > began + commandTimeoutMs(ChannelPurpose::SdlGraphics));
    bool rejected = false;
    try { (void)commandTimeoutMs(static_cast<ChannelPurpose>(255)); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
#ifdef _WIN32
    WSADATA data{}; assert(WSAStartup(MAKEWORD(2,2), &data) == 0);
    delayedReply(ChannelPurpose::Control, true);
    delayedReply(ChannelPurpose::SdlGraphics, false);
    assert(WSACleanup() == 0);
    std::puts("QMP_DEADLINE_BUDGET_PASS delayedPeer=true controlMs=5000 graphicsMs=7000 gpu=false");
#else
    std::puts("QMP_DEADLINE_BUDGET_PASS policy=true windowsDelayedPeerNotRun=true gpu=false");
#endif
}
