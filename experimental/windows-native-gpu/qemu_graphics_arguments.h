// SPDX-License-Identifier: MIT
// Graphics transport arguments, independent of guest boot and official input.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace driver_qemu_args {
struct Transport {
    std::uintptr_t sourceProcess;
    unsigned port, fenceSlots, allocationSlots;
};
enum class BootDisplay { None, StandardVga };
inline void validate(const Transport& t) {
    const bool fences = t.fenceSlots == 64 || t.fenceSlots == 128 || t.fenceSlots == 256 || t.fenceSlots == 512;
    const bool allocations = t.allocationSlots == 16 || t.allocationSlots == 32 || t.allocationSlots == 64 ||
        t.allocationSlots == 128 || t.allocationSlots == 256 || t.allocationSlots == 512 || t.allocationSlots == 1024;
    if (!t.sourceProcess || !t.port || t.port > 65535 || !fences || !allocations)
        throw std::invalid_argument("Invalid native graphics transport endpoints");
}
inline std::wstring fenceDevice(const Transport& t) {
    validate(t);
    return L"wddm-fence-hub,id=wddm-fences,source-process=" + std::to_wstring(t.sourceProcess) +
        (t.fenceSlots == 64 ? L"" : L",slot-count=" + std::to_wstring(t.fenceSlots));
}
inline std::wstring allocationDevice(const Transport& t) {
    validate(t);
    return L"wddm-allocation-hub,id=wddm-allocations,source-process=" + std::to_wstring(t.sourceProcess) +
        (t.allocationSlots == 16 ? L"" : L",slot-count=" + std::to_wstring(t.allocationSlots));
}
inline std::vector<std::wstring> guestTransport(const Transport& t) {
    validate(t);
    return {L"-device", L"virtio-serial-pci,id=bridge-serial", L"-chardev",
        L"socket,id=wddm,host=127.0.0.1,nodelay=on,port=" + std::to_wstring(t.port), L"-device",
        L"virtserialport,bus=bridge-serial.0,chardev=wddm,name=org.7wdev.wddm"};
}
inline bool hexText(const std::wstring& text, std::size_t size) {
    if (text.size() != size) return false;
    for (const auto c : text) if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) return false;
    return true;
}
inline std::wstring relayDevice(const std::wstring& session, const std::wstring& luid, const std::wstring& auditDirectory) {
    if (!hexText(session, 32) || !hexText(luid, 16) || auditDirectory.empty() ||
        auditDirectory.find_first_of(std::wstring{L'\0', L'\r', L'\n'}) != std::wstring::npos)
        throw std::invalid_argument("Invalid native SDL relay endpoints");
    std::wstring escaped;
    for (const auto c : auditDirectory) { escaped += c; if (c == L',') escaped += c; }
    return L"wddm-sdl-relay,id=relay,session=" + session + L",generation=1,adapter-luid=" + luid +
        L",audit-directory=" + escaped + L",source-format=bgra8";
}
inline void append(std::vector<std::wstring>& args, const std::vector<std::wstring>& additions) {
    args.insert(args.end(), additions.begin(), additions.end());
}

// This composer is a launcher integration boundary, not an activation switch.
// Source-process inheritance, authenticated ownership, owner QMP endpoints and
// the matching guest runtime must be established by the process owner first.
// It deliberately supports only the existing single-display SDL GPU route.
inline std::vector<std::wstring> replaceLauncherGraphics(const std::vector<std::wstring>& original,
        const Transport& transport, const std::wstring& session, const std::wstring& luid, const std::wstring& auditDirectory,
        BootDisplay bootDisplay = BootDisplay::None) {
    validate(transport);
    if (bootDisplay != BootDisplay::None && bootDisplay != BootDisplay::StandardVga)
        throw std::invalid_argument("Unsupported native graphics boot display");
    const auto relay = relayDevice(session, luid, auditDirectory) +
        (bootDisplay == BootDisplay::StandardVga ? L",boot-display=wddm-boot-display" : L"");
    unsigned displays = 0, graphics = 0, keyboard = 0, pointer = 0;
    std::vector<std::wstring> result;
    for (std::size_t i = 0; i < original.size(); ++i) {
        const auto& option = original[i];
        if (option.find(L'\0') != std::wstring::npos) throw std::invalid_argument("NUL in launcher arguments");
        if (option == L"-device" || option == L"-display" || option == L"-chardev" || option == L"-vga") {
            if (i + 1 == original.size() || original[i + 1].empty() || original[i + 1][0] == L'-')
                throw std::invalid_argument("Missing launcher option value");
            const auto& value = original[++i];
            if (value.find(L'\0') != std::wstring::npos) throw std::invalid_argument("NUL in launcher option value");
            if (option == L"-vga") throw std::invalid_argument("Unsupported launcher VGA override");
            if (option == L"-display") {
                ++displays;
                if (value != L"sdl,gl=on,show-cursor=off,window-close=off" && value != L"sdl,gl=on,show-cursor=on,window-close=off")
                    throw std::invalid_argument("Requires official SDL GPU display options");
            }
            if (option == L"-device") {
                const auto model = value.substr(0, value.find(L','));
                if (model == L"virtio-keyboard-pci") ++keyboard;
                if (model == L"virtio-tablet-pci") ++pointer;
                if (model == L"virtio-vga-gl") {
                    if (value.find(L",venus=") != std::wstring::npos || value.find(L",max_outputs=") != std::wstring::npos)
                        throw std::invalid_argument("Unsupported Vulkan or multiple-display launcher profile");
                    ++graphics;
                    if (bootDisplay == BootDisplay::StandardVga)
                        result.insert(result.end(), {L"-device", L"VGA,id=wddm-boot-display"});
                    else
                        result.insert(result.end(), {L"-vga", L"none"});
                    continue;
                }
                if (value[0] == L'{' || model == L"VGA" || model == L"qxl" || model == L"qxl-vga" ||
                    model == L"secondary-vga" || model == L"bochs-display" || model == L"ramfb" || model == L"vmware-svga" ||
                    model == L"cirrus-vga" || model == L"ati-vga" ||
                    model.find(L"wddm-") == 0 || model.find(L"virtio-gpu") == 0 || model.find(L"virtio-vga") == 0 ||
                    value.find(L",id=bridge-serial") != std::wstring::npos || value.find(L",id=relay") != std::wstring::npos ||
                    value.find(L",id=wddm-boot-display") != std::wstring::npos)
                    throw std::invalid_argument("Conflicting graphics or transport device");
            }
            if (option == L"-chardev" && (value.find(L",id=wddm") != std::wstring::npos || value.find(L",id=graphics-trace") != std::wstring::npos))
                throw std::invalid_argument("Conflicting graphics transport channel");
            result.insert(result.end(), {option, value});
        } else {
            result.push_back(option);
        }
    }
    if (displays != 1 || graphics != 1 || keyboard != 1 || pointer != 1)
        throw std::invalid_argument("Requires one official display, keyboard and tablet");
    result.insert(result.end(), {L"-device", fenceDevice(transport)});
    append(result, guestTransport(transport));
    result.insert(result.end(), {L"-device", allocationDevice(transport), L"-device", relay});
    return result;
}
}
