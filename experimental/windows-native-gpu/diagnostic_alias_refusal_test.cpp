// SPDX-License-Identifier: MIT
// State/sequence controls only. No GPU operation or completion is mocked here.
#include "diagnostic_alias_refusal.h"
#include <iostream>
#define CHECK(c) do { if(!(c)) throw std::runtime_error("Diagnostic refusal check failed: " #c); } while(0)
template<class F> static void rejects(F action) {
    bool refused=false;try { action(); } catch(const std::exception&) { refused=true; }CHECK(refused);
}
int main() {
    using guest_broker::DiagnosticAliasRefusal;
    bool tagged=false;
    try { throw guest_broker::DiagnosticAliasRefusalError(); }
    catch(const std::system_error& error) {
        tagged=error.code().value()==ENOMEM && dynamic_cast<const guest_broker::DiagnosticAliasRefusalError*>(&error)!=nullptr;
    }
    CHECK(tagged);
    const std::system_error ordinary(ENOMEM,std::generic_category());
    const std::exception& base=ordinary;
    CHECK(dynamic_cast<const guest_broker::DiagnosticAliasRefusalError*>(&base)==nullptr);
    rejects([] { DiagnosticAliasRefusal invalid(true,0); });
    rejects([] { DiagnosticAliasRefusal invalid(true,1701); });
    DiagnosticAliasRefusal app(false,100);
    CHECK(!app.acceptedNativeFrame(100) && !app.completedFrames() && !app.refuseResourceAlias(true));
    DiagnosticAliasRefusal primary(true,100);
    CHECK(!primary.refuseResourceAlias(true));
    rejects([&] { primary.acceptedNativeFrame(2); });CHECK(!primary.completedFrames());
    for(std::uint64_t frame=1;frame<100;++frame) {
        CHECK(!primary.acceptedNativeFrame(frame) && !primary.refuseResourceAlias(true));
    }
    rejects([&] { primary.acceptedNativeFrame(99); });
    rejects([&] { primary.acceptedNativeFrame(101); });CHECK(primary.completedFrames()==99);
    CHECK(primary.acceptedNativeFrame(100));
    CHECK(!primary.refuseResourceAlias(false)); // Sync aliases do not consume the one resource refusal.
    CHECK(primary.refuseResourceAlias(true) && !primary.refuseResourceAlias(true));
    for(std::uint64_t frame=101;frame<=1800;++frame)
        CHECK(!primary.acceptedNativeFrame(frame) && !primary.refuseResourceAlias(true));
    rejects([&] { primary.acceptedNativeFrame(1801); });CHECK(primary.completedFrames()==1800);
    DiagnosticAliasRefusal late(true,1700);
    for(std::uint64_t frame=1;frame<=1700;++frame) CHECK(late.acceptedNativeFrame(frame)==(frame==1700));
    CHECK(late.refuseResourceAlias(true));
    std::cout<<"DIAGNOSTIC_ALIAS_REFUSAL primaryOnly=true acceptedFramesSequential=true oneResourceRefusal=true syncAliasesUnchanged=true invalidThresholdRejected=true injectedOriginTyped=true nativeCallsMade=false gpuProof=false\n";
}
