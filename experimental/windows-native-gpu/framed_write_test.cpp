// SPDX-License-Identifier: MIT
#include "framed_write.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

static void require(bool value) { if(!value) throw std::runtime_error("Framed write control failed"); }
int main() {
    require(driver_transport::tcpNoDelayEnabled(1,1) && driver_transport::tcpNoDelayEnabled(1,4) &&
            driver_transport::tcpNoDelayEnabled(256,2) && !driver_transport::tcpNoDelayEnabled(0,1) &&
            !driver_transport::tcpNoDelayEnabled(1,0) && !driver_transport::tcpNoDelayEnabled(1,5) &&
            !driver_transport::tcpNoDelayEnabled(1,-1));
    const std::string payload="GPU RPC reply: payload remains owned until send completes";
    const auto length=static_cast<std::uint32_t>(payload.size());
    const auto expected=std::string(reinterpret_cast<const char*>(&length),sizeof length)+payload;
    for(const auto chunk:std::array<std::size_t,7>{1,2,3,4,5,17,65536}) {
        std::string received;unsigned calls=0;
        driver_transport::writeFrame(payload.data(),payload.size(),[&](const driver_transport::WriteBuffer* buffers,std::size_t count) {
            ++calls;require(count>=1 && count<=2);
            auto left=chunk;std::size_t sent=0;
            for(std::size_t index=0;index<count && left;++index) {
                const auto bytes=(std::min)(left,buffers[index].size);
                received.append(buffers[index].bytes,bytes);left-=bytes;sent+=bytes;
            }
            return sent;
        });
        require(received==expected && calls==(expected.size()+chunk-1)/chunk);
    }
    unsigned calls=0;bool rejected=false;
    try { driver_transport::writeFrame(payload.data(),payload.size(),[&](const auto*,std::size_t) { ++calls;return std::size_t{0}; }); }
    catch(const std::runtime_error&) { rejected=true; }
    require(rejected && calls==1);
    rejected=false;
    try { driver_transport::writeFrame(payload.data(),payload.size(),[&](const auto*,std::size_t) { return expected.size()+1; }); }
    catch(const std::runtime_error&) { rejected=true; }
    require(rejected);
    rejected=false;calls=0;
    try { driver_transport::writeFrame(nullptr,1,[&](const auto*,std::size_t) { ++calls;return std::size_t{1}; }); }
    catch(const std::invalid_argument&) { rejected=true; }
    require(rejected && calls==0);
    rejected=false;
    try { driver_transport::writeFrame(payload.data(),0,[&](const auto*,std::size_t) { ++calls;return std::size_t{1}; }); }
    catch(const std::invalid_argument&) { rejected=true; }
    require(rejected && calls==0);
    rejected=false;
    try { driver_transport::writeFrame(payload.data(),std::numeric_limits<std::size_t>::max(),[&](const auto*,std::size_t) { ++calls;return std::size_t{1}; }); }
    catch(const std::invalid_argument&) { rejected=true; }
    require(rejected && calls==0);
    std::cout<<"FRAMED_WRITE_CONTROL exactPrefix=true gatherWithoutPayloadCopy=true partialHeader=true partialPayload=true zeroRejected=true overrunRejected=true bounds=true winsockBoolean1To4Bytes=true gpu=false\n";
}
