#include "wire.h"
#include <iostream>
#include <stdexcept>
using namespace native_gpu;
static void require(bool condition) { if (!condition) throw std::runtime_error("wire regression"); }
static std::int32_t status(const std::vector<std::uint8_t>& reply) {
    require(reply.size() >= sizeof(Header)); Header h{};
    std::memcpy(&h, reply.data(), sizeof h); return h.status;
}
struct Fake : Renderer {
    int calls = 0; bool succeeds = true;
    Capabilities capabilities() const override { return {1, 3, 0x10de, 123}; }
    bool clearPresent(Color) override { ++calls; return succeeds; }
};
int main() {
    Fake fake; Session session(fake);
    auto draw = request(Operation::ClearPresent, Color{{10, 20, 30, 255}});
    require(status(session.dispatch(draw)) == -71 && fake.calls == 0);
    for (std::uint32_t type = 1; type <= 8; ++type) {
        Header h{type, 0, 0, 0}; std::vector<std::uint8_t> p(sizeof h);
        std::memcpy(p.data(), &h, sizeof h);
        require(status(session.dispatch(p)) == -95 && fake.calls == 0);
    }
    require(session.dispatch({1, 2, 3}).empty());
    require(status(session.dispatch(request(Operation::Capabilities, std::uint32_t{2}))) == -93);
    auto hello = request(Operation::Capabilities, ProtocolVersion);
    auto reply = session.dispatch(hello);
    require(status(reply) == 0 && reply.size() == 32);
    // Golden little-endian header: extension type 0x1000, zero handle/status/pad.
    require(hello[0] == 0 && hello[1] == 0x10 && hello[2] == 0 && hello[3] == 0);
    for (std::size_t length = 16; length < 20; ++length) {
        auto shortDraw = draw; shortDraw.resize(length);
        require(status(session.dispatch(shortDraw)) == -22 && fake.calls == 0);
    }
    auto bad = draw; bad.push_back(0); require(status(session.dispatch(bad)) == -22);
    bad = draw; bad[4] = 1; require(status(session.dispatch(bad)) == -22);
    bad = draw; bad[8] = 1; require(status(session.dispatch(bad)) == -22);
    bad = draw; bad[12] = 1; require(status(session.dispatch(bad)) == -22);
    bad = draw; bad.resize(MaxPacket + 1); require(status(session.dispatch(bad)) == -22);
    require(status(session.dispatch(draw)) == 0 && fake.calls == 1);
    fake.succeeds = false;
    require(status(session.dispatch(draw)) == -5 && fake.calls == 2);
    require(status(session.dispatch(request(Operation::Capabilities, std::uint32_t{2}))) == -93);
    require(status(session.dispatch(draw)) == -71 && fake.calls == 2);
    Fake other; Session isolated(other);
    require(status(isolated.dispatch(draw)) == -71 && other.calls == 0);
    std::cout << "PASS: framing, negotiation, bounds, session isolation, Linux ABI rejection\n";
}
