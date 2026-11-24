// SPDX-License-Identifier: MIT
// Fixed, explicit owned-guest test; this never synthesizes Windows input.
#pragma once
#include "bounded_input_channel.h"
namespace driver_input {
inline bool deliveryDiagnostic(Channel& channel) {
    // Physical scan positions for omarchyinput93 followed by Return.
    constexpr unsigned scans[]={0x18,0x32,0x1e,0x13,0x2e,0x23,0x15,0x17,0x31,0x19,0x16,0x14,0x0a,0x04,0x1c};
    for(const auto scan:scans) if(!channel.post({Kind::Key,scan,0,0,true}) || !channel.post({Kind::Key,scan,0,0,false})) return false;
    return channel.post({Kind::Absolute,0,20000,20000}) && channel.post({Kind::Button,0,0,0,true}) &&
        channel.post({Kind::Button,0,0,0,false});
}
}
