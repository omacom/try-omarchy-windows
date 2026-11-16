// SPDX-License-Identifier: MIT
#include "qmp_protocol.h"
#include <iostream>
#include <stdexcept>
using namespace driver_qmp;
static void require(bool value) { if (!value) throw std::runtime_error("QMP protocol test failed"); }
template<class Function> static void rejected(Function function) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid QMP message accepted");
}
int main() {
    require(parse(R"({"QMP":{"version":{"qemu":{"major":11}},"capabilities":[]}})").contains("QMP"));
    require(result(parse(R"({"return":{},"id":"fence-1"})"), "fence-1").is_object());
    require(event(parse(R"({"event":"STOP","timestamp":{"seconds":1}})")));
    require(!event(parse(R"({"event":"STOP","id":"fence-1"})")));
    rejected([] { result(parse(R"({"return":{},"id":"other"})"), "fence-1"); });
    rejected([] { result(parse(R"({"return":{},"id":1})"), "fence-1"); });
    rejected([] { result(parse(R"({"return":{},"error":{},"id":"fence-1"})"), "fence-1"); });
    rejected([] { result(parse(R"({"error":{"class":"GenericError"},"id":"fence-1"})"), "fence-1"); });
    for (const auto& text : {std::string{}, std::string{"[]"}, std::string{"{}{}"},
                            std::string{"{\"id\":\"a\",\"id\":\"b\"}"},
                            std::string{"{\"nested\":{\"a\":1,\"a\":2}}"}, std::string(MaxMessage + 1, ' ')})
        rejected([&] { parse(text); });
    rejected([] { parse("{\"x\":" + std::string(18, '[') + "0" + std::string(18, ']') + "}"); });
    require(parse(R"({"a":{"x":1},"b":{"x":2},"arr":[{"x":3},{"x":4}]})").at("b").at("x") == 2);
    require(parse(R"({"unicode":"\ud83d\ude80","escaped":"quote\" and slash\\"})").at("unicode").is_string());
    std::cout << "PASS: bounded QMP JSON, duplicate-key rejection and exact reply correlation\n";
}
