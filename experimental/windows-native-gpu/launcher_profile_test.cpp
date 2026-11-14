// SPDX-License-Identifier: MIT
#define main owned_qemu_fixture_main
#include "owned_qemu_process_test.cpp"
#undef main
#include "launcher_profile.h"
#include <fstream>
using Json = nlohmann::json;
using Profile = driver_launcher::Profile;

static Json fixture() {
    return {{"protocol",1},{"qemu","C:\\Launcher Fixture\\qemu.exe"},{"auditDirectory","C:\\Launcher Fixture\\audit"},
        {"arguments",{"-machine","q35,accel=whpx","-cpu","host","-smp","4","-m","6144M",
            "-device","virtio-vga-gl,blob=on,hostmem=1073741824","-display","sdl,gl=on,show-cursor=off,window-close=off",
            "-kernel","C:\\Launcher Fixture\\kernel","-initrd","C:\\Launcher Fixture\\initrd","-append","root=/dev/vda quiet",
            "-drive","file=C:\\Launcher Fixture\\root.qcow2,format=qcow2,if=virtio","-serial","file:C:\\Launcher Fixture\\serial.log",
            "-device","virtio-keyboard-pci","-device","virtio-tablet-pci","-qmp","unix:C:\\controls\\keyboard,server=on,wait=off",
            "-audiodev","none,id=snd","-name","Try Omarchy","-no-reboot"}}};
}
template<class F> static void invalidProfile(F&& call) {
    bool failed=false;try { call(); } catch(const std::exception&) { failed=true; }
    require(failed,"Invalid launcher profile accepted");
}
static void preserved(const Profile& profile) {
    const auto original=profile.arguments;
    for (const auto mode : {driver_qemu_args::BootDisplay::None,driver_qemu_args::BootDisplay::StandardVga}) {
        const auto args=profile.compose({123456,23456,512,1024},std::wstring(32,L'a'),std::wstring(16,L'b'),34567,45678,mode);
        auto expected=original;
        unsigned replaced=0;
        for(std::size_t i=0;i+1<expected.size();++i) if(expected[i]==L"-device" && expected[i+1].find(L"virtio-vga-gl")==0) {
            expected[i]=mode==driver_qemu_args::BootDisplay::None ? L"-vga" : L"-device";
            expected[i+1]=mode==driver_qemu_args::BootDisplay::None ? L"none" : L"VGA,id=wddm-boot-display";++replaced;
        }
        require(replaced==1 && args.size()==original.size()+17 && args.front()==profile.executable &&
            std::equal(expected.begin(),expected.end(),args.begin()+1),"Original boot/input argument changed");
        require(profile.arguments==original,"Composer mutated original argument vector");
        require(args[args.size()-4]==L"-qmp" && args[args.size()-3]==L"tcp:127.0.0.1:34567,server=on,wait=off" &&
            args[args.size()-2]==L"-qmp" && args.back()==L"tcp:127.0.0.1:45678,server=on,wait=off","Private QMP roles missing");
        const auto relay=driver_qemu_args::relayDevice(std::wstring(32,L'a'),std::wstring(16,L'b'),profile.auditDirectory.wstring());
        require(args[args.size()-5]==relay+(mode==driver_qemu_args::BootDisplay::StandardVga ? L",boot-display=wddm-boot-display" : L""),
            "Boot console relay binding changed");
    }
    invalidProfile([&]{profile.compose({123456,23456,512,1024},std::wstring(32,L'a'),std::wstring(16,L'b'),34567,45678,
        static_cast<driver_qemu_args::BootDisplay>(2));});
}
int main(int argc,char** argv) {
    if(argc==4 && std::string(argv[1])=="--owned-child") return owned_qemu_fixture_main(argc,argv);
    try {
        if(argc==2) {
            std::ifstream input(argv[1],std::ios::binary);Json rows;input>>rows;
            require(rows.is_array() && rows.size()==6,"Wrong Go profile export");
            unsigned accepted=0,rejected=0;
            for(const auto& row:rows) {
                bool valid=false;
                try {
                    const auto profile=Profile::parse(row.at("encoded").get<std::string>());preserved(profile);
                    std::vector<std::wstring> expected;
                    for(const auto& value:row.at("original")) expected.push_back(Profile::wide(value.get<std::string>()));
                    require(profile.arguments==expected && profile.memoryMiB==6144,"Go-to-native argument roundtrip changed");valid=true;
                } catch(const std::invalid_argument&) {}
                require(valid==row.at("supported").get<bool>(),"Go/native profile compatibility differs");
                if(valid) ++accepted;else ++rejected;
            }
            require(accepted==3 && rejected==3,"Missing Go profile compatibility control");
            std::cout<<Json{{"actualGoProfiles",6},{"accepted",accepted},{"rejected",rejected},{"unicodeRoundtrip",true},
                {"originalBootInputPreserved",true},{"qemuStarted",false},{"gpu",false},{"normalGuestBootVerified",false}}.dump()<<'\n';
            return 0;
        }
        require(argc==1,"Unexpected profile-test arguments");
        auto json=fixture();const auto parsed=Profile::parse(json.dump(-1,' ',true));
        require(parsed.memoryMiB==6144 && !parsed.hostCursor && parsed.machineName=="Try Omarchy","Launcher facts lost");preserved(parsed);
        json["qemu"]=u8"C:\\GPU \u8a66\u9a13\U0001f642\\qemu.exe";
        require(Profile::parse(json.dump(-1,' ',true)).executable==L"C:\\GPU \u8a66\u9a13\U0001f642\\qemu.exe","Unicode executable changed");
        json=fixture();json["arguments"][11]="sdl,gl=on,show-cursor=on,window-close=off";
        require(Profile::parse(json.dump(-1,' ',true)).hostCursor,"Host cursor preference lost");
        for(const auto key:{"protocol","qemu","arguments","auditDirectory"}) {
            json=fixture();json.erase(key);invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        }
        for(const auto text:{"{}","[]","{\"protocol\":1,\"protocol\":1}","{\"arguments\":[{}]}","{\"arguments\":[[[\"x\"]]]}"})
            invalidProfile([&]{Profile::parse(text);});
        invalidProfile([&]{Profile::parse(std::string(20001,' '));});
        invalidProfile([&]{Profile::wide(std::string("a\0b",3));});
        invalidProfile([&]{Profile::wide(std::string("\xc0\xaf",2));});
        for(const auto key:{"qemu","auditDirectory"}) {
            json=fixture();json[key]="relative-path";invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        }
        json=fixture();json["future"]=true;invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        json=fixture();json["protocol"]=true;invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        json=fixture();json["arguments"].push_back("-nographic");invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        json=fixture();json["arguments"].push_back("-name");json["arguments"].push_back("other");invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        json=fixture();json["arguments"][7]="1048577M";invalidProfile([&]{Profile::parse(json.dump(-1,' ',true));});
        invalidProfile([&]{parsed.compose({1,23456,512,1024},std::wstring(32,L'a'),std::wstring(16,L'b'),34567,34567);});
        {
            Child child;child.resume();unsigned calls=0;
            driver_launcher::bootHeadStart(child.guard.handle.value,40,[&]{++calls;});
            require(calls>0 && !child.guard.exited(),"Boot head start changed child lifetime");
            invalidProfile([&]{driver_launcher::bootHeadStart(child.guard.handle.value,1000,[]{throw std::runtime_error("Creator cancelled");});});
            require(!child.guard.exited(),"Creator check stopped a child without its owner");
            child.guard.stop(0);
        }
        std::cout<<"LAUNCHER_PROFILE_PASS checks="<<checks<<" originalBootInputPreserved=true unicode=true privateQmp=true bootHeadStartCancellation=true qemu=false gpu=false windows=0\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"LAUNCHER_PROFILE_FAIL "<<error.what()<<'\n';return 1; }
}
