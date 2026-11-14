// SPDX-License-Identifier: MIT
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <cstddef>
#include <cstring>
#include <vector>
namespace driver_qemu {
inline bool tcpPeerOwned(SOCKET connection,DWORD expected) {
    sockaddr_in local{},peer{};int localSize=sizeof local,peerSize=sizeof peer;
    if(!expected || getsockname(connection,reinterpret_cast<sockaddr*>(&local),&localSize) ||
       getpeername(connection,reinterpret_cast<sockaddr*>(&peer),&peerSize) ||
       local.sin_family!=AF_INET || peer.sin_family!=AF_INET ||
       local.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || peer.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)) return false;
    DWORD bytes=0;
    if(GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_CONNECTIONS,0)!=ERROR_INSUFFICIENT_BUFFER) return false;
    for(unsigned attempt=0;attempt<4;++attempt) {
        if(bytes>1024*1024-4096) return false;
        std::vector<unsigned char> buffer(bytes+4096);bytes=static_cast<DWORD>(buffer.size());
        const auto result=GetExtendedTcpTable(buffer.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_CONNECTIONS,0);
        if(result==ERROR_INSUFFICIENT_BUFFER) continue;
        const auto offset=offsetof(MIB_TCPTABLE_OWNER_PID,table);
        if(result!=NO_ERROR || bytes<offset || bytes>buffer.size()) return false;
        DWORD count=0;std::memcpy(&count,buffer.data(),sizeof count);
        if(count>(bytes-offset)/sizeof(MIB_TCPROW_OWNER_PID)) return false;
        for(DWORD index=0;index<count;++index) {
            MIB_TCPROW_OWNER_PID row{};
            std::memcpy(&row,buffer.data()+offset+index*sizeof row,sizeof row);
            if(row.dwState==MIB_TCP_STATE_ESTAB && row.dwLocalAddr==peer.sin_addr.s_addr && row.dwRemoteAddr==local.sin_addr.s_addr &&
               (row.dwLocalPort&0xffff)==peer.sin_port && (row.dwRemotePort&0xffff)==local.sin_port)
                return row.dwOwningPid==expected;
        }
        return false;
    }
    return false;
}
}
