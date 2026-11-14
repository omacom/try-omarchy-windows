// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include "tcp_peer_owner.h"
#include <cstdio>
int main() {
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)) return 1;
    const auto listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    const auto client=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    int bytes=sizeof address;
    if(listener==INVALID_SOCKET || client==INVALID_SOCKET || bind(listener,reinterpret_cast<sockaddr*>(&address),bytes) ||
       listen(listener,1) || getsockname(listener,reinterpret_cast<sockaddr*>(&address),&bytes) ||
       connect(client,reinterpret_cast<sockaddr*>(&address),bytes)) return 2;
    const auto server=accept(listener,nullptr,nullptr);
    if(server==INVALID_SOCKET) return 3;
    const bool verified=driver_qemu::tcpPeerOwned(client,GetCurrentProcessId()) &&
        driver_qemu::tcpPeerOwned(server,GetCurrentProcessId()) && !driver_qemu::tcpPeerOwned(client,0) &&
        !driver_qemu::tcpPeerOwned(client,GetCurrentProcessId()==1?2:1) && !driver_qemu::tcpPeerOwned(listener,GetCurrentProcessId());
    const bool closed=!closesocket(server) && !closesocket(client) && !closesocket(listener);
    WSACleanup();
    if(!verified || !closed) return 4;
    std::puts("TCP_PEER_OWNER_ACCEPT_AND_FOREIGN_PID_REJECTION_PASS");return 0;
}
