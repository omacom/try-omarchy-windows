// SPDX-License-Identifier: MIT
#include "input_delivery_diagnostic.h"
#include <chrono>
#include <iostream>
#include <vector>
using driver_input::Event;
using driver_input::Kind;
using driver_input::Channel;
using driver_qmp::Json;
static void require(bool value) { if(!value) throw std::runtime_error("Input control failed"); }
struct Transport {
    std::mutex guard;
    std::condition_variable ready;
    std::vector<Json> records;
    bool blocked=false,unblock=false,failFirst=false;
    void send(const Json& value) {
        std::unique_lock<std::mutex> lock(guard);records.push_back(value);ready.notify_all();
        if(blocked && records.size()==1) require(ready.wait_for(lock,std::chrono::seconds(5),[this]{return unblock;}));
        if(failFirst && records.size()==1) throw std::runtime_error("Owned transport failed");
    }
    void wait(std::size_t count) {
        std::unique_lock<std::mutex> lock(guard);
        require(ready.wait_for(lock,std::chrono::seconds(5),[this,count]{return records.size()>=count;}));
    }
    void release() { std::lock_guard<std::mutex> lock(guard);unblock=true;ready.notify_all(); }
};
int main() {
    Event position{};
    require(driver_input::absolute(1279,719,1280,720,position) && position.x==32767 && position.y==32767);
    require(driver_input::absolute(-200,99999,3840,2160,position) && position.x==0 && position.y==32767);
    require(!driver_input::absolute(0,0,1,2160,position) && !driver_input::absolute(0,0,3840,65537,position));
    require(std::string(driver_input::key(0x1e))=="a" && std::string(driver_input::key(0x9d))=="ctrl_r");
    require(std::string(driver_input::key(0x57))=="f11" && std::string(driver_input::key(0xdb))=="meta_l");
    require(!driver_input::key(0) && !driver_input::key(256) && !driver_input::valid({Kind::Absolute,0,32768,0}));
    {
        Transport t;Channel c([&](const Json& v){t.send(v);});
        require(c.post({Kind::Key,0x1e,0,0,true}));t.wait(1);
        require(c.stop() && !c.post({Kind::Key,0x1e,0,0,true}));
        require(t.records.size()==2 && t.records[1]["events"][0]["data"]["down"]==false);
        require(c.acknowledgements()==2 && c.resetCount()==1);
    }
    {
        Transport t;t.blocked=true;Channel c([&](const Json& v){t.send(v);});
        require(c.post({Kind::Key,0x1d,0,0,true}));t.wait(1);
        for(int n=0;n<1000;++n) require(c.post({Kind::Absolute,0,n,n}));
        require(c.post({Kind::Key,0x1e,0,0,true}) && c.post({Kind::Key,0x1e,0,0,false}));
        for(int n=1000;n<2000;++n) require(c.post({Kind::Absolute,0,n,n}));
        require(c.coalescedMotions()==1998);t.release();t.wait(5);require(c.stop());
        require(t.records[1]["events"][0]["data"]["value"]==999);
        require(t.records[2]["events"][0]["type"]=="key" && t.records[3]["events"][0]["data"]["down"]==false);
        require(t.records[4]["events"][0]["data"]["value"]==1999);
        require(t.records.back()["events"][0]["data"]["key"]["data"]=="ctrl");
    }
    {
        Transport t;t.blocked=true;Channel c([&](const Json& v){t.send(v);});
        require(c.post({Kind::Key,0x1d,0,0,true}));t.wait(1);
        require(c.post({Kind::Key,0x1e,0,0,true}) && c.post({Kind::Reset}));
        t.release();t.wait(2);require(c.stop());
        require(t.records.size()==2 && t.records[1]["events"][0]["data"]["key"]["data"]=="ctrl");
    }
    {
        Transport t;t.blocked=true;Channel c([&](const Json& v){t.send(v);});
        require(c.post({Kind::Button,0,0,0,true}));t.wait(1);
        for(std::size_t n=0;n<Channel::Capacity;++n) require(c.post({Kind::Key,0x1e,0,0,(n%2)==0}));
        require(!c.post({Kind::Key,0x1e,0,0,true}) && !c.healthy());
        t.release();require(!c.stop());
        require(t.records.size()==2 && t.records[1]["events"][0]["data"]["button"]=="left" &&
            t.records[1]["events"][0]["data"]["down"]==false);
    }
    {
        Transport t;t.failFirst=true;Channel c([&](const Json& v){t.send(v);});
        require(c.post({Kind::Key,0x1e,0,0,true}));t.wait(2);require(!c.stop());
        require(t.records[1]["events"][0]["data"]["down"]==false);
    }
    {
        Transport t;Channel c([&](const Json& v){t.send(v);});
        require(c.post({Kind::Wheel,5}));t.wait(1);require(c.stop());
        require(t.records.size()==1 && t.records[0]["events"].size()==2 &&
            t.records[0]["events"][0]["data"]["down"]==true && t.records[0]["events"][1]["data"]["down"]==false);
    }
    {
        Transport t;Channel c([&](const Json& v){t.send(v);});
        require(!c.post({Kind::Key,0}) && !c.stop() && t.records.empty());
    }
    {
        Transport t;Channel c([&](const Json& v){t.send(v);});
        require(driver_input::deliveryDiagnostic(c));t.wait(33);require(c.stop());
        require(t.records.size()==33 && t.records[30]["events"][0]["data"]["value"]==20000 &&
            t.records[31]["events"][0]["data"]["down"]==true && t.records[32]["events"][0]["data"]["down"]==false);
    }
    std::cout<<"INPUT_ORDER_FOCUS_OVERFLOW_RELEASE_AND_TRANSPORT_CONTROLS_PASS\n";
}
