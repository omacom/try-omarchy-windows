//go:build windows

package main

import (
	"fmt"
	"path/filepath"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

// DXGI_ADAPTER_DESC1 uses pointer-sized memory fields and an eight-byte LUID.
// GetDesc1 is slot 10; IDXGIFactory1::EnumAdapters1 is slot 12 and
// IDXGIFactory6::EnumAdapterByGpuPreference is slot 29 in the COM vtable.
type dxgiAdapterDesc struct {
	Name                                    [128]uint16
	Vendor, Device, Subsystem, Revision     uint32
	VideoMemory, SystemMemory, SharedMemory uintptr
	LUIDLow                                 uint32
	LUIDHigh                                int32
	Flags                                   uint32
}

var (
	dxgiFactory1IID = mmGUIDValue{0x770aae78, 0xf26f, 0x4dba, [8]byte{0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}}
	dxgiFactory6IID = mmGUIDValue{0xc1b6694f, 0xff09, 0x44a9, [8]byte{0xb0, 0x3c, 0x77, 0x90, 0x0a, 0x0a, 0x1d, 0x17}}
	dxgiAdapter1IID = mmGUIDValue{0x29038f61, 0x3839, 0x4626, [8]byte{0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}}
)

//go:uintptrescapes
func dxgiCall(object uintptr, slot int, args ...uintptr) uintptr {
	table := *(*uintptr)(unsafe.Pointer(object))
	method := *(*uintptr)(unsafe.Pointer(table + uintptr(slot)*unsafe.Sizeof(uintptr(0))))
	result, _, _ := syscall.SyscallN(method, append([]uintptr{object}, args...)...)
	return result
}

func dxgiAdapterFacts() map[string]string {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	// COM initialization is scoped to this thread; do not uninitialize an
	// apartment owned elsewhere if it uses a different threading model.
	hr, _, _ := procMMCoInitializeEx.Call(0, 0)
	if int32(hr) >= 0 {
		defer procMMCoUninitialize.Call()
	}
	facts := map[string]string{
		"gpu.adapters":              "unavailable",
		"gpu.order.highPerformance": "unavailable (IDXGIFactory6 unsupported)",
		"gpu.order.minimumPower":    "unavailable (IDXGIFactory6 unsupported)",
	}
	create := syscall.NewLazyDLL("dxgi.dll").NewProc("CreateDXGIFactory1")
	if err := create.Find(); err != nil {
		facts["gpu.adapters"] = "unavailable: " + err.Error()
		return facts
	}
	var factory uintptr
	hr, _, _ = create.Call(uintptr(unsafe.Pointer(&dxgiFactory1IID)), uintptr(unsafe.Pointer(&factory)))
	if int32(hr) < 0 || factory == 0 {
		facts["gpu.adapters"] = fmt.Sprintf("unavailable: CreateDXGIFactory1 HRESULT 0x%08x", uint32(hr))
		return facts
	}
	defer dxgiCall(factory, 2)
	enumerate := func(f uintptr, preference *uint32) string {
		var entries []string
		for i := uint32(0); i < 64; i++ {
			var adapter uintptr
			var result uintptr
			if preference == nil {
				result = dxgiCall(f, 12, uintptr(i), uintptr(unsafe.Pointer(&adapter)))
			} else {
				result = dxgiCall(f, 29, uintptr(i), uintptr(*preference), uintptr(unsafe.Pointer(&dxgiAdapter1IID)), uintptr(unsafe.Pointer(&adapter)))
			}
			if uint32(result) == 0x887a0002 {
				break
			} // DXGI_ERROR_NOT_FOUND
			if int32(result) < 0 || adapter == 0 {
				entries = append(entries, fmt.Sprintf("enumeration failed at %d: HRESULT 0x%08x", i, uint32(result)))
				break
			}
			var desc dxgiAdapterDesc
			result = dxgiCall(adapter, 10, uintptr(unsafe.Pointer(&desc)))
			dxgiCall(adapter, 2)
			if int32(result) < 0 {
				entries = append(entries, fmt.Sprintf("%d: GetDesc1 HRESULT 0x%08x", i, uint32(result)))
				continue
			}
			entries = append(entries, fmt.Sprintf("%d: %s vendor=0x%04x device=0x%04x LUID=%08x:%08x software=%v", i, syscall.UTF16ToString(desc.Name[:]), desc.Vendor, desc.Device, uint32(desc.LUIDHigh), desc.LUIDLow, desc.Flags&2 != 0))
		}
		if len(entries) == 0 {
			return "no adapters enumerated"
		}
		return strings.Join(entries, "; ")
	}
	facts["gpu.adapters"] = enumerate(factory, nil)
	var factory6 uintptr
	if result := dxgiCall(factory, 0, uintptr(unsafe.Pointer(&dxgiFactory6IID)), uintptr(unsafe.Pointer(&factory6))); int32(result) >= 0 && factory6 != 0 {
		defer dxgiCall(factory6, 2)
		minimum, high := uint32(1), uint32(2)
		facts["gpu.order.highPerformance"] = enumerate(factory6, &high)
		facts["gpu.order.minimumPower"] = enumerate(factory6, &minimum)
	}
	return facts
}

func qemuGPUPreference(executable string) string {
	path, err := filepath.Abs(executable)
	if err != nil {
		return "unavailable: " + err.Error()
	}
	keyPath, _ := syscall.UTF16PtrFromString(`Software\Microsoft\DirectX\UserGpuPreferences`)
	var key syscall.Handle
	if err := syscall.RegOpenKeyEx(syscall.HKEY_CURRENT_USER, keyPath, 0, syscall.KEY_READ, &key); err != nil {
		if err == syscall.ERROR_FILE_NOT_FOUND {
			return "unset (Windows default)"
		}
		return "unavailable: " + err.Error()
	}
	defer syscall.RegCloseKey(key)
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return "unavailable: " + err.Error()
	}
	var kind, size uint32
	err = syscall.RegQueryValueEx(key, name, nil, &kind, nil, &size)
	if err == syscall.ERROR_FILE_NOT_FOUND {
		return "unset (Windows default)"
	}
	if err != nil {
		return "unavailable: " + err.Error()
	}
	if kind != syscall.REG_SZ || size < 2 || size > 4096 || size%2 != 0 {
		return "unavailable: invalid registry value"
	}
	buf := make([]uint16, size/2)
	if err := syscall.RegQueryValueEx(key, name, nil, &kind, (*byte)(unsafe.Pointer(&buf[0])), &size); err != nil {
		return "unavailable: " + err.Error()
	}
	return syscall.UTF16ToString(buf)
}

func gpuFreezeFacts(cfg *config) map[string]string {
	facts := launcherFacts(cfg)
	facts["recovery.reason"] = "post-ready GPU main loop unresponsive"
	facts["recovery.qmpMisses"] = fmt.Sprint(qmpHangMisses)
	facts["recovery.qmpProbeSeconds"] = "5"
	facts["recovery.qemuPID"] = fmt.Sprint(qemuPid.Load())
	return facts
}
