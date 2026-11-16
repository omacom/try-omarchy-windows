// SPDX-License-Identifier: MIT
#include "guest_broker_receipts.h"
#include <atomic>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
static void require(bool ok) { if(!ok) throw std::runtime_error("Broker receipt framing regression"); }
int main() {
    int channel[2];require(!pipe(channel));
    const std::string partial="unterminated unrelated trace";
    require(write(channel[1],partial.data(),partial.size())==static_cast<ssize_t>(partial.size()));
    require(guest_broker::receiptTo(channel[1],"BROKER_TEST pid=%u authenticated=%s",123u,"true"));
    require(!guest_broker::receiptTo(channel[1],"%s",std::string(4096,'x').c_str()));
    require(!guest_broker::receiptTo(-1,"BROKER_TEST invalid=true"));
    close(channel[1]);char bytes[256];const auto count=read(channel[0],bytes,sizeof bytes);close(channel[0]);
    require(count>0 && std::string(bytes,static_cast<std::size_t>(count))==partial+"\nBROKER_TEST pid=123 authenticated=true\n");
    require(!pipe(channel));std::string captured;std::atomic<bool> failed{false};
    std::thread reader([&] { for(;;) { const auto n=read(channel[0],bytes,sizeof bytes);if(n<0) { failed=true;break; }if(!n) break;captured.append(bytes,static_cast<std::size_t>(n)); } });
    std::vector<std::thread> writers;
    for(unsigned worker=0;worker<4;++worker) writers.emplace_back([&,worker] {
        for(unsigned sequence=0;sequence<64;++sequence)
            if(!guest_broker::receiptTo(channel[1],"BROKER_TEST worker=%u sequence=%u authenticated=true",worker,sequence)) failed=true;
    });
    for(auto& writer:writers) { writer.join(); }
    close(channel[1]);reader.join();close(channel[0]);require(!failed);
    std::set<std::string> actual;std::istringstream stream(captured);std::string line;
    while(std::getline(stream,line)) if(!line.empty()) require(actual.insert(line).second);
    require(actual.size()==256);
    for(unsigned worker=0;worker<4;++worker) for(unsigned sequence=0;sequence<64;++sequence)
        require(actual.count("BROKER_TEST worker="+std::to_string(worker)+" sequence="+std::to_string(sequence)+" authenticated=true")==1);
    std::cout<<"BROKER_RECEIPT_CONTROL singleWrite=true newlinePrefix=true concurrentWriters=true truncationRejected=true gpu=false\n";
}
