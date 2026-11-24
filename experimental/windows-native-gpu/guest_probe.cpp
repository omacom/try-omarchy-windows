// SPDX-License-Identifier: MIT
// Linux QEMU guest client. Tests driver memory and GPU copy, not rendering.
#include "driver_wire.h"
#include "adapter_query_client.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <iostream>
#include <fstream>
#include <stdexcept>
using namespace driver_bridge;
struct Port {
    int fd;
    explicit Port(const char* path) : fd(open(path, O_RDWR | O_CLOEXEC)) {
        if (fd < 0) throw std::runtime_error("Cannot open virtio serial port");
    }
    ~Port() { close(fd); }
    void transfer(void* data, std::size_t size, bool reading) {
        auto p = static_cast<std::uint8_t*>(data);
        while (size) {
            pollfd ready{fd, static_cast<short>(reading ? POLLIN : POLLOUT), 0};
            const int result = poll(&ready, 1, 10000);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0 || (ready.revents & (POLLERR | POLLNVAL))) throw std::runtime_error("Port timed out or failed");
            const auto n = reading ? read(fd, p, size) : write(fd, p, size);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("Port disconnected");
            p += n; size -= static_cast<std::size_t>(n);
        }
    }
    std::vector<std::uint8_t> exchange(std::vector<std::uint8_t> packet) {
        auto length = static_cast<std::uint32_t>(packet.size()); transfer(&length, 4, false);
        transfer(packet.data(), packet.size(), false); transfer(&length, 4, true);
        if (length < sizeof(Header) || length > native_gpu::MaxPacket) throw std::runtime_error("Invalid reply length");
        packet.resize(length); transfer(packet.data(), packet.size(), true); return packet;
    }
};
static Header unpack(const std::vector<std::uint8_t>& packet) {
    Header h{}; std::memcpy(&h, packet.data(), sizeof h);
    if (h.padding) throw std::runtime_error("Invalid response padding");
    return h;
}
struct Response { Header header; Reply reply; std::vector<std::uint8_t> data; };
static Response exchange(Port& port, std::vector<std::uint8_t> requestPacket, std::uint32_t dataSize = 0) {
    const auto op = unpack(requestPacket).type;
    const auto packet = port.exchange(requestPacket); const auto h = unpack(packet);
    if (h.type != op || h.status || packet.size() != sizeof(Header) + sizeof(Reply) + dataSize)
        throw std::runtime_error("Driver operation protocol failure");
    Reply r{}; std::memcpy(&r, packet.data() + sizeof(Header), sizeof r);
    if (r.ntstatus < 0 || r.reserved) throw std::runtime_error("Windows driver returned failure");
    if (op == static_cast<std::uint32_t>(Op::QueryDriverVersion)) std::cout << "WDDM enum=" << r.value << '\n';
    return {h, r, {packet.begin() + sizeof(Header) + sizeof(Reply), packet.end()}};
}
static std::uint32_t operation(Port& port, Op op, std::uint32_t handle = 0) {
    return exchange(port, request(op, handle)).header.handle;
}
static std::uint32_t syncContext(Port& port, std::uint32_t device) {
    const auto reply = exchange(port, request(Op::CreateContext, device, ContextDesc{0, 0, 8, 0, 0, 0}));
    if (!reply.header.handle) throw std::runtime_error("Missing synchronization context");
    std::cout << "BRIDGE_SYNC_CONTEXT created=true\n";
    return reply.header.handle;
}
static void contextFixtureTest(Port& port, std::uint32_t device) {
    // Optional local fixture generated from this host's installed Linux UMD.
    // Private data is neither checked in nor evidence of a live guest runtime.
    std::ifstream input("/driver-contexts.bin", std::ios::binary);
    if (!input) return;
    std::uint32_t count = 0;
    if (!input.read(reinterpret_cast<char*>(&count), sizeof count) || !count || count > 16)
        throw std::runtime_error("Invalid context fixture count");
    for (std::uint32_t i = 0; i < count; ++i) {
        ContextDesc desc{};
        if (!input.read(reinterpret_cast<char*>(&desc), sizeof desc) || !validContext(desc) || !desc.privateBytes)
            throw std::runtime_error("Invalid context fixture descriptor");
        auto packet = request(Op::CreateContext, device, desc);
        const auto start = packet.size(); packet.resize(start + desc.privateBytes);
        if (!input.read(reinterpret_cast<char*>(packet.data() + start), desc.privateBytes))
            throw std::runtime_error("Truncated context fixture");
        const auto response = exchange(port, packet, desc.privateBytes);
        if (!response.header.handle) throw std::runtime_error("Missing NVIDIA graphics context");
        // The final context deliberately survives until disconnect.
        if (i + 1 < count) operation(port, Op::DestroyContext, response.header.handle);
        std::cout << "BRIDGE_DRIVER_CONTEXT node=" << desc.node << " privateBytes=" << desc.privateBytes
                  << " created=true replyBytesVerified=true\n";
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing context fixture data");
}
static void adapterQueryTest(Port& port, std::uint32_t adapter, native_gpu::Capabilities caps) {
    auto call = [&](const std::vector<std::uint8_t>& packet) { return port.exchange(packet); };
    const auto version = adapterQuery(call, adapter, 13, std::vector<std::uint8_t>(4));
    const auto identity = adapterQuery(call, adapter, 31, std::vector<std::uint8_t>(28));
    std::uint32_t model = 0, vendor = 0, device = 0;
    std::memcpy(&model, version.data.data(), 4);
    std::memcpy(&vendor, identity.data.data() + 4, 4);
    std::memcpy(&device, identity.data.data() + 8, 4);
    if (version.ntstatus < 0 || identity.ntstatus < 0 || model < 2000 || vendor != caps.vendor || device != caps.device)
        throw std::runtime_error("Adapter query identity/version mismatch");
    std::cout << "BRIDGE_ADAPTER_QUERY identityVerified=true versionVerified=true\n";
}
static void queryFixtureTest(Port& port, std::uint32_t adapter) {
    std::ifstream input("/driver-queries.bin", std::ios::binary);
    if (!input) return;
    std::uint32_t count = 0;
    if (!input.read(reinterpret_cast<char*>(&count), sizeof count) || !count || count > 32)
        throw std::runtime_error("Invalid query fixture count");
    auto call = [&](const std::vector<std::uint8_t>& packet) { return port.exchange(packet); };
    for (std::uint32_t i = 0; i < count; ++i) {
        QueryDesc desc{};
        if (!input.read(reinterpret_cast<char*>(&desc), sizeof desc) || !validQuery(desc))
            throw std::runtime_error("Invalid query fixture descriptor");
        std::vector<std::uint8_t> data(desc.bytes);
        if (!input.read(reinterpret_cast<char*>(data.data()), desc.bytes)) throw std::runtime_error("Truncated query fixture");
        const auto result = adapterQuery(call, adapter, desc.type, data);
        if (result.ntstatus < 0 || result.data.size() != desc.bytes) throw std::runtime_error("Native adapter query failed");
        std::cout << "BRIDGE_DRIVER_QUERY type=" << desc.type << " bytes=" << desc.bytes << " buffersComplete=true\n";
    }
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("Trailing query fixture data");
}
static std::uint32_t allocationTest(Port& port, std::uint32_t device, std::uint32_t queue, unsigned seed) {
    constexpr std::uint32_t size = 65536;
    const auto created = exchange(port, request(Op::CreateAllocation, device, size));
    const auto allocation = created.header.handle;
    if (!allocation || created.reply.value != size) throw std::runtime_error("Allocation size mismatch");
    std::vector<std::uint8_t> pattern(size);
    for (std::uint32_t i = 0; i < size; ++i) pattern[i] = static_cast<std::uint8_t>((i * 17u + seed * 29u) ^ (i >> 8));
    for (std::uint32_t offset = 0; offset < size; offset += MaxChunk) {
        const auto count = std::min(MaxChunk, size - offset);
        auto packet = request(Op::WriteAllocation, allocation, Range{offset, count});
        packet.insert(packet.end(), pattern.begin() + offset, pattern.begin() + offset + count);
        if (exchange(port, packet).reply.value != count) throw std::runtime_error("Short allocation write");
        const auto read = exchange(port, request(Op::ReadAllocation, allocation, Range{offset, count}), count);
        if (read.reply.value != count || !std::equal(read.data.begin(), read.data.end(), pattern.begin() + offset))
            throw std::runtime_error("Host allocation CPU roundtrip mismatch");
    }
    const auto outOfBounds = unpack(port.exchange(request(Op::ReadAllocation, allocation, Range{size - 1, 2})));
    if (outOfBounds.status != -22) throw std::runtime_error("Allocation bounds check failed");
    exchange(port, request(Op::MakeResident, allocation, queue));
    const auto mapped = exchange(port, request(Op::MapAllocation, allocation, queue));
    if (!mapped.reply.value || exchange(port, request(Op::MapAllocation, allocation, queue)).reply.value != mapped.reply.value)
        throw std::runtime_error("GPU virtual address mapping failed");
    const auto residency = exchange(port, request(Op::QueryResidency, allocation)).reply.value;
    if (residency != 1 && residency != 2) throw std::runtime_error("Allocation is not resident");
    std::cout << "BRIDGE_ALLOCATION bytes=" << size << " cpuRoundtrip=true gpuVaMapped=true residency=" << residency << '\n';
    return allocation;
}
static void sharedAllocationTest(Port& port, std::uint32_t device, std::uint32_t queue, bool gpuCopy) {
    constexpr std::uint32_t size = 65536, hugeSize = 2 * 1024 * 1024;
    auto memory = mmap(nullptr, hugeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (memory == MAP_FAILED) throw std::runtime_error("Cannot obtain test huge page; boot with hugepagesz=2M hugepages=1");
    // This root-only fixture checks every PFN rather than assuming mmap's pages
    // are physically contiguous. A production guest driver must pin/register
    // pages and maintain their lifetime instead of reading /proc pagemap.
    auto bytes = static_cast<volatile std::uint8_t*>(memory);
    for (std::uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>((i * 37u) ^ (i >> 8));
    const auto pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap < 0) throw std::runtime_error("Cannot read test page mapping");
    std::uint64_t firstPfn = 0;
    for (std::uint32_t page = 0; page < 2 * size / 4096; ++page) {
        const auto index = (reinterpret_cast<std::uintptr_t>(memory) / 4096 + page) * 8;
        std::uint64_t entry = 0;
        if (pread(pagemap, &entry, sizeof entry, static_cast<off_t>(index)) != sizeof entry || !(entry & (1ull << 63)))
            throw std::runtime_error("Test page is not present");
        const auto pfn = entry & ((1ull << 55) - 1);
        if (!page) firstPfn = pfn;
        if (!firstPfn || pfn != firstPfn + page) throw std::runtime_error("Test physical pages are not contiguous");
    }
    close(pagemap);
    const auto created = exchange(port, request(Op::CreateSharedAllocation, device, GuestRange{firstPfn * 4096, size, 0}));
    const auto allocation = created.header.handle;
    if (!allocation || created.reply.value != size) throw std::runtime_error("Shared allocation creation failed");
    for (std::uint32_t offset = 0; offset < size; offset += MaxChunk) {
        const auto count = std::min(MaxChunk, size - offset);
        const auto response = exchange(port, request(Op::ReadAllocation, allocation, Range{offset, count}), count);
        for (std::uint32_t i = 0; i < count; ++i)
            if (response.data[i] != bytes[offset + i]) throw std::runtime_error("Windows did not see guest RAM writes");
    }
    auto write = request(Op::WriteAllocation, allocation, Range{0, 4}); write.insert(write.end(), {89, 34, 201, 17});
    exchange(port, write); std::atomic_thread_fence(std::memory_order_seq_cst);
    if (bytes[0] != 89 || bytes[1] != 34 || bytes[2] != 201 || bytes[3] != 17)
        throw std::runtime_error("Guest did not see Windows RAM writes");
    exchange(port, request(Op::MakeResident, allocation, queue));
    if (!exchange(port, request(Op::MapAllocation, allocation, queue)).reply.value)
        throw std::runtime_error("Shared guest allocation GPU mapping failed");
    const auto residency = exchange(port, request(Op::QueryResidency, allocation)).reply.value;
    if (residency != 1 && residency != 2) throw std::runtime_error("Shared guest allocation is not resident");
    std::cout << "BRIDGE_SHARED_ALLOCATION bytes=" << size << " guestToHost=true hostToGuest=true gpuVaMapped=true residency=" << residency << '\n';
    if (gpuCopy) {
        const auto second = exchange(port, request(Op::CreateSharedAllocation, device, GuestRange{firstPfn * 4096 + size, size, 0}));
        if (!second.header.handle || second.reply.value != size) throw std::runtime_error("GPU destination creation failed");
        std::uint64_t copied = 0;
        const CopyRange ranges[] = {{allocation, 0, 0, size}, {allocation, 7, 31, 73},
            {allocation, 0, 31, size - 31}, {allocation, 0, 0, size},
            {allocation, 201, 501, 257}, {allocation, 0, 0, size}};
        unsigned cycle = 0;
        for (const auto range : ranges) {
            for (std::uint32_t i = 0; i < size; ++i) {
                bytes[i] = static_cast<std::uint8_t>((i * 31u + cycle * 13u) ^ (i >> 8)); bytes[size + i] = 85;
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const auto copiedResult = exchange(port, request(Op::CopySharedAllocation, second.header.handle, range));
            if (copiedResult.reply.value != range.size) throw std::runtime_error("Short GPU copy");
            std::atomic_thread_fence(std::memory_order_seq_cst);
            for (std::uint32_t i = 0; i < size; ++i) {
                const auto expected = i >= range.destinationOffset && i - range.destinationOffset < range.size
                    ? bytes[range.sourceOffset + i - range.destinationOffset] : static_cast<std::uint8_t>(85);
                if (bytes[size + i] != expected) throw std::runtime_error("Guest GPU copy bytes or destination guard mismatch");
                if (bytes[i] != static_cast<std::uint8_t>((i * 31u + cycle * 13u) ^ (i >> 8)))
                    throw std::runtime_error("GPU copy modified source");
            }
            copied += range.size; ++cycle;
        }
        std::cout << "BRIDGE_GPU_COPY cycles=" << cycle << " bytes=" << copied
                  << " guestToGpuToGuest=true guards=true fenceCompleted=true\n";
    }
    // Retain this allocation and mapping until disconnect to verify teardown
    // while the Windows worker still owns a view of QEMU's RAM section.
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Usage: guest-probe /dev/vport0p1");
        Port port(argv[1]); const auto helloPacket = port.exchange(hello());
        const auto h = unpack(helloPacket);
        if (h.status || h.type != static_cast<std::uint32_t>(Op::Hello) || helloPacket.size() != 32)
            throw std::runtime_error("Handshake failed");
        native_gpu::Capabilities caps{}; std::memcpy(&caps, helloPacket.data() + sizeof(Header), sizeof caps);
        if (caps.version != Version || caps.vendor != 0x10de || (caps.flags & 7) != 7) throw std::runtime_error("Unexpected backend capabilities");
        std::cout << "BRIDGE_VENDOR=" << caps.vendor << " BRIDGE_DEVICE=" << caps.device << '\n';
        for (unsigned cycle = 0; cycle < 5; ++cycle) {
            const auto adapter = operation(port, Op::OpenAdapter);
            operation(port, Op::QueryDriverVersion, adapter);
            if (caps.flags & QueryCapability) adapterQueryTest(port, adapter, caps);
            else if (unpack(port.exchange(request(Op::BeginAdapterQuery, adapter, QueryDesc{13, 4, 0, 0}))).status != -95)
                throw std::runtime_error("Unadvertised driver queries accepted");
            const auto device = operation(port, Op::CreateDevice, adapter);
            if (caps.flags & ContextCapability) {
                const auto context = syncContext(port, device);
                if (unpack(port.exchange(request(Op::DestroyDevice, device))).status != -16)
                    throw std::runtime_error("Device destroyed with live context");
                operation(port, Op::DestroyContext, context);
                if (unpack(port.exchange(request(Op::DestroyContext, context))).status != -9)
                    throw std::runtime_error("Stale context accepted");
            }
            const auto queue = operation(port, Op::CreatePagingQueue, device);
            operation(port, Op::ReadPagingFence, queue);
            const auto allocation = allocationTest(port, device, queue, cycle);
            const auto busy = unpack(port.exchange(request(Op::DestroyDevice, device)));
            if (busy.status != -16) throw std::runtime_error("Live child ownership check failed");
            operation(port, Op::DestroyAllocation, allocation);
            operation(port, Op::DestroyPagingQueue, queue); operation(port, Op::DestroyDevice, device);
            operation(port, Op::CloseAdapter, adapter);
            const auto stale = unpack(port.exchange(request(Op::QueryDriverVersion, adapter)));
            if (stale.status != -9) throw std::runtime_error("Stale handle check failed");
        }
        // Leave a live hierarchy to verify cleanup when the VM disconnects.
        const auto abandonedAdapter = operation(port, Op::OpenAdapter);
        if (caps.flags & QueryCapability) queryFixtureTest(port, abandonedAdapter);
        const auto abandonedDevice = operation(port, Op::CreateDevice, abandonedAdapter);
        if (caps.flags & ContextCapability) {
            syncContext(port, abandonedDevice);
            contextFixtureTest(port, abandonedDevice);
        }
        const auto abandonedQueue = operation(port, Op::CreatePagingQueue, abandonedDevice);
        allocationTest(port, abandonedDevice, abandonedQueue, 5);
        if (caps.flags & 8) sharedAllocationTest(port, abandonedDevice, abandonedQueue, (caps.flags & 16) != 0);
        if (caps.flags & QueryCapability)
            exchange(port, request(Op::BeginAdapterQuery, abandonedAdapter, QueryDesc{0, MaxQueryBytes, 0, 0}));
        std::cout << "PASS: QEMU guest WDDM allocation bridge, 5 lifecycle cycles; guest rendering=false\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
