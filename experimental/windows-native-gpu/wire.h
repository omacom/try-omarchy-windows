// SPDX-License-Identifier: MIT
// Independently implemented framing inspired by virtio-nvgpu, pinned in sources.json.
// The Linux NVIDIA operations deliberately have no Windows implementation.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace native_gpu {
constexpr std::size_t MaxPacket = 4096;
constexpr std::uint32_t ProtocolVersion = 1;
enum class Operation : std::uint32_t { Capabilities = 0x1000, ClearPresent = 0x1001 };
struct Header {
    std::uint32_t type;
    std::uint32_t handle;
    std::int32_t status;
    std::uint32_t padding;
};
static_assert(sizeof(Header) == 16, "virtio-nvgpu framing size");
static_assert(offsetof(Header, status) == 8, "signed status offset");
// Capabilities is also the version handshake. Native operations never advertise
// Linux RM compatibility, shared guest mappings, or a working virtio device.
struct Capabilities {
    std::uint32_t version;
    std::uint32_t flags; // bit 0: host presentation; bit 1: host shared-resource readback
    std::uint32_t vendor;
    std::uint32_t device;
};
struct Color { std::uint8_t rgba[4]; };
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual Capabilities capabilities() const = 0;
    virtual bool clearPresent(Color color) = 0;
};
class Session {
    Renderer& renderer;
    bool negotiated = false;
    static std::vector<std::uint8_t> response(Header header, std::int32_t status) {
        header.handle = 0; header.status = status; header.padding = 0;
        std::vector<std::uint8_t> out(sizeof header);
        std::memcpy(out.data(), &header, sizeof header);
        return out;
    }
public:
    explicit Session(Renderer& r) : renderer(r) {}
    // This consumes a descriptor-sized byte span. A future QEMU adapter must
    // validate guest descriptors before copying them here and bound the reply.
    std::vector<std::uint8_t> dispatch(const std::vector<std::uint8_t>& packet) {
        if (packet.size() < sizeof(Header)) return {}; // cannot read a header
        Header header{};
        std::memcpy(&header, packet.data(), sizeof header);
        if (packet.size() > MaxPacket || header.status || header.padding || header.handle)
            return response(header, -22); // Linux EINVAL on the wire
        if (header.type == static_cast<std::uint32_t>(Operation::Capabilities)) {
            if (packet.size() != sizeof(Header) + sizeof(std::uint32_t))
                return response(header, -22);
            std::uint32_t version{};
            std::memcpy(&version, packet.data() + sizeof(Header), sizeof version);
            if (version != ProtocolVersion) { negotiated = false; return response(header, -93); }
            auto caps = renderer.capabilities();
            if (caps.version != ProtocolVersion) return response(header, -93);
            negotiated = true;
            auto out = response(header, 0);
            out.resize(sizeof(Header) + sizeof(Capabilities));
            std::memcpy(out.data() + sizeof(Header), &caps, sizeof caps);
            return out;
        }
        if (header.type == static_cast<std::uint32_t>(Operation::ClearPresent)) {
            if (packet.size() != sizeof(Header) + sizeof(Color)) return response(header, -22);
            if (!negotiated) return response(header, -71); // EPROTO
            Color color{};
            std::memcpy(&color, packet.data() + sizeof(Header), sizeof color);
            return response(header, renderer.clearPresent(color) ? 0 : -5);
        }
        // Includes original OPEN/CLOSE/IOCTL/MMAP/MUNMAP/proc/sys/event IDs 1..8.
        // Do not pass untrusted Linux ioctl numbers to DeviceIoControl/D3DKMTEscape.
        return response(header, -95); // EOPNOTSUPP
    }
};
template<class T> std::vector<std::uint8_t> request(Operation operation, const T& body) {
    Header header{static_cast<std::uint32_t>(operation), 0, 0, 0};
    std::vector<std::uint8_t> out(sizeof header + sizeof body);
    std::memcpy(out.data(), &header, sizeof header);
    std::memcpy(out.data() + sizeof header, &body, sizeof body);
    return out;
}
} // namespace native_gpu
