// SPDX-License-Identifier: MIT
// Consume actual Go buildQemuArgs output; never execute QEMU or send input.
#include "qemu_graphics_arguments.h"
#include "third_party/nlohmann-json/json.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
using Json = nlohmann::json;
static void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Requires fresh Go launcher argument export");
        std::ifstream input(argv[1], std::ios::binary);
        require(input.good(), "Cannot open Go argument export");
        const auto profiles = Json::parse(input);
        require(profiles.is_array() && profiles.size() == 5, "Wrong export profile count");
        unsigned accepted = 0, rejected = 0;
        for (const auto& row : profiles) {
            require(row.at("name").is_string() && row.at("supported").is_boolean() && row.at("arguments").is_array(), "Invalid exported profile");
            std::vector<std::wstring> args;
            for (const auto& item : row.at("arguments")) {
                const auto text = item.get<std::string>();
                // This diagnostic deliberately supplies only ASCII test paths.
                // The production composer copies wide strings without recoding.
                require(std::all_of(text.begin(),text.end(),[](unsigned char c){return c && c < 128;}), "Unexpected fixture encoding");
                args.emplace_back(text.begin(),text.end());
            }
            const auto before = args;
            bool supported = false;
            std::vector<std::wstring> result;
            try {
                result = driver_qemu_args::replaceLauncherGraphics(args,{123456,23456,512,1024},
                    std::wstring(32,L'a'),std::wstring(16,L'b'),L"C:\\Native GPU Fixture\\audit");
                supported = true;
            } catch (const std::invalid_argument&) {}
            require(args == before && supported == row.at("supported").get<bool>(), "Unexpected mutation or compatibility decision");
            bool bootSupported = false;
            std::vector<std::wstring> bootResult;
            try {
                bootResult = driver_qemu_args::replaceLauncherGraphics(args,{123456,23456,512,1024},
                    std::wstring(32,L'a'),std::wstring(16,L'b'),L"C:\\Native GPU Fixture\\audit",
                    driver_qemu_args::BootDisplay::StandardVga);
                bootSupported = true;
            } catch (const std::invalid_argument&) {}
            require(args == before && bootSupported == supported, "Boot display changed input or compatibility decisions");
            if (!supported) { ++rejected; continue; }
            ++accepted;
            auto expected = args;
            unsigned replacements = 0;
            for (std::size_t i = 0; i + 1 < expected.size(); ++i) {
                if (expected[i] == L"-device" && expected[i + 1].find(L"virtio-vga-gl,") == 0) {
                    expected[i] = L"-vga"; expected[i + 1] = L"none"; ++replacements;
                }
            }
            require(replacements == 1 && result.size() == expected.size() + 12 &&
                std::equal(expected.begin(),expected.end(),result.begin()), "Changed non-graphics launcher arguments");
            for (std::size_t i = 0; i + 1 < expected.size(); ++i) {
                if (expected[i] == L"-vga" && expected[i + 1] == L"none") {
                    expected[i] = L"-device"; expected[i + 1] = L"VGA,id=wddm-boot-display";
                }
            }
            require(bootResult.size() == result.size() &&
                std::equal(expected.begin(),expected.end(),bootResult.begin()) &&
                bootResult.back() == result.back() + L",boot-display=wddm-boot-display",
                "Boot console changed non-graphics launcher arguments or created an independent relay display");
        }
        require(accepted == 2 && rejected == 3, "Missing positive or unsupported profile control");
        std::cout << Json{{"launcherProfilesVerified",5},{"compatibleProfiles",accepted},{"unsupportedProfilesRejected",rejected},
            {"officialArgumentsPreserved",true},{"qemuLaunched",false},{"inputInjected",false},
            {"processHandoffVerified",false},{"dailyUseAccepted",false}}.dump() << '\n';
        return 0;
    } catch (const std::exception& e) { std::cerr << "LAUNCHER_ARGUMENTS_FAIL " << e.what() << '\n'; return 1; }
}
