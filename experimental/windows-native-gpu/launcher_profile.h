// SPDX-License-Identifier: MIT
// Original launcher arguments are carried as bounded ASCII-escaped UTF-8 JSON.
#pragma once
#include <windows.h>
#include "qemu_graphics_arguments.h"
#include "third_party/nlohmann-json/json.hpp"
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace driver_launcher {
struct Profile {
    std::wstring executable;
    std::filesystem::path auditDirectory;
    std::vector<std::wstring> arguments;
    std::string machineName;
    unsigned memoryMiB = 0;
    bool hostCursor = false;

    static std::wstring wide(const std::string& text) {
        if (text.empty() || text.find_first_of(std::string{'\0','\r','\n'}) != std::string::npos || text.size() > 32760)
            throw std::invalid_argument("Invalid launcher profile string");
        const auto count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
        if (!count) throw std::invalid_argument("Invalid launcher profile UTF-8");
        std::wstring result(static_cast<std::size_t>(count),L'\0');
        if (MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count) != count)
            throw std::invalid_argument("Cannot decode launcher profile UTF-8");
        return result;
    }
    static Profile parse(const std::string& encoded) {
        if (encoded.empty() || encoded.size() > 20000) throw std::invalid_argument("Launcher profile JSON exceeds its bound");
        for (const unsigned char c : encoded) if (!c || c >= 128) throw std::invalid_argument("Launcher profile requires ASCII-escaped JSON");
        std::set<std::string> keys;
        const auto callback = [&keys](int depth,nlohmann::json::parse_event_t event,nlohmann::json& value) {
            if (depth > 2 || (event == nlohmann::json::parse_event_t::object_start && depth != 0))
                throw std::invalid_argument("Nested launcher profile object");
            if (event == nlohmann::json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
                throw std::invalid_argument("Duplicate launcher profile field");
            return true;
        };
        const auto json = nlohmann::json::parse(encoded,callback);
        if (!json.is_object() || keys != std::set<std::string>{"protocol","qemu","arguments","auditDirectory"} ||
            !json.at("protocol").is_number_unsigned() || json.at("protocol") != 1 || !json.at("qemu").is_string() ||
            !json.at("auditDirectory").is_string() || !json.at("arguments").is_array() ||
            json.at("arguments").empty() || json.at("arguments").size() > 512)
            throw std::invalid_argument("Invalid launcher profile schema");
        Profile result;
        result.executable = wide(json.at("qemu").get<std::string>());
        result.auditDirectory = wide(json.at("auditDirectory").get<std::string>());
        if (!std::filesystem::path(result.executable).is_absolute() || !result.auditDirectory.is_absolute())
            throw std::invalid_argument("Launcher profile paths must be absolute");
        for (const auto& value : json.at("arguments")) {
            if (!value.is_string()) throw std::invalid_argument("Launcher arguments must be strings");
            result.arguments.push_back(wide(value.get<std::string>()));
        }
        const std::set<std::wstring> flags{L"-no-reboot",L"-full-screen"};
        const std::set<std::wstring> values{L"-machine",L"-cpu",L"-smp",L"-m",L"-device",L"-display",L"-serial",L"-drive",
            L"-kernel",L"-initrd",L"-append",L"-chardev",L"-netdev",L"-global",L"-audiodev",L"-D",L"-name",L"-qmp",L"-virtfs",L"-L"};
        const std::set<std::wstring> unique{L"-machine",L"-cpu",L"-smp",L"-m",L"-display",L"-serial",L"-kernel",L"-initrd",L"-append",L"-name"};
        std::set<std::wstring> seen;
        unsigned disks = 0, monitors = 0;
        for (std::size_t i = 0; i < result.arguments.size(); ++i) {
            const auto& option = result.arguments[i];
            if (flags.count(option)) {
                if (!seen.insert(option).second) throw std::invalid_argument("Duplicate launcher flag");
                continue;
            }
            if (!values.count(option) || i+1 == result.arguments.size()) throw std::invalid_argument("Unsupported launcher option");
            const auto& value = result.arguments[++i];
            if (unique.count(option) && !seen.insert(option).second) throw std::invalid_argument("Duplicate launcher boot option");
            if (option == L"-machine" && value != L"q35,accel=whpx" && value != L"q35,accel=whpx,kernel-irqchip=off")
                throw std::invalid_argument("Launcher profile requires QEMU/WHPX q35");
            if (option == L"-cpu" && value != L"host") throw std::invalid_argument("Launcher profile requires the GPU CPU profile");
            if (option == L"-m") {
                if (value.size() < 2 || value.back() != L'M') throw std::invalid_argument("Invalid launcher memory format");
                unsigned memory = 0;
                for (std::size_t n = 0; n+1 < value.size(); ++n) {
                    if (value[n] < L'0' || value[n] > L'9' || memory > 104857)
                        throw std::invalid_argument("Invalid launcher memory bound");
                    memory = memory*10 + static_cast<unsigned>(value[n]-L'0');
                }
                if (memory < 512 || memory > 1048576) throw std::invalid_argument("Invalid launcher memory bound");
                result.memoryMiB = memory;
            }
            if (option == L"-name") {
                if (value.find(L',') != std::wstring::npos) throw std::invalid_argument("Compound launcher machine name");
                result.machineName = json.at("arguments").at(i).get<std::string>();
            }
            if (option == L"-display") result.hostCursor = value == L"sdl,gl=on,show-cursor=on,window-close=off";
            if (option == L"-drive") ++disks;
            if (option == L"-qmp") {
                if (value.find(L"unix:") != 0 || value.find(L",server=on,wait=off") == std::wstring::npos)
                    throw std::invalid_argument("Launcher QMP roles must remain private Unix controls");
                ++monitors;
            }
        }
        if (!seen.count(L"-no-reboot") || !seen.count(L"-name") || !seen.count(L"-machine") || !seen.count(L"-cpu") ||
            !seen.count(L"-smp") || !seen.count(L"-m") || !seen.count(L"-kernel") || !seen.count(L"-initrd") ||
            !seen.count(L"-append") || !seen.count(L"-serial") || disks != 1 || !monitors || result.machineName.empty())
            throw std::invalid_argument("Incomplete original launcher boot profile");
        // Reuse the existing tested graphics compatibility and input checks.
        (void)driver_qemu_args::replaceLauncherGraphics(result.arguments,{1,1,512,1024},std::wstring(32,L'a'),std::wstring(16,L'b'),L"C:\\profile-validation");
        return result;
    }
    std::vector<std::wstring> compose(const driver_qemu_args::Transport& transport,const std::wstring& session,
            const std::wstring& luid,unsigned ownerPort,unsigned graphicsPort,
            driver_qemu_args::BootDisplay bootDisplay = driver_qemu_args::BootDisplay::None) const {
        if (!ownerPort || ownerPort > 65535 || !graphicsPort || graphicsPort > 65535 || ownerPort == graphicsPort ||
            ownerPort == transport.port || graphicsPort == transport.port)
            throw std::invalid_argument("Invalid private launcher QMP endpoints");
        auto result = driver_qemu_args::replaceLauncherGraphics(arguments,transport,session,luid,auditDirectory.wstring(),bootDisplay);
        result.insert(result.begin(),executable);
        result.insert(result.end(),{L"-qmp",L"tcp:127.0.0.1:"+std::to_wstring(ownerPort)+L",server=on,wait=off",
            L"-qmp",L"tcp:127.0.0.1:"+std::to_wstring(graphicsPort)+L",server=on,wait=off"});
        return result;
    }
};

inline void bootHeadStart(HANDLE process,DWORD milliseconds,const std::function<void()>& requireLive) {
    const auto deadline = GetTickCount64()+milliseconds;
    while (GetTickCount64() < deadline) {
        if (requireLive) requireLive();
        if (WaitForSingleObject(process,0) != WAIT_TIMEOUT) throw std::runtime_error("Launcher QEMU exited during boot head start");
        Sleep(20);
    }
    if (requireLive) requireLive();
}
}
