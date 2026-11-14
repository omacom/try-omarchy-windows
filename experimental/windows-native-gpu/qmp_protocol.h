// SPDX-License-Identifier: MIT
#pragma once
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "third_party/nlohmann-json/json.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <array>
#include <set>
#include <stdexcept>
#include <string>
namespace driver_qmp {
using Json = nlohmann::json;
constexpr std::size_t MaxMessage = 8192;
inline Json parse(const std::string& text) {
    if (text.empty() || text.size() > MaxMessage) throw std::runtime_error("QMP message size invalid");
    std::array<std::set<std::string>, 18> keys;
    auto callback = [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth < 0 || depth > 16) throw std::runtime_error("QMP JSON depth exceeded");
        const auto index = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) keys[index + 1].clear();
        if (event == Json::parse_event_t::key && !keys[index].insert(value.get<std::string>()).second)
            throw std::runtime_error("QMP duplicate JSON key");
        return true;
    };
    auto message = Json::parse(text, callback);
    if (!message.is_object()) throw std::runtime_error("QMP message is not an object");
    return message;
}
inline bool event(const Json& message) {
    return message.contains("event") && message.at("event").is_string() &&
           !message.contains("return") && !message.contains("error") && !message.contains("id");
}
inline Json result(const Json& message, const std::string& id) {
    if (!message.contains("id") || !message.at("id").is_string() || message.at("id") != id ||
        message.contains("event") || message.contains("return") == message.contains("error"))
        throw std::runtime_error("QMP reply correlation invalid");
    if (message.contains("error")) throw std::runtime_error("QEMU rejected the host control command");
    return message.at("return");
}
} // namespace driver_qmp
