// SPDX-License-Identifier: MIT
#include "native_worker_protocol.h"
#include <iostream>
#include <stdexcept>
using namespace driver_worker;
static unsigned checks=0;
#define CHECK(c) do { ++checks; if (!(c)) throw std::runtime_error(#c); } while (0)
int main() {
    try {
        Request init{}; init.sequence=1; init.ownedNtHandle=32; init.luidLow=57;
        CHECK(valid(init,1,false)); CHECK(!valid(init,1,true));
        for (unsigned n=0;n<7;++n) {
            auto bad=init;
            if (n==0) ++bad.magic;
            if (n==1) ++bad.version;
            if (n==2) bad.reserved=1;
            if (n==3) bad.sequence=2;
            if (n==4) bad.ownedNtHandle=0;
            if (n==5) bad.ownedNtHandle=UINT64_MAX;
            if (n==6) bad.flags=Streaming;
            CHECK(!valid(bad,1,false));
        }
        Request consume{}; consume.operation=Operation::Consume; consume.sequence=2;
        CHECK(valid(consume,2,true)); CHECK(!valid(consume,2,false));
        consume.flags=Streaming; CHECK(valid(consume,2,true));
        for (unsigned n=0;n<6;++n) {
            auto bad=consume;
            if (n==0) bad.ownedNtHandle=32;
            if (n==1) bad.luidLow=57;
            if (n==2) bad.luidHigh=1;
            if (n==3) bad.sequence=1;
            if (n==4) bad.flags=Present;
            if (n==5) bad.operation=static_cast<Operation>(4);
            CHECK(!valid(bad,2,true));
        }
        Request stop{}; stop.operation=Operation::Stop; stop.sequence=3;
        CHECK(valid(stop,3,true)); CHECK(!valid(stop,3,false)); stop.flags=Streaming; CHECK(!valid(stop,3,true));
        Response response{}; response.operation=Operation::Consume; response.sequence=2; response.workerPid=57; response.hash=123;
        CHECK(valid(response,consume,57)); CHECK(!valid(response,consume,58));
        for (unsigned n=0;n<8;++n) {
            auto bad=response;
            if (n==0) ++bad.magic;
            if (n==1) ++bad.version;
            if (n==2) bad.sequence=1;
            if (n==3) bad.operation=Operation::Stop;
            if (n==4) bad.hresult=1;
            if (n==5) bad.hresult=-1;
            if (n==6) bad.reserved=1;
            if (n==7) bad.padding=1;
            CHECK(!valid(bad,consume,57));
        }
        std::cout<<"NATIVE_WORKER_PROTOCOL checks="<<checks<<" gpu=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
