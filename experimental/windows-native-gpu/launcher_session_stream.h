// SPDX-License-Identifier: MIT
// Launcher-owned idle waits; one absolute deadline once any frame byte arrives.
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <utility>
#include <functional>
#include <stdexcept>
#include <vector>

namespace driver_launcher {
class SessionStream {
    SOCKET socket;
    std::uint32_t minimum,maximum;
    DWORD packetMilliseconds;
    std::function<bool()> live;
    bool ready(ULONGLONG deadline,bool writing=false) {
        for(;;) {
            if(!live()) return false;
            const auto now=GetTickCount64();
            if(deadline && now>=deadline) throw std::runtime_error("Launcher session packet deadline exceeded");
            const auto wait=deadline ? (std::min)(ULONGLONG{100},deadline-now) : ULONGLONG{100};
            fd_set descriptors;FD_ZERO(&descriptors);FD_SET(socket,&descriptors);
            timeval timeout{0,static_cast<long>(wait*1000)};
            const auto result=select(0,writing ? nullptr : &descriptors,writing ? &descriptors : nullptr,nullptr,&timeout);
            if(result==SOCKET_ERROR) throw std::runtime_error("Launcher session transport poll failed");
            if(!result) continue;
            if(!live()) return false;
            if(deadline && GetTickCount64()>=deadline) throw std::runtime_error("Launcher session packet deadline exceeded");
            return true;
        }
    }
    bool exact(void* output,std::size_t bytes,ULONGLONG deadline,bool boundary) {
        auto* cursor=static_cast<char*>(output);std::size_t done=0;
        while(done<bytes) {
            if(!ready(deadline)) {
                // Cancellation/owned process exit may abandon a partial frame;
                // it is never dispatched. The existing owner still reaps QEMU.
                return false;
            }
            const auto count=recv(socket,cursor+done,static_cast<int>(bytes-done),0);
            if((!count || (count==SOCKET_ERROR && WSAGetLastError()==WSAECONNRESET)) && !done && boundary) return false;
            if(count==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) continue;
            if(count<=0) throw std::runtime_error("Truncated launcher session packet");
            done+=static_cast<std::size_t>(count);
            if(GetTickCount64()>=deadline) throw std::runtime_error("Launcher session packet deadline exceeded");
        }
        return true;
    }
public:
    SessionStream(SOCKET transport,std::uint32_t minimumBytes,std::uint32_t maximumBytes,
                  std::function<bool()> stillLive,DWORD packetTimeoutMilliseconds=10000)
        :socket(transport),minimum(minimumBytes),maximum(maximumBytes),packetMilliseconds(packetTimeoutMilliseconds),live(std::move(stillLive)) {
        if(socket==INVALID_SOCKET || !minimum || maximum<minimum || maximum>0x7fffffffu || !live ||
           !packetMilliseconds || packetMilliseconds>10000)
            throw std::invalid_argument("Invalid launcher session reader");
        // This is the exclusive, already authenticated owned-QEMU stream.
        // Nonblocking sends make the absolute deadline apply to backpressure,
        // rather than resetting a ten-second SO_SNDTIMEO for each partial send.
        u_long nonblocking=1;
        if(ioctlsocket(socket,FIONBIO,&nonblocking)) throw std::runtime_error("Cannot configure launcher session transport");
    }
    bool packet(std::vector<std::uint8_t>& output) {
        output.clear();
        // No periodic messages, GPU work or finite idle timer is required.
        // Startup connection and all in-progress packet deadlines stay bounded.
        if(!ready(0)) return false;
        const auto deadline=GetTickCount64()+packetMilliseconds;
        std::uint32_t bytes=0;
        if(!exact(&bytes,sizeof bytes,deadline,true)) return false;
        if(bytes<minimum || bytes>maximum) throw std::runtime_error("Invalid launcher session frame length");
        output.resize(bytes);
        if(!exact(output.data(),output.size(),deadline,false)) { output.clear();return false; }
        return true;
    }
    bool reply(const std::vector<std::uint8_t>& payload) {
        if(payload.size()<minimum || payload.size()>maximum) throw std::runtime_error("Invalid launcher session reply length");
        std::uint32_t bytes=static_cast<std::uint32_t>(payload.size());
        WSABUF views[2]{{sizeof bytes,reinterpret_cast<char*>(&bytes)},
            {bytes,reinterpret_cast<char*>(const_cast<std::uint8_t*>(payload.data()))}};
        unsigned index=0;const auto deadline=GetTickCount64()+packetMilliseconds;
        while(index<2) {
            if(!ready(deadline,true)) return false;
            DWORD sent=0;
            if(WSASend(socket,views+index,2-index,&sent,0,nullptr,nullptr)==SOCKET_ERROR) {
                if(WSAGetLastError()==WSAEWOULDBLOCK) continue;
                throw std::runtime_error("Launcher session reply write failed");
            }
            if(!sent) throw std::runtime_error("Launcher session reply made no progress");
            if(GetTickCount64()>=deadline) throw std::runtime_error("Launcher session packet deadline exceeded");
            while(index<2 && sent>=views[index].len) { sent-=views[index].len;++index; }
            if(index<2) { views[index].buf+=sent;views[index].len-=sent; }
            else if(sent) throw std::runtime_error("Invalid launcher session send count");
        }
        return true;
    }
};
}
