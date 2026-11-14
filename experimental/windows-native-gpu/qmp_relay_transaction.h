// SPDX-License-Identifier: MIT
#pragma once
#include "qmp_protocol.h"
#include <cstdint>

namespace driver_qmp {
// Both correlated requests consume the same deadline. Receiving the command
// acknowledgement must not restart the budget for the subsequent state query.
template<class Send>
Json relayTransaction(Send&& send, const std::string& instruction, std::uint64_t deadline) {
    const auto acknowledgement=send("qom-set",Json{{"path","/machine/peripheral/relay"},
        {"property","relay-command"},{"value",instruction}},deadline);
    if(!acknowledgement.is_object() || !acknowledgement.empty())
        throw std::runtime_error("SDL graphics acknowledgement invalid");
    const auto encoded=send("qom-get",Json{{"path","/machine/peripheral/relay"},
        {"property","relay-state"}},deadline);
    auto state=Json::parse(encoded.template get<std::string>());
    if(!state.is_object()) throw std::runtime_error("SDL graphics state is not an object");
    return state;
}
}
