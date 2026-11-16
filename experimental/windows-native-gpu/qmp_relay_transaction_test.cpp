// SPDX-License-Identifier: MIT
// Simulated command/state latency and malformed replies; no sockets or GPU.
#include "qmp_relay_transaction.h"
#include "qmp_deadline_budget.h"
#include <cassert>
#include <cstdio>
#include <vector>
struct Peer {
    std::uint64_t now=100,first=6000,second=500;
    unsigned calls=0;
    driver_qmp::Json acknowledgement=driver_qmp::Json::object();
    std::string state=R"({"attached":false})";
    std::vector<std::uint64_t> deadlines;
    driver_qmp::Json send(const std::string& operation,const driver_qmp::Json& args,std::uint64_t until) {
        ++calls;deadlines.push_back(until);
        assert(args.at("path")=="/machine/peripheral/relay");
        if(calls==1) {
            assert(operation=="qom-set" && args.at("property")=="relay-command" && args.at("value")=="detach:1");
            now+=first;
        } else {
            assert(calls==2 && operation=="qom-get" && args.at("property")=="relay-state");
            now+=second;
        }
        if(now>=until) throw std::runtime_error("QMP control deadline exceeded");
        return calls==1 ? acknowledgement : driver_qmp::Json(state);
    }
    driver_qmp::Json run() {
        const auto until=now+driver_qmp::commandTimeoutMs(driver_qmp::ChannelPurpose::SdlGraphics);
        return driver_qmp::relayTransaction([this](auto& operation,auto args,auto deadline) {
            return send(operation,args,deadline);
        },"detach:1",until);
    }
};
int main() {
    Peer success;assert(success.run().at("attached")==false);
    assert(success.deadlines==std::vector<std::uint64_t>({7100,7100}));
    Peer late;late.second=1500;
    bool rejected=false;
    try { (void)late.run(); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && late.calls==2 && late.deadlines[0]==late.deadlines[1]);
    Peer badAck;badAck.acknowledgement={{"unexpected",true}};rejected=false;
    try { (void)badAck.run(); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && badAck.calls==1);
    Peer badState;badState.state="[]";rejected=false;
    try { (void)badState.run(); } catch(const std::runtime_error&) { rejected=true; }
    assert(rejected && badState.calls==2);
    std::puts("QMP_RELAY_TRANSACTION_PASS sharedDeadline=true lateSecondReplyRejected=true acknowledgementChecked=true stateChecked=true gpu=false");
}
