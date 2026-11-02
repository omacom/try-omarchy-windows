// SPDX-License-Identifier: MIT
#include "qemu_graphics_arguments.h"
#include "driver_wire.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
using namespace driver_qemu_args;
static unsigned checks = 0;
static void require(bool value) { ++checks; if (!value) std::abort(); }
template<class F> static void rejected(F&& call) {
    bool invalid = false;
    try { call(); } catch (const std::invalid_argument&) { invalid = true; }
    require(invalid);
}
int main() {
    static_assert(driver_bridge::DefaultFenceSlots == 64 && driver_bridge::DefaultVendorCpuSlots == 16,
        "Graphics argument default capacity must match the wire contract");
    const Transport t{123456, 23456, 512, 1024};
    require(fenceDevice(t) == L"wddm-fence-hub,id=wddm-fences,source-process=123456,slot-count=512");
    require(allocationDevice(t) == L"wddm-allocation-hub,id=wddm-allocations,source-process=123456,slot-count=1024");
    require(fenceDevice({99,1,64,16}) == L"wddm-fence-hub,id=wddm-fences,source-process=99");
    require(allocationDevice({99,1,64,16}) == L"wddm-allocation-hub,id=wddm-allocations,source-process=99");
    const std::wstring session(32, L'a'), luid(16, L'b');
    require(relayDevice(session,luid,L"C:\\Owned, audits\\frame") == L"wddm-sdl-relay,id=relay,session=" + session +
        L",generation=1,adapter-luid=" + luid + L",audit-directory=C:\\Owned,, audits\\frame,source-format=bgra8");
    // Unrelated options include quoted paths, the writable guest, bridges,
    // audio, QMP roles and the unchanged official keyboard/tablet/display.
    const std::vector<std::wstring> original{L"-machine",L"q35,accel=whpx",L"-cpu",L"host",L"-m",L"6144M",
        L"-device",L"virtio-vga-gl,blob=on,hostmem=1073741824", L"-display",L"sdl,gl=on,show-cursor=off,window-close=off",
        L"-drive",L"file=C:\\My Guest\\root.qcow2,format=qcow2,if=virtio",L"-kernel",L"C:\\Guest\\vmlinuz-linux",
        L"-initrd",L"C:\\Guest\\initramfs-linux.img",L"-append",L"root=/dev/vda tryomarchy.render=gpu",
        L"-device",L"virtio-keyboard-pci",L"-device",L"virtio-tablet-pci",L"-qmp",L"unix:C:\\Controls\\keyboard,server=on,wait=off",
        L"-audiodev",L"sdl,id=snd",L"-device",L"virtio-sound-pci,audiodev=snd",L"-netdev",L"user,id=n0",
        L"-device",L"virtio-net-pci,netdev=n0,romfile=",L"-chardev",L"socket,id=cam0,host=127.0.0.1,port=1234,reconnect-ms=1000",
        L"-device",L"virtio-serial-pci,id=virtioserial0",L"-device",L"virtserialport,chardev=cam0,name=dev.tryomarchy.camera",
        L"-global",L"ICH9-LPC.disable_s3=1",L"-no-reboot",L"-name",L"Try Omarchy"};
    auto expected = original;
    expected[6] = L"-vga"; expected[7] = L"none";
    auto composed = replaceLauncherGraphics(original,t,session,luid,L"C:\\Audit");
    require(composed.size() > original.size() && std::equal(expected.begin(),expected.end(),composed.begin()));
    require(original[6] == L"-device" && original[7].find(L"virtio-vga-gl") == 0);
    auto cursor = original; cursor[9] = L"sdl,gl=on,show-cursor=on,window-close=off";
    require(replaceLauncherGraphics(cursor,t,session,luid,L"C:\\Audit")[9] == cursor[9]);
    for (const auto& profile : {original,cursor}) {
        auto bootExpected = profile;
        bootExpected[6] = L"-device"; bootExpected[7] = L"VGA,id=wddm-boot-display";
        const auto boot = replaceLauncherGraphics(profile,t,session,luid,L"C:\\Audit",BootDisplay::StandardVga);
        require(boot.size() == composed.size() && std::equal(bootExpected.begin(),bootExpected.end(),boot.begin()));
        require(boot.back() == composed.back() + L",boot-display=wddm-boot-display");
    }
    rejected([&]{replaceLauncherGraphics(original,t,session,luid,L"C:\\Audit",static_cast<BootDisplay>(2));});
    for (const auto& suffix : std::vector<std::vector<std::wstring>>{{L"-device",L"virtio-keyboard-pci"},
        {L"-display",original[9]}, {L"-device",L"wddm-fence-hub"}, {L"-device",L"virtio-gpu-pci"},
        {L"-device",L"VGA"}, {L"-device",L"qxl-vga"}, {L"-device",L"{\"driver\":\"VGA\"}"},
        {L"-device",L"virtio-serial-pci,id=bridge-serial"}, {L"-chardev",L"socket,id=wddm,port=1"},
        {L"-device",L"virtio-serial-pci,id=wddm-boot-display"}, {L"-vga",L"std"}, {L"-device"}}) {
        auto bad = original; append(bad,suffix);
        rejected([&]{replaceLauncherGraphics(bad,t,session,luid,L"C:\\Audit");});
        rejected([&]{replaceLauncherGraphics(bad,t,session,luid,L"C:\\Audit",BootDisplay::StandardVga);});
    }
    for (const auto& display : {L"none",L"sdl,gl=off",L"sdl,gl=on,show-cursor=off,window-close=on"}) {
        auto bad = original; bad[9] = display;
        rejected([&]{replaceLauncherGraphics(bad,t,session,luid,L"C:\\Audit");});
    }
    for (const auto& graphics : {L"virtio-vga-gl,venus=on",L"virtio-vga-gl,max_outputs=2",L"virtio-gpu-pci",L"VGA"}) {
        auto bad = original; bad[7] = graphics;
        rejected([&]{replaceLauncherGraphics(bad,t,session,luid,L"C:\\Audit");});
    }
    for (const auto bad : {Transport{0,1,64,16},Transport{1,0,64,16},Transport{1,65536,64,16},
        Transport{1,1,63,16},Transport{1,1,64,17}}) rejected([&]{fenceDevice(bad);});
    rejected([&]{relayDevice(session,L"bad",L"C:\\Audit");});
    rejected([&]{relayDevice(L"bad",luid,L"C:\\Audit");});
    rejected([&]{relayDevice(session,luid,L"C:\\Audit\nmalformed");});
    rejected([&]{relayDevice(session,luid,std::wstring(L"a\0b",3));});
    std::printf("QEMU_GRAPHICS_ARGUMENTS_PASS checks=%u originalInputPreserved=true processHandoffVerified=false gpu=false\n",checks);
}
