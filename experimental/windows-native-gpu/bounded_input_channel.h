// SPDX-License-Identifier: MIT
// Window input never waits for QMP or shares the GPU mapping connection.
#pragma once
#include "qmp_protocol.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
namespace driver_input {
enum class Kind { Key, Button, Absolute, Wheel, Reset };
struct Event { Kind kind; unsigned code=0; int x=0,y=0; bool down=false; };
inline const char* key(unsigned scan) {
    static constexpr const char* basic[]={nullptr,"esc","1","2","3","4","5","6","7","8","9","0","minus","equal","backspace","tab",
        "q","w","e","r","t","y","u","i","o","p","bracket_left","bracket_right","ret","ctrl","a","s","d","f","g","h","j","k","l",
        "semicolon","apostrophe","grave_accent","shift","backslash","z","x","c","v","b","n","m","comma","dot","slash","shift_r","kp_multiply",
        "alt","spc","caps_lock","f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","num_lock","scroll_lock","kp_7","kp_8","kp_9",
        "kp_subtract","kp_4","kp_5","kp_6","kp_add","kp_1","kp_2","kp_3","kp_0","kp_decimal",nullptr,nullptr,"less","f11","f12"};
    if(scan<std::size(basic)) return basic[scan];
    switch(scan) {
    case 0x9c:return "kp_enter";case 0x9d:return "ctrl_r";case 0xb5:return "kp_divide";case 0xb7:return "print";case 0xb8:return "alt_r";
    case 0xc7:return "home";case 0xc8:return "up";case 0xc9:return "pgup";case 0xcb:return "left";case 0xcd:return "right";
    case 0xcf:return "end";case 0xd0:return "down";case 0xd1:return "pgdn";case 0xd2:return "insert";case 0xd3:return "delete";
    case 0xdb:return "meta_l";case 0xdc:return "meta_r";case 0xdd:return "menu";case 0xff:return "pause";default:return nullptr;
    }
}
inline const char* button(unsigned code) {
    static constexpr const char* names[]={"left","middle","right","side","extra","wheel-up","wheel-down","wheel-left","wheel-right"};
    return code<std::size(names)?names[code]:nullptr;
}
inline bool valid(Event e) {
    switch(e.kind) {
    case Kind::Key:return key(e.code)!=nullptr;
    case Kind::Button:return e.code<5;
    case Kind::Absolute:return e.x>=0 && e.x<=32767 && e.y>=0 && e.y<=32767;
    case Kind::Wheel:return e.code>=5 && e.code<9;
    case Kind::Reset:return true;
    } return false;
}
inline bool absolute(int x,int y,int width,int height,Event& result) {
    if(width<2 || height<2 || width>65536 || height>65536) return false;
    const auto scale=[](int value,int extent) {
        const int clamped=value<0?0:value>=extent?extent-1:value;
        return static_cast<int>(static_cast<std::int64_t>(clamped)*32767/(extent-1));
    };
    result={Kind::Absolute,0,scale(x,width),scale(y,height),false};return true;
}
class Channel {
public:
    static constexpr std::size_t Capacity=256;
    using Send=std::function<void(const driver_qmp::Json&)>;
private:
    std::mutex guard;
    std::condition_variable ready;
    std::deque<Event> pending;
    std::thread worker;
    Send send;
    bool sealed=false;
    std::atomic<bool> failed{false};
    std::array<bool,256> keys{};
    std::array<bool,5> buttons{};
    std::atomic<std::uint64_t> acknowledged{0},coalesced{0},resets{0};
    void deliver(const driver_qmp::Json& events) {
        if(events.empty()) return;
        send(driver_qmp::Json{{"events",events}});++acknowledged;
    }
    static driver_qmp::Json keyEvent(unsigned code,bool down) {
        return {{"type","key"},{"data",{{"down",down},{"key",{{"type","qcode"},{"data",key(code)}}}}}};
    }
    static driver_qmp::Json buttonEvent(unsigned code,bool down) {
        return {{"type","btn"},{"data",{{"down",down},{"button",button(code)}}}};
    }
    void release() {
        auto events=driver_qmp::Json::array();
        // Keep each QMP command below its existing bounded message size.
        for(unsigned code=0;code<keys.size();++code) if(keys[code]) {
            events.push_back(keyEvent(code,false));
            if(events.size()==16) { deliver(events);events.clear(); }
        }
        for(unsigned code=0;code<buttons.size();++code) if(buttons[code]) events.push_back(buttonEvent(code,false));
        deliver(events);keys.fill(false);buttons.fill(false);++resets;
    }
    void run() noexcept {
        try {
            for(;;) {
                Event event{};
                { std::unique_lock<std::mutex> lock(guard);ready.wait(lock,[this]{return sealed || !pending.empty();});
                  if(sealed) break;
                  event=pending.front();pending.pop_front(); }
                if(event.kind==Kind::Reset) { release();continue; }
                auto events=driver_qmp::Json::array();
                if(event.kind==Kind::Key) {
                    if(keys[event.code]==event.down) continue;
                    // Conservative ownership even if the acknowledgement is lost.
                    if(event.down) keys[event.code]=true;
                    events.push_back(keyEvent(event.code,event.down));deliver(events);keys[event.code]=event.down;
                } else if(event.kind==Kind::Button) {
                    if(buttons[event.code]==event.down) continue;
                    if(event.down) buttons[event.code]=true;
                    events.push_back(buttonEvent(event.code,event.down));deliver(events);buttons[event.code]=event.down;
                } else if(event.kind==Kind::Absolute) {
                    events.push_back({{"type","abs"},{"data",{{"axis","x"},{"value",event.x}}}});
                    events.push_back({{"type","abs"},{"data",{{"axis","y"},{"value",event.y}}}});deliver(events);
                } else {
                    events.push_back(buttonEvent(event.code,true));events.push_back(buttonEvent(event.code,false));deliver(events);
                }
            }
            release();
        } catch(...) {
            failed=true;
            try { release(); } catch(...) { /* A failed transport cannot promise key release. */ }
        }
        { std::lock_guard<std::mutex> lock(guard);sealed=true;pending.clear(); }
    }
public:
    explicit Channel(Send transport):send(std::move(transport)) {
        if(!send) throw std::invalid_argument("Input transport required");
        worker=std::thread([this]{run();});
    }
    Channel(const Channel&)=delete;
    ~Channel() { stop(); }
    bool post(Event event) noexcept {
        try {
            std::lock_guard<std::mutex> lock(guard);
            if(sealed || failed.load()) return false;
            if(!valid(event)) { failed=true;sealed=true;pending.clear();ready.notify_one();return false; }
            if(event.kind==Kind::Reset) pending.clear();
            if(event.kind==Kind::Absolute && !pending.empty() && pending.back().kind==Kind::Absolute) {
                pending.back()=event;++coalesced;return true;
            }
            if(pending.size()==Capacity) { failed=true;sealed=true;pending.clear();ready.notify_one();return false; }
            pending.push_back(event);ready.notify_one();return true;
        } catch(...) { failed=true;return false; }
    }
    bool healthy() const noexcept { return !failed.load(); }
    bool stop() noexcept {
        { std::lock_guard<std::mutex> lock(guard);sealed=true;pending.clear(); }
        ready.notify_one();if(worker.joinable()) worker.join();return healthy();
    }
    std::uint64_t acknowledgements() const noexcept { return acknowledged.load(); }
    std::uint64_t coalescedMotions() const noexcept { return coalesced.load(); }
    std::uint64_t resetCount() const noexcept { return resets.load(); }
};
}
