// SPDX-License-Identifier: MIT
// Shared by the Linux acceptance client and the future guest KMT interface.
// The caller serializes the entire transaction on its connection.
#pragma once
#include "driver_wire.h"
#include <algorithm>
#include <stdexcept>

namespace driver_bridge {
class QueryProtocolError : public std::runtime_error {
public:
    std::int32_t code;
    explicit QueryProtocolError(std::int32_t error)
        : std::runtime_error("Adapter query protocol failure"), code(error) {}
};
struct QueryResponse { std::int32_t ntstatus; std::vector<std::uint8_t> data; };

template<class Exchange>
QueryResponse adapterQuery(Exchange&& exchange, std::uint32_t adapter,
                           std::uint32_t type, const std::vector<std::uint8_t>& input) {
    if (input.size() > MaxQueryBytes) throw QueryProtocolError(-22);
    const QueryDesc desc{type, static_cast<std::uint32_t>(input.size()), 0, 0};
    if (!adapter || !validQuery(desc)) throw QueryProtocolError(-22);
    auto call = [&](const std::vector<std::uint8_t>& packet, std::uint32_t dataBytes = 0) {
        const auto output = exchange(packet);
        if (output.size() < sizeof(Header)) throw QueryProtocolError(-71);
        Header expected{}, actual{};
        std::memcpy(&expected, packet.data(), sizeof expected);
        std::memcpy(&actual, output.data(), sizeof actual);
        if (actual.type != expected.type || actual.padding) throw QueryProtocolError(-71);
        if (actual.status) throw QueryProtocolError(actual.status);
        if (actual.handle != adapter || output.size() != sizeof(Header) + sizeof(Reply) + dataBytes)
            throw QueryProtocolError(-71);
        Reply result{}; std::memcpy(&result, output.data() + sizeof(Header), sizeof result);
        if (result.reserved) throw QueryProtocolError(-71);
        return std::make_pair(result, std::vector<std::uint8_t>(output.begin() + sizeof(Header) + sizeof(Reply), output.end()));
    };
    bool begun = false;
    try {
        const auto begin = call(request(Op::BeginAdapterQuery, adapter, desc)); begun = true;
        if (begin.first.ntstatus || begin.first.value != desc.bytes) throw QueryProtocolError(-71);
        for (std::uint32_t offset = 0; offset < desc.bytes;) {
            const auto count = std::min(MaxChunk, desc.bytes - offset);
            auto packet = request(Op::WriteAdapterQuery, adapter, Range{offset, count});
            packet.insert(packet.end(), input.begin() + offset, input.begin() + offset + count);
            const auto written = call(packet);
            if (written.first.ntstatus || written.first.value != offset + count) throw QueryProtocolError(-71);
            offset += count;
        }
        const auto ran = call(request(Op::RunAdapterQuery, adapter));
        if (ran.first.value != desc.bytes) throw QueryProtocolError(-71);
        QueryResponse result{ran.first.ntstatus, std::vector<std::uint8_t>(desc.bytes)};
        for (std::uint32_t offset = 0; offset < desc.bytes;) {
            const auto count = std::min(MaxChunk, desc.bytes - offset);
            const auto read = call(request(Op::ReadAdapterQuery, adapter, Range{offset, count}), count);
            if (read.first.ntstatus != result.ntstatus || read.first.value != desc.bytes) throw QueryProtocolError(-71);
            std::copy(read.second.begin(), read.second.end(), result.data.begin() + offset);
            offset += count;
        }
        const auto ended = call(request(Op::EndAdapterQuery, adapter)); begun = false;
        if (ended.first.ntstatus) throw QueryProtocolError(-71);
        return result;
    } catch (...) {
        // Best effort cancellation; transport failure still requires the
        // caller to disconnect so the worker can release the entire session.
        if (begun) { try { call(request(Op::EndAdapterQuery, adapter)); } catch (...) {} }
        throw;
    }
}
} // namespace driver_bridge
