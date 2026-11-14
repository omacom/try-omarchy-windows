// SPDX-License-Identifier: MIT
#include "readonly_rootfs_args.h"
#include <iostream>

int main() {
    const auto require=[](bool value) { if(!value) throw std::runtime_error("read-only root argument check failed"); };
    const auto path=std::filesystem::absolute(".")/std::filesystem::u8path(u8"root with spaces, quotes\" and \u03a9.ext4");
    const auto args=driver_qemu::readonlyRootArguments(path);
    require(args.size()==6 && args[0]==L"-blockdev" && args[2]==L"-blockdev" && args[4]==L"-device" &&
        args[5]==L"virtio-blk-pci,drive=wddm-root");
    const auto ascii=[&](const std::wstring& value) {
        std::string result;
        for(const auto c:value) {
            require(static_cast<unsigned long>(c)<128);
            result.push_back(static_cast<char>(c));
        }
        return result;
    };
    const auto file=driver_qmp::parse(ascii(args[1]));
    const auto raw=driver_qmp::parse(ascii(args[3]));
    require(file==driver_qmp::Json{{"driver","file"},{"filename",path.u8string()},{"node-name","wddm-root-file"},{"read-only",true}});
    require(raw==driver_qmp::Json{{"driver","raw"},{"file","wddm-root-file"},{"node-name","wddm-root"},{"read-only",true}});
    bool rejected=false;
    try { driver_qemu::readonlyRootArguments("relative.ext4"); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected);
    std::cout<<"PASS: read-only file/raw nodes, exact path roundtrip, ASCII JSON, fixed device and relative path rejection; gpu=false\n";
}
