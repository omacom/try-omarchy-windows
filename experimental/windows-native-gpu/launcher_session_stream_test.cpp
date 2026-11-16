// SPDX-License-Identifier: MIT
// Real Windows loopback transport; no QEMU, GPU, window or input injection.
#include "launcher_session_stream.h"
#include <atomic>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
struct Network {
    Network() { WSADATA data{};require(WSAStartup(MAKEWORD(2,2),&data)==0,"WSAStartup"); }
    ~Network() { WSACleanup(); }
};
struct Pair {
    SOCKET listener=INVALID_SOCKET,sender=INVALID_SOCKET,receiver=INVALID_SOCKET;
    Pair() {
        listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener!=INVALID_SOCKET,"Listener");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        require(!bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address) && !listen(listener,1),"Listen");
        int bytes=sizeof address;require(!getsockname(listener,reinterpret_cast<sockaddr*>(&address),&bytes),"Endpoint");
        sender=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(sender!=INVALID_SOCKET,"Sender");
        require(!connect(sender,reinterpret_cast<sockaddr*>(&address),sizeof address),"Connect");
        receiver=accept(listener,nullptr,nullptr);require(receiver!=INVALID_SOCKET,"Accept");
        const DWORD timeout=2000;
        for(const auto endpoint:{sender,receiver}) {
            require(!setsockopt(endpoint,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout),"Test receive timeout");
            require(!setsockopt(endpoint,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout),"Test send timeout");
        }
    }
    void closeSender() { if(sender!=INVALID_SOCKET) { closesocket(sender);sender=INVALID_SOCKET; } }
    ~Pair() { closeSender();if(receiver!=INVALID_SOCKET) closesocket(receiver);if(listener!=INVALID_SOCKET) closesocket(listener); }
};
struct Worker {
    std::exception_ptr failure;
    std::thread thread;
    template<class F> explicit Worker(F action):thread([this,action] { try { action(); } catch(...) { failure=std::current_exception(); } }) {}
    void finish() { if(thread.joinable()) thread.join();if(failure) std::rethrow_exception(failure); }
    ~Worker() { if(thread.joinable()) thread.join(); }
};
static void sendBytes(SOCKET socket,const void* bytes,std::size_t count) {
    const auto* data=static_cast<const char*>(bytes);
    while(count) { const int n=send(socket,data,static_cast<int>(count),0);require(n>0,"Send");data+=n;count-=static_cast<std::size_t>(n); }
}
static void sendPacket(SOCKET socket,const std::vector<std::uint8_t>& payload) {
    const auto bytes=static_cast<std::uint32_t>(payload.size());sendBytes(socket,&bytes,sizeof bytes);sendBytes(socket,payload.data(),payload.size());
}
template<class F> static void fails(F action,const char* expected,const char* context="control") {
    std::string actual="no rejection";
    try { action(); } catch(const std::exception& error) { actual=error.what();if(actual.find(expected)!=std::string::npos) return; }
    throw std::runtime_error("Session transport rejection case="+std::string(context)+" expected="+std::string(expected)+" actual="+actual);
}
int main() {
    try {
        Network network;
        {
            Pair sockets;unsigned liveChecks=0;
            driver_launcher::SessionStream reader(sockets.receiver,4,64,[&] { ++liveChecks;return true; },200);
            Worker sender([&] { Sleep(350);sendPacket(sockets.sender,{1,2,3,4});Sleep(350);sendPacket(sockets.sender,{5,6,7,8,9}); });
            std::vector<std::uint8_t> packet;
            const auto start=GetTickCount64();require(reader.packet(packet) && packet==std::vector<std::uint8_t>({1,2,3,4}),"First packet after idle");
            require(reader.packet(packet) && packet==std::vector<std::uint8_t>({5,6,7,8,9}),"Separate packet after second idle");
            sender.finish();require(GetTickCount64()-start>=600 && liveChecks>=6,"Idle did not exceed packet deadline");
        }
        {
            Pair sockets;driver_launcher::SessionStream reader(sockets.receiver,4,64,[] { return true; },100);
            sockets.closeSender();std::vector<std::uint8_t> packet{42};require(!reader.packet(packet) && packet.empty(),"Clean boundary EOF");
        }
        for(bool header:{true,false}) {
            Pair sockets;driver_launcher::SessionStream reader(sockets.receiver,4,64,[] { return true; },100);
            const std::uint32_t bytes=8;
            sendBytes(sockets.sender,&bytes,header?2:sizeof bytes);
            if(!header) sendBytes(sockets.sender,"ab",2);
            sockets.closeSender();std::vector<std::uint8_t> packet;
            fails([&] { reader.packet(packet); },"Truncated launcher session packet");
        }
        for(const std::uint32_t bytes:{0u,3u,65u,0xffffffffu}) {
            Pair sockets;driver_launcher::SessionStream reader(sockets.receiver,4,64,[] { return true; },100);
            sendBytes(sockets.sender,&bytes,sizeof bytes);std::vector<std::uint8_t> packet;
            fails([&] { reader.packet(packet); },"frame length");require(packet.empty(),"Malformed length allocated payload");
        }
        for(bool header:{true,false}) {
            Pair sockets;driver_launcher::SessionStream reader(sockets.receiver,4,64,[] { return true; },100);
            Worker sender([&] {
                const std::uint32_t bytes=4;
                if(header) {
                    const auto* data=reinterpret_cast<const unsigned char*>(&bytes);
                    for(unsigned i=0;i<sizeof bytes;++i) { sendBytes(sockets.sender,data+i,1);Sleep(70); }
                    sendBytes(sockets.sender,"data",4);
                } else {
                    sendBytes(sockets.sender,&bytes,sizeof bytes);
                    for(unsigned i=0;i<4;++i) { sendBytes(sockets.sender,"x",1);Sleep(70); }
                }
            });
            std::vector<std::uint8_t> packet;const auto start=GetTickCount64();
            fails([&] { reader.packet(packet); },"packet deadline",header?"header trickle":"payload trickle");require(GetTickCount64()-start<2000,"Trickle did not honor absolute deadline");sender.finish();
        }
        {
            Pair sockets;driver_launcher::SessionStream reader(sockets.receiver,4,64,[] { return true; },100);
            Worker sender([&] {
                const std::uint32_t bytes=4;const auto* data=reinterpret_cast<const char*>(&bytes);
                sendBytes(sockets.sender,data,2);Sleep(70);sendBytes(sockets.sender,data+2,2);Sleep(70);sendBytes(sockets.sender,"data",4);
            });
            std::vector<std::uint8_t> packet;fails([&] { reader.packet(packet); },"packet deadline","combined header/payload");sender.finish();
        }
        {
            Pair sockets;driver_launcher::SessionStream stream(sockets.receiver,4,64,[] { return true; },1000);
            const std::vector<std::uint8_t> expected{4,3,2,1,9,8,7,6};std::vector<std::uint8_t> received;
            Worker consumer([&] {
                std::uint32_t bytes=0;char* header=reinterpret_cast<char*>(&bytes);unsigned done=0;
                while(done<sizeof bytes) { const int count=recv(sockets.sender,header+done,static_cast<int>(sizeof bytes-done),0);require(count>0,"Reply header");done+=static_cast<unsigned>(count); }
                require(bytes==expected.size(),"Reply size");received.resize(bytes);done=0;
                while(done<bytes) { const int count=recv(sockets.sender,reinterpret_cast<char*>(received.data()+done),static_cast<int>(bytes-done),0);require(count>0,"Reply body");done+=static_cast<unsigned>(count); }
            });
            require(stream.reply(expected),"Reply rejected");consumer.finish();require(received==expected,"Vectored reply bytes");
            fails([&] { stream.reply({1,2,3}); },"reply length");fails([&] { stream.reply(std::vector<std::uint8_t>(65)); },"reply length");
        }
        for(bool cancel:{false,true}) {
            Pair sockets;std::atomic<bool> live{true};
            const int socketBufferBytes=4096;require(!setsockopt(sockets.receiver,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&socketBufferBytes),sizeof socketBufferBytes),"Small send buffer");
            require(!setsockopt(sockets.sender,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&socketBufferBytes),sizeof socketBufferBytes),"Small receive buffer");
            driver_launcher::SessionStream stream(sockets.receiver,4,1048576,[&] { return live.load(); },cancel?1000u:100u);
            // Observe actual nonblocking backpressure; buffer-size settings
            // alone do not guarantee a large WSASend cannot be accepted.
            const std::vector<char> filler(16384,'x');std::uint64_t filled=0;bool blocked=false;
            while(filled<64ull*1024*1024) {
                const auto bytes=send(sockets.receiver,filler.data(),static_cast<int>(filler.size()),0);
                if(bytes==SOCKET_ERROR) { require(WSAGetLastError()==WSAEWOULDBLOCK,"Unexpected pressure error");blocked=true;break; }
                require(bytes>0,"Pressure made no progress");filled+=static_cast<unsigned>(bytes);
            }
            require(blocked,"No actual backpressure within bounded control");
            std::cout<<"{\"realBackpressureObserved\":true,\"filledBytes\":"<<filled<<",\"cancellationControl\":"<<(cancel?"true":"false")<<"}\n";
            Worker stop([&] { if(cancel) { Sleep(150);live.store(false); } });
            const std::vector<std::uint8_t> payload(1048576,42);
            if(cancel) require(!stream.reply(payload),"Backpressured reply cancellation");
            else fails([&] { stream.reply(payload); },"packet deadline","reply backpressure");
            stop.finish();
        }
        for(bool partial:{false,true}) {
            Pair sockets;std::atomic<bool> live{true};
            driver_launcher::SessionStream reader(sockets.receiver,4,64,[&] { return live.load(); },1000);
            if(partial) { const std::uint32_t bytes=8;sendBytes(sockets.sender,&bytes,sizeof bytes);sendBytes(sockets.sender,"x",1); }
            Worker cancel([&] { Sleep(150);live.store(false); });std::vector<std::uint8_t> packet{42};
            require(!reader.packet(packet) && packet.empty(),"Cancelled frame dispatched");cancel.finish();
        }
        {
            Pair sockets;
            fails([&] { driver_launcher::SessionStream reader(INVALID_SOCKET,4,64,[] { return true; }); },"Invalid launcher session reader");
            for(const auto timeout:{0u,10001u}) fails([&] { driver_launcher::SessionStream reader(sockets.receiver,4,64,[] { return true; },timeout); },"Invalid launcher session reader");
            fails([&] { driver_launcher::SessionStream reader(sockets.receiver,0,64,[] { return true; }); },"Invalid launcher session reader");
            fails([&] { driver_launcher::SessionStream reader(sockets.receiver,4,3,[] { return true; }); },"Invalid launcher session reader");
            fails([&] { driver_launcher::SessionStream reader(sockets.receiver,4,0x80000000u,[] { return true; }); },"Invalid launcher session reader");
            fails([&] { driver_launcher::SessionStream reader(sockets.receiver,4,64,{}); },"Invalid launcher session reader");
        }
        std::cout<<"{\"launcherSessionStreamControls\":true,\"realWindowsLoopback\":true,\"idleBeyondPacketDeadline\":true,\"trickleDeadlines\":true,\"partialFrameCancellation\":true,\"boundedReplyBackpressure\":true,\"gpu\":false,\"qemu\":false,\"hostInput\":false}\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
