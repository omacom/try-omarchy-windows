// SPDX-License-Identifier: MIT
// Private guest DSO: observe a real native fence after process exit hooks,
// when driver-library finalizers can still use their existing mapped storage.
#include <wsl/winadapter.h>
#include <dxg/d3dkmthk.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>

static volatile std::uint64_t* fence = nullptr;
static int descriptor = -1;
template<class T> static bool call(unsigned nr, T& value) {
    if (!ioctl(descriptor, _IOC(_IOC_READ | _IOC_WRITE, 'G', nr, sizeof value), &value)) return true;
    std::printf("EXIT_LIFETIME_FAILURE nr=%u errno=%d\n", nr, errno);
    return false;
}

extern "C" int wddm_exit_lifetime_arm() {
    if (fence || descriptor >= 0) return 1;
    descriptor = open("/dev/dxg", O_RDWR | O_CLOEXEC);
    if (descriptor < 0) return 1;
    D3DKMT_OPENADAPTERFROMLUID adapter{}; adapter.AdapterLuid.LowPart = 0x57475055;
    if (!call(1, adapter)) return 1;
    D3DKMT_CREATEDEVICE device{}; device.hAdapter = adapter.hAdapter; device.Flags.RequestVSync = 1;
    if (!call(2, device)) return 1;
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 sync{}; sync.hDevice = device.hDevice;
    sync.Info.Type = D3DDDI_MONITORED_FENCE;
    sync.Info.Flags.NoGPUAccess = 1;
    sync.Info.MonitoredFence.InitialFenceValue = 57;
    sync.Info.MonitoredFence.EngineAffinity = 1;
    if (!call(16, sync) || !sync.Info.MonitoredFence.FenceValueCPUVirtualAddress ||
        sync.Info.MonitoredFence.FenceValueGPUVirtualAddress) return 1;
    fence = static_cast<volatile std::uint64_t*>(sync.Info.MonitoredFence.FenceValueCPUVirtualAddress);
    if (*fence != 57) return 1;
    std::puts("EXIT_LIFETIME_ARMED nativeFence=true observed=57 lateLibraryFinalizer=true");
    return 0;
}

__attribute__((destructor)) static void finalizer() {
    if (!fence) return;
    std::puts("EXIT_LIFETIME_FINALIZER_BEGIN processExit=true");
    // No ioctl or new mapping: a driver finalizer retains its original CPU view.
    const auto observed = *fence;
    if (observed != 57) {
        std::printf("EXIT_LIFETIME_FAILURE observed=%llu\n", static_cast<unsigned long long>(observed));
        _exit(91);
    }
    std::puts("EXIT_LIFETIME_FINALIZER_COMPLETE observed=57 mappingLive=true");
    close(descriptor);
}
