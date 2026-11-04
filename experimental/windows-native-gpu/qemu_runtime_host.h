// SPDX-License-Identifier: MIT
// The native driver owner also owns QEMU, including every failure teardown.
#pragma once
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include "qmp_protocol.h"
#include "qmp_socket_wait.h"
#include "qmp_deadline_budget.h"
#include "qmp_relay_transaction.h"
#include "driver_wire.h"
#include "cpu_aperture.h"
#include "qemu_fence_test.h"
#include "qemu_graphics_arguments.h"
#include "owned_qemu_process.h"
#include "launcher_profile.h"
#include "readonly_rootfs_args.h"
#include "bounded_input_channel.h"
#include "tcp_peer_owner.h"
#include "binary_capture.h"
#include "sdl-relay-output/control.h"
#include <array>
#include <memory>
#include <optional>
#include <mutex>

namespace driver_qemu {
struct Socket {
    SOCKET value = INVALID_SOCKET;
    Socket() = default;
    Socket(const Socket&) = delete;
    ~Socket() { if (value != INVALID_SOCKET) closesocket(value); }
};
inline unsigned short reservePort(Socket& socket) {
    socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket.value == INVALID_SOCKET) throw std::runtime_error("QMP socket creation failed");
    const BOOL exclusive = TRUE;
    if (setsockopt(socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof exclusive))
        throw std::runtime_error("QMP port reservation failed");
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof address)) throw std::runtime_error("QMP bind failed");
    int size = sizeof address;
    if (getsockname(socket.value, reinterpret_cast<sockaddr*>(&address), &size)) throw std::runtime_error("QMP endpoint failed");
    return ntohs(address.sin_port);
}
class Qmp {
    Socket socket;
    std::string buffered;
    std::uint64_t next = 0;
    const driver_qmp::ChannelPurpose channelPurpose;
    const std::uint32_t commandBudgetMs;
    void ready(bool reading, ULONGLONG deadline) {
        waitQmpSocket(socket.value, reading, deadline);
    }
    driver_qmp::Json read(ULONGLONG deadline) {
        for (;;) {
            const auto end = buffered.find('\n');
            if (end != std::string::npos) {
                auto line = buffered.substr(0, end); buffered.erase(0, end + 1);
                return driver_qmp::parse(line);
            }
            if (buffered.size() >= driver_qmp::MaxMessage) throw std::runtime_error("QMP line bound exceeded");
            ready(true, deadline);
            char bytes[1024]; const auto count = recv(socket.value, bytes, sizeof bytes, 0);
            if (count <= 0) throw std::runtime_error("QMP disconnected");
            buffered.append(bytes, static_cast<std::size_t>(count));
        }
    }
public:
    Qmp(unsigned short port, const OwnedProcess& owner, const std::string& name,
        driver_qmp::ChannelPurpose purpose = driver_qmp::ChannelPurpose::Control)
        : channelPurpose(purpose), commandBudgetMs(driver_qmp::commandTimeoutMs(purpose)) {
        const auto deadline = GetTickCount64() + 10000;
        for (;;) {
            socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket.value == INVALID_SOCKET) throw std::runtime_error("QMP client socket failed");
            sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
            if (!connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof address)) break;
            closesocket(socket.value); socket.value = INVALID_SOCKET;
            if (owner.exited() || GetTickCount64() >= deadline) throw std::runtime_error("Owned QEMU QMP unavailable");
            Sleep(20);
        }
        if(!tcpPeerOwned(socket.value,GetProcessId(owner.handle.value)))
            throw std::runtime_error("QMP socket peer is not the owned QEMU process");
        const DWORD milliseconds = 5000;
        if (setsockopt(socket.value, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds) ||
            setsockopt(socket.value, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof milliseconds))
            throw std::runtime_error("QMP timeout configuration failed");
        const auto greeting = read(deadline);
        if (!greeting.contains("QMP") || !greeting.at("QMP").is_object()) throw std::runtime_error("QMP greeting invalid");
        if (!call("qmp_capabilities").is_object()) throw std::runtime_error("QMP capabilities invalid");
        const auto identity = call("query-name");
        if (!identity.is_object() || !identity.contains("name") || identity.at("name") != name)
            throw std::runtime_error("QMP owned machine name mismatch");
    }
private:
    driver_qmp::Json callUntil(const std::string& operation, driver_qmp::Json arguments, ULONGLONG deadline) {
        if (next == UINT64_MAX) throw std::runtime_error("QMP ID exhausted");
        const auto id = "fence-" + std::to_string(++next);
        driver_qmp::Json command{{"execute", operation}, {"id", id}};
        if (!arguments.is_null()) command["arguments"] = std::move(arguments);
        const auto bytes = command.dump() + '\n';
        if (bytes.size() > driver_qmp::MaxMessage) throw std::runtime_error("QMP request bound exceeded");
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            ready(false, deadline);
            const auto count = send(socket.value, bytes.data() + sent, static_cast<int>(bytes.size() - sent), 0);
            if (count <= 0) throw std::runtime_error("QMP write failed");
            sent += static_cast<std::size_t>(count);
        }
        for (unsigned eventCount = 0; eventCount < 32; ++eventCount) {
            const auto reply = read(deadline);
            if (driver_qmp::event(reply)) continue;
            return driver_qmp::result(reply, id);
        }
        throw std::runtime_error("QMP asynchronous event bound exceeded");
    }
public:
    driver_qmp::Json call(const std::string& operation, driver_qmp::Json arguments = nullptr) {
        return callUntil(operation,std::move(arguments),GetTickCount64()+commandBudgetMs);
    }
    driver_qmp::Json relayCommandState(const std::string& instruction) {
        if(channelPurpose!=driver_qmp::ChannelPurpose::SdlGraphics)
            throw std::runtime_error("SDL transaction requires its dedicated graphics channel");
        const auto deadline=GetTickCount64()+commandBudgetMs;
        return driver_qmp::relayTransaction([this](const std::string& operation,driver_qmp::Json arguments,std::uint64_t until) {
            return callUntil(operation,std::move(arguments),until);
        },instruction,deadline);
    }
    void mapping(const std::string& command) {
        const auto reply = call("qom-set", {{"path", "/machine/peripheral/wddm-fences"}, {"property", "fence-mapping"}, {"value", command}});
        if (!reply.is_object() || !reply.empty()) throw std::runtime_error("QMP mapping acknowledgement invalid");
    }
    void allocationMapping(const std::string& command) {
        const auto reply = call("qom-set", {{"path", "/machine/peripheral/wddm-allocations"}, {"property", "allocation-mapping"}, {"value", command}});
        if (!reply.is_object() || !reply.empty()) throw std::runtime_error("QMP allocation acknowledgement invalid");
    }
};
struct FenceLease { std::uint32_t slot; std::uint64_t generation, offset; };
using AllocationLease = driver_cpu::Lease;
class Runtime {
    // Keep a read handle denying write sharing until QEMU and its job close.
    Handle readonlyBase;
    OwnedProcess process;
    std::unique_ptr<Qmp> qmp;
    std::shared_ptr<driver_input::Channel> windowInput;
    std::shared_ptr<driver_sdl::Control> sdlGraphics;
    std::array<std::optional<FenceLease>, driver_bridge::DesktopFenceSlots> leases;
    std::uint32_t fenceSlots = driver_bridge::DefaultFenceSlots;
    std::size_t allocationSlots = driver_bridge::DefaultVendorCpuSlots;
    driver_cpu::Aperture allocationAperture;
    bool allocationsEnabled = false;
    std::uint64_t generation = 0;
    unsigned mapped = 0, mappedTotal = 0, unmappedTotal = 0;
    unsigned fencesReleasedAfterVmExit = 0, allocationsReleasedAfterVmExit = 0, allocationSlotsReleasedAfterVmExit = 0;
    bool stopped = false, failed = false;
    bool inputDiagnostic = false;
    std::filesystem::path logfile;
    static std::string nvidiaLuid() {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("SDL adapter factory failed");
        for(UINT index=0;index<32;++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;const auto hr=factory->EnumAdapters1(index,&adapter);
            if(hr==DXGI_ERROR_NOT_FOUND) break;
            DXGI_ADAPTER_DESC1 desc{};
            if(FAILED(hr) || FAILED(adapter->GetDesc1(&desc))) throw std::runtime_error("SDL adapter enumeration failed");
            if(desc.VendorId==0x10de && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) return driver_sdl::luidText(&desc.AdapterLuid);
        }
        throw std::runtime_error("SDL relay requires a hardware NVIDIA adapter");
    }
public:
    Runtime(unsigned short driverPort, const qemu_fence::Paths& paths, bool enableCpu = false, bool cpuStoreTest = false, bool cpuEofTest = false, bool hwQueueEofTest = false, std::size_t cpuSlots = driver_bridge::DefaultVendorCpuSlots, bool syncEofTest = false, bool reservationEofTest = false, bool gpuStateEofTest = false, bool cpuSpanEofTest = false, bool syncNoMaxEofTest = false, bool hwQueueNoBroadcastEofTest = false, bool hwQueueNoBroadcastWaitEofTest = false, std::uint32_t configuredFenceSlots = 0, const char* readonlyRoot = nullptr, bool desktopFences = false, bool enableWindowInput = false, bool inputSelfTest = false, bool enableSdl = false, bool virtioTrace = false, bool userNetwork = false, const driver_launcher::Profile* launcherProfile = nullptr, std::function<void()> requireLauncherLive = {}, std::function<void(const ProcessBinding&)> onBound = {}, driver_qemu_args::BootDisplay bootDisplay = driver_qemu_args::BootDisplay::None)
        : fenceSlots(configuredFenceSlots ? configuredFenceSlots : cpuSlots >= driver_bridge::ExpandedVendorCpuSlots ? driver_bridge::ExpandedFenceSlots : driver_bridge::DefaultFenceSlots),
          allocationSlots(cpuSlots), allocationAperture(cpuSlots,desktopFences), allocationsEnabled(enableCpu), logfile(launcherProfile ? std::filesystem::path{} : std::filesystem::absolute(paths.log)) {
        if (bootDisplay != driver_qemu_args::BootDisplay::None && bootDisplay != driver_qemu_args::BootDisplay::StandardVga)
            throw std::runtime_error("Invalid launcher boot-console mode");
        if (bootDisplay != driver_qemu_args::BootDisplay::None && (!launcherProfile || !requireLauncherLive || !onBound))
            throw std::runtime_error("Boot console requires the owned original launcher profile and lifetime binding");
        if (launcherProfile && (!desktopFences || !enableSdl || readonlyRoot || enableWindowInput || inputSelfTest || virtioTrace || userNetwork))
            throw std::runtime_error("Launcher runtime requires its original devices and official SDL input");
        if (!driver_bridge::validFenceSlots(fenceSlots,desktopFences) || !driver_bridge::validVendorCpuSlots(cpuSlots,gpu_diagnostic::HighResolution,desktopFences) || (cpuSlots==driver_bridge::SparseVendorCpuSlots && fenceSlots!=(desktopFences ? 512u : 256u)) || (cpuSlots==driver_bridge::DesktopVendorCpuSlots && (!desktopFences || fenceSlots!=512)) || (!enableCpu && cpuSlots != driver_bridge::DefaultVendorCpuSlots))
            throw std::runtime_error("Invalid configured allocation aperture capacity");
        std::vector<std::wstring> rootArguments;
        inputDiagnostic=inputSelfTest;if(inputDiagnostic && !enableWindowInput) throw std::runtime_error("Input self-test requires window input");
        if(enableWindowInput && (!readonlyRoot || !desktopFences)) throw std::runtime_error("Window input requires the owned desktop root runtime");
        if(enableSdl && ((!readonlyRoot && !launcherProfile) || !desktopFences || (enableWindowInput && !inputSelfTest)))
            throw std::runtime_error("SDL graphics requires the owned desktop root; optional input is isolated QMP diagnostic only");
        if(virtioTrace && (!enableSdl || !readonlyRoot || !desktopFences))
            throw std::runtime_error("Virtio graphics trace requires the owned SDL root runtime");
        if(userNetwork && (!enableSdl || !readonlyRoot || !desktopFences))
            throw std::runtime_error("User networking requires the owned SDL desktop root runtime");
        if (readonlyRoot) {
            const auto root=std::filesystem::path(readonlyRoot);
            if (!root.is_absolute() || !std::filesystem::is_regular_file(root))
                throw std::runtime_error("Read-only root image must name an existing absolute regular file");
            rootArguments=readonlyRootArguments(root);
            readonlyBase.value=CreateFileW(root.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if (readonlyBase.value==INVALID_HANDLE_VALUE)
                throw std::runtime_error("Cannot hold read-only root image without write sharing");
        }
        const unsigned memoryMiB=launcherProfile ? launcherProfile->memoryMiB : readonlyRoot ? 3072u : gpu_diagnostic::GuestMemoryMiB;
        const auto executable = launcherProfile ? launcherProfile->executable : std::filesystem::absolute(paths.qemu).wstring();
        Socket reservation; const auto qmpPort = reservePort(reservation);
        Socket inputReservation;const auto inputPort=enableWindowInput?reservePort(inputReservation):static_cast<unsigned short>(0);
        Socket graphicsReservation;const auto graphicsPort=enableSdl?reservePort(graphicsReservation):static_cast<unsigned short>(0);
        unsigned char random[16]{};
        if (BCryptGenRandom(nullptr, random, sizeof random, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw std::runtime_error("Cannot create owned QEMU name");
        std::string name = "wddm-"; const char hex[] = "0123456789abcdef";
        for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
        const auto qmpName = launcherProfile ? launcherProfile->machineName : name;
        if(enableSdl) {
            sdlGraphics=std::make_shared<driver_sdl::Control>();sdlGraphics->session=name.substr(5);
            sdlGraphics->adapterLuid=nvidiaLuid();
            sdlGraphics->bootConsole=bootDisplay==driver_qemu_args::BootDisplay::StandardVga;
        }
        Handle source, log, input;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &source.value,
                PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, TRUE, 0))
            throw std::runtime_error("Cannot create inherited source process handle");
        SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
        if (launcherProfile) {
            // The Go launcher already opened its per-attempt QEMU log. The
            // bridge's stdout is a protocol pipe; QEMU keeps the original log
            // through its separately inherited stderr-file entry.
            const auto originalLog = GetStdHandle(STD_ERROR_HANDLE);
            if (!originalLog || originalLog == INVALID_HANDLE_VALUE || GetFileType(originalLog) != FILE_TYPE_DISK ||
                !DuplicateHandle(GetCurrentProcess(),originalLog,GetCurrentProcess(),&log.value,0,TRUE,DUPLICATE_SAME_ACCESS))
                throw std::runtime_error("Launcher runtime requires its inherited QEMU log file");
        } else {
            log.value = CreateFileW(logfile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (log.value == INVALID_HANDLE_VALUE || input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create fresh QEMU log and input");
        HANDLE inherited[]{source.value, log.value, input.value};
        SIZE_T size = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        auto attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
        bool initialized = attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &size);
        bool ready = initialized && UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr);
        const driver_qemu_args::Transport transport{reinterpret_cast<std::uintptr_t>(source.value), driverPort,
            fenceSlots, static_cast<unsigned>(allocationSlots)};
        std::vector<std::wstring> args;
        if (launcherProfile) {
            const auto session=std::wstring(sdlGraphics->session.begin(),sdlGraphics->session.end());
            const auto luid=std::wstring(sdlGraphics->adapterLuid.begin(),sdlGraphics->adapterLuid.end());
            args=launcherProfile->compose(transport,session,luid,qmpPort,graphicsPort,bootDisplay);
        } else {
        args={executable, L"-machine", L"q35,accel=whpx", L"-cpu", L"host", L"-m", std::to_wstring(memoryMiB), L"-smp", readonlyRoot ? L"2" : L"1",
            L"-display", enableSdl?L"sdl,gl=on,show-cursor=off,window-close=off":L"none", L"-monitor", L"none", L"-serial", L"stdio", L"-nodefaults", L"-no-reboot",
            L"-L", std::filesystem::absolute(paths.firmware).wstring(), L"-kernel", std::filesystem::absolute(paths.kernel).wstring(),
            L"-initrd", std::filesystem::absolute(paths.initramfs).wstring(), L"-append",
            readonlyRoot ? L"console=ttyS0 rdinit=/init root=overlay systemd.unit=multi-user.target systemd.show_status=no loglevel=4 panic=1" :
            hwQueueNoBroadcastWaitEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_hwqueue_no_broadcast_wait_eof_test=1" :
            hwQueueNoBroadcastEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_hwqueue_no_broadcast_eof_test=1" :
            cpuStoreTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_cpu_store_test=1" :
            syncEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_sync_eof_test=1" :
            syncNoMaxEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_sync_no_max_eof_test=1" :
            reservationEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_reservation_eof_test=1" :
            gpuStateEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_gpu_state_eof_test=1" :
            cpuSpanEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_cpu_span_eof_test=1" :
            hwQueueEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_hwqueue_eof_test=1" :
            cpuEofTest ? L"console=ttyS0 rdinit=/init panic=1 wddm_cpu_eof_test=1" : L"console=ttyS0 rdinit=/init panic=1",
            L"-name", std::wstring(name.begin(), name.end()), L"-qmp", L"tcp:127.0.0.1:" + std::to_wstring(qmpPort) + L",server=on,wait=off",
            L"-device", driver_qemu_args::fenceDevice(transport)};
        driver_qemu_args::append(args, driver_qemu_args::guestTransport(transport));
        args.insert(args.end(),rootArguments.begin(),rootArguments.end());
        if(userNetwork) {
            args.insert(args.end(),{L"-netdev",L"user,id=browser-net",
                L"-device",L"virtio-net-pci,id=browser-nic,netdev=browser-net,romfile="});
        }
        if(virtioTrace) {
            const auto trace=driver_capture::directory()/"graphics-virtio.trace";
            const auto file=CreateFileW(trace.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve exclusive graphics trace file");
            if(!CloseHandle(file)) throw std::runtime_error("Cannot close reserved graphics trace file");
            std::wstring escaped;
            for(const auto c:trace.wstring()) { escaped+=c;if(c==L',') escaped+=c; }
            args.insert(args.end(),{L"-chardev",L"file,id=graphics-trace,path="+escaped+L",append=on",
                L"-device",L"virtserialport,bus=bridge-serial.0,nr=2,chardev=graphics-trace,name=org.7wdev.graphics.trace"});
            std::cerr<<"{\"nativeGraphicsTraceConfigured\":true,\"carrier\":\"virtio-serial\",\"file\":\"graphics-virtio.trace\""
                <<",\"port\":2,\"liveHostCapture\":true,\"inputImplementationChanged\":false,\"gpuProtocolChanged\":false}\n";
        }
        if(enableSdl) {
            const auto session=std::wstring(sdlGraphics->session.begin(),sdlGraphics->session.end());
            const auto luid=std::wstring(sdlGraphics->adapterLuid.begin(),sdlGraphics->adapterLuid.end());
            args.insert(args.end(),{L"-vga",L"none",L"-qmp",L"tcp:127.0.0.1:"+std::to_wstring(graphicsPort)+L",server=on,wait=off",
                L"-device",driver_qemu_args::relayDevice(session,luid,driver_capture::directory().wstring())});
        }
        if(enableWindowInput) args.insert(args.end(),{L"-qmp",L"tcp:127.0.0.1:"+std::to_wstring(inputPort)+L",server=on,wait=off"});
        if(readonlyRoot) {
            args.insert(args.end(),{L"-device",L"virtio-keyboard-pci,id=wddm-keyboard",
                                    L"-device",L"virtio-tablet-pci,id=wddm-pointer"});
        }
        if (allocationsEnabled) {
            args.push_back(L"-device");
            args.push_back(driver_qemu_args::allocationDevice(transport));
        }
        }
        std::wstring command;
        for (const auto& argument : args) { if (!command.empty()) command += L' '; command += qemu_fence::quote(argument); }
        if (command.size() >= 32767) throw std::runtime_error("Owned QEMU command exceeds the Windows command-line bound");
        STARTUPINFOEXW startup{}; startup.StartupInfo.cb = sizeof startup; startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.value; startup.StartupInfo.hStdOutput = log.value; startup.StartupInfo.hStdError = log.value;
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION child{};
        closesocket(reservation.value); reservation.value = INVALID_SOCKET;
        if(inputReservation.value!=INVALID_SOCKET) { closesocket(inputReservation.value);inputReservation.value=INVALID_SOCKET; }
        if(graphicsReservation.value!=INVALID_SOCKET) { closesocket(graphicsReservation.value);graphicsReservation.value=INVALID_SOCKET; }
        const auto started = ready && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &child);
        if (initialized) DeleteProcThreadAttributeList(attributes);
        if (attributes) HeapFree(GetProcessHeap(), 0, attributes);
        if (!started) throw std::runtime_error("Cannot start owned QEMU runtime");
        // A failed identity/job attachment must still reap the child we just
        // created suspended. It must not leave an unowned suspended process.
        OwnedProcess pending; pending.handle.value = child.hProcess;
        Handle mainThread; mainThread.value = child.hThread;
        process.adoptVerified(child.hProcess,GetCurrentProcess(),
            {child.dwProcessId,processCreationTime(child.hProcess)},executable);
        pending.handle.value = nullptr; CloseHandle(child.hProcess);
        if (launcherProfile && onBound) onBound(process.binding());
        if (launcherProfile && requireLauncherLive) requireLauncherLive();
        const bool resumed = ResumeThread(mainThread.value) != static_cast<DWORD>(-1);
        CloseHandle(mainThread.value); mainThread.value = nullptr;
        if (!resumed) throw std::runtime_error("Cannot start QEMU within its owned job");
        std::cout << "{\"ownedQemuPid\":" << child.dwProcessId << ",\"guestMemoryMiB\":" << memoryMiB << "}\n" << std::flush;
        if (readonlyRoot) std::cout << "{\"ownedQemuReadonlyRootAttached\":true,\"fileNodeReadonly\":true,\"rawNodeReadonly\":true,\"sourceWriteSharingDenied\":true}\n" << std::flush;
        if (launcherProfile) driver_launcher::bootHeadStart(process.handle.value,10000,requireLauncherLive);
        qmp = std::make_unique<Qmp>(qmpPort, process, qmpName);
        if(userNetwork) {
            const auto backend=qmp->call("qom-get",{{"path","/machine/peripheral/browser-nic"},{"property","netdev"}});
            if(backend!="browser-net") throw std::runtime_error("Owned virtio NIC has an unexpected network backend");
            std::cout<<"{\"ownedQemuUserNetworkRealized\":true,\"nic\":\"virtio-net-pci\",\"hostForwards\":[],"
                <<"\"internetConnectivityVerified\":false,\"browserRenderingVerified\":false}\n"<<std::flush;
        }
        if(enableSdl) {
            auto graphicsQmp=std::make_shared<Qmp>(graphicsPort,process,qmpName,
                driver_qmp::ChannelPurpose::SdlGraphics);
            const auto options=graphicsQmp->call("query-display-options");
            if(options!=driver_qmp::Json{{"gl","on"},{"window-close",false},{"type","sdl"},{"show-cursor",launcherProfile && launcherProfile->hostCursor}})
                throw std::runtime_error("Owned SDL display options differ");
            sdlGraphics->command=[graphicsQmp](const std::string& instruction) {
                const auto state=graphicsQmp->relayCommandState(instruction);
                std::cerr<<"{\"nativeSdlGraphicsCommand\":true,\"command\":\""<<instruction<<"\",\"state\":"<<state.dump()<<"}\n";
                return state;
            };
            sdlGraphics->beforeFatalExit=[this]() noexcept {
                // Leave every QMP connection, lease and native object alive.
                // The process will fail closed immediately after this proof.
                process.stop(0);
                try {
                    std::cerr<<driver_qmp::Json{{"nativeSdlFatalOwnerReaped",true},
                        {"ownedQemuReaped",process.reaped},{"qemuExit",process.exit},
                        {"qemuForcedStop",process.forced},{"beforeHostFatalExit",true},
                        {"driverObjectsRetained",true},{"inputImplementationChanged",false}}.dump()<<'\n';
                } catch(...) {
                    std::fprintf(stderr,"SDL_FATAL_OWNER_REAPED beforeHostFatalExit=true driverObjectsRetained=true\n");
                }
            };
            std::cerr<<"{\"nativeSdlGraphicsChannelOpened\":true,\"ownedQemuPid\":"<<child.dwProcessId
                <<",\"session\":\""<<sdlGraphics->session<<"\",\"adapterLuid\":\""<<sdlGraphics->adapterLuid
                <<"\",\"socketPeerVerified\":true,\"dedicatedQmp\":true,\"inputImplementation\":\"official-qemu-sdl\"}\n";
        }
        if(enableWindowInput) {
            auto inputQmp=std::make_shared<Qmp>(inputPort,process,name);
            windowInput=std::make_shared<driver_input::Channel>([inputQmp](const driver_qmp::Json& arguments) {
                const auto reply=inputQmp->call("input-send-event",arguments);
                if(!reply.is_object() || !reply.empty()) throw std::runtime_error("QMP input acknowledgement invalid");
            });
            std::cerr<<"{\"nativeInputChannelOpened\":true,\"ownedQemuPid\":"<<child.dwProcessId
                <<",\"socketPeerVerified\":true,\"dedicatedQmp\":true,\"queueCapacity\":256,\"typedInputVerified\":false}\n";
        }
    }
    std::shared_ptr<driver_input::Channel> inputChannel() const { return windowInput; }
    bool inputSelfTest() const { return inputDiagnostic; }
    std::shared_ptr<driver_sdl::Control> sdlControl() const { return sdlGraphics; }
    ~Runtime() { stop(); report(); }
    FenceLease map(volatile std::uint64_t* fence) {
        if (stopped || !fence || generation == UINT64_MAX) throw std::runtime_error("Fence mapping owner unavailable");
        std::uint32_t slot = 0;
        for (; slot < fenceSlots; ++slot) if (!leases[slot]) break;
        if (slot == fenceSlots) throw std::runtime_error("Fence slot quota exceeded");
        const auto source = reinterpret_cast<std::uintptr_t>(fence);
        const FenceLease lease{slot, ++generation, slot * 4096ull + (source & 4095)};
        try { qmp->mapping("map:" + std::to_string(slot) + ':' + std::to_string(source) + ':' + std::to_string(lease.generation)); }
        catch (...) { failed = true; stop(0); throw; }
        leases[slot] = lease; ++mapped; ++mappedTotal; return lease;
    }
    void unmap(FenceLease lease) {
        if (stopped) return; // stop() verified owned VM exit before clearing pages.
        if (lease.slot >= fenceSlots || !leases[lease.slot] || leases[lease.slot]->generation != lease.generation)
            throw std::runtime_error("Native fence lease ownership mismatch");
        try { qmp->mapping("unmap:" + std::to_string(lease.slot) + ':' + std::to_string(lease.generation)); }
        catch (...) { failed = true; stop(0); throw; }
        leases[lease.slot].reset(); --mapped; ++unmappedTotal;
    }
    void stop(DWORD grace = 5000) noexcept {
        if (stopped) return;
        if(windowInput) {
            const bool clean=windowInput->stop();if(!clean) failed=true;
            std::cerr<<"{\"nativeInputChannelStopped\":"<<(clean?"true":"false")<<",\"workerJoined\":true,\"acknowledgements\":"
                <<windowInput->acknowledgements()<<",\"coalescedMotions\":"<<windowInput->coalescedMotions()
                <<",\"resetCount\":"<<windowInput->resetCount()<<",\"beforeOwnedVmStop\":true}\n";
            windowInput.reset();
        }
        process.stop(grace); // Every driver owner must outlive this step.
        qmp.reset();sdlGraphics.reset();
        fencesReleasedAfterVmExit += mapped;
        allocationsReleasedAfterVmExit += allocationAperture.groups();
        allocationSlotsReleasedAfterVmExit += allocationAperture.chunks();
        for (auto& lease : leases) lease.reset();
        allocationAperture.clearAfterVmExit();
        mapped = 0; stopped = true;
    }
    bool cleanExit() const { return stopped && !failed && !process.forced && process.exit == 0; }
    driver_qemu::ProcessBinding processBinding() const { return process.binding(); }
    bool guestExited() const { return process.exited(); }
    // A launcher watcher may stop only the held process. Lease/GPU teardown
    // stays on the existing serving thread after the owned guest has exited.
    void stopGuest() noexcept { process.stop(0); }
    bool hasStopped() const { return stopped; }
    unsigned liveMappings() const { return mapped; }
    bool canMapFence() const {
        const bool allowed=!stopped && generation!=UINT64_MAX && mapped<fenceSlots;
        if(!allowed) std::cerr<<"{\"nativeFenceAdmissionRejected\":true,\"live\":"<<mapped<<",\"limit\":"<<fenceSlots
            <<",\"runtimeStopped\":"<<(stopped ? "true" : "false")<<",\"generationExhausted\":"<<(generation==UINT64_MAX ? "true" : "false")<<"}\n";
        return allowed;
    }
    std::size_t allocationSlotLimit() const { return allocationSlots; }
    std::size_t fenceSlotLimit() const { return fenceSlots; }
    bool canMapAllocation(std::uint32_t bytes) const {
        const bool allowed=allocationsEnabled && !stopped && allocationAperture.canMap(bytes);
        if(!allowed) std::cerr<<"{\"nativeCpuSlotAdmissionRejected\":true,\"requestedBytes\":"<<bytes
            <<",\"liveChunks\":"<<allocationAperture.chunks()<<",\"slotLimit\":"<<allocationSlots
            <<",\"largestFreeRunSlots\":"<<allocationAperture.largestFreeRunSlots()<<",\"mappedBytes\":"<<allocationAperture.bytes()
            <<",\"runtimeStopped\":"<<(stopped ? "true" : "false")<<",\"cpuEnabled\":"<<(allocationsEnabled ? "true" : "false")<<"}\n";
        return allowed;
    }
    AllocationLease mapAllocation(void* data, std::uint32_t bytes) {
        const auto source = reinterpret_cast<std::uintptr_t>(data);
        if (!canMapAllocation(bytes) || !source || source % 4096 || source > (1ull << 47) - bytes)
            throw std::runtime_error("Invalid owned allocation mapping");
        return allocationAperture.map(bytes, [this, source](std::uint32_t slot, std::uint64_t offset, std::uint32_t chunk, std::uint64_t chunkGeneration) {
            qmp->allocationMapping("map:" + std::to_string(slot) + ':' + std::to_string(source + offset) + ':' +
                                    std::to_string(chunk) + ':' + std::to_string(chunkGeneration));
        }, [this] { failed = true; stop(0); });
    }
    void unmapAllocation(AllocationLease lease) {
        if (stopped) return; // VM exit was proved before its foreign pages were cleared.
        allocationAperture.release(lease, [this](std::uint32_t slot, std::uint64_t chunkGeneration) {
            qmp->allocationMapping("unmap:" + std::to_string(slot) + ':' + std::to_string(chunkGeneration));
        }, [this] { failed = true; stop(0); });
    }
    mutable bool reported = false;
    void report() const {
        if (reported) return;
        // Exception unwinding must preserve the owned-VM exit receipt too.
        // A diagnostic report made while running must not hide final teardown.
        if (stopped) reported = true;
        std::cout << "{\"ownedQemuExited\":" << (stopped ? "true" : "false") << ",\"qemuExit\":" << process.exit
                  << ",\"qemuForcedStop\":" << (process.forced ? "true" : "false")
                  << ",\"fenceControlFailed\":" << (failed ? "true" : "false")
                  << ",\"liveFenceMappings\":" << mapped << ",\"fenceMappingsCreated\":" << mappedTotal
                  << ",\"fenceUnmapAcknowledgements\":" << unmappedTotal
                  << ",\"fenceMappingsReleasedAfterVmExit\":" << fencesReleasedAfterVmExit
                  << ",\"fenceApertureSlots\":" << fenceSlots
                  << ",\"liveAllocationMappings\":" << allocationAperture.groups() << ",\"liveAllocationMappedBytes\":" << allocationAperture.bytes()
                  << ",\"allocationMappingsCreated\":" << allocationAperture.maps()
                  << ",\"allocationMappingsReleasedAfterVmExit\":" << allocationsReleasedAfterVmExit
                  << ",\"liveAllocationSlotMappings\":" << allocationAperture.chunks()
                  << ",\"allocationSlotMappingsCreated\":" << allocationAperture.chunkMaps()
                  << ",\"allocationSlotMappingsReleasedAfterVmExit\":" << allocationSlotsReleasedAfterVmExit
                  << ",\"allocationSlotUnmapAcknowledgements\":" << allocationAperture.chunkUnmaps()
                  << ",\"allocationApertureSlots\":" << allocationSlots
                  << ",\"allocationUnmapAcknowledgements\":" << allocationAperture.unmaps() << "}\n";
    }
};
} // namespace driver_qemu
