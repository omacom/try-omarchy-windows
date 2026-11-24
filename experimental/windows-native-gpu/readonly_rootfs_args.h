// SPDX-License-Identifier: MIT
#pragma once
#include "qmp_protocol.h"
#include <filesystem>
#include <vector>

namespace driver_qemu {
inline std::vector<std::wstring> readonlyRootArguments(const std::filesystem::path& path) {
    if (!path.is_absolute()) throw std::runtime_error("Read-only root image must be an absolute file path");
    // ensure_ascii keeps the Windows command-line JSON ASCII while preserving
    // UTF-8 filenames, quotes, commas and backslashes without keyval parsing.
    const auto file=driver_qmp::Json{{"driver","file"},{"filename",path.u8string()},
        {"node-name","wddm-root-file"},{"read-only",true}}.dump(-1,' ',true);
    const auto raw=driver_qmp::Json{{"driver","raw"},{"file","wddm-root-file"},
        {"node-name","wddm-root"},{"read-only",true}}.dump();
    return {L"-blockdev",std::wstring(file.begin(),file.end()),L"-blockdev",std::wstring(raw.begin(),raw.end()),
        L"-device",L"virtio-blk-pci,drive=wddm-root"};
}
}
