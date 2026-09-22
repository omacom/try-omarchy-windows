//go:build windows

package main

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"
)

var (
	ole32MMDevice          = syscall.NewLazyDLL("ole32.dll")
	procMMCLSIDFromString  = ole32MMDevice.NewProc("CLSIDFromString")
	procMMCoInitializeEx   = ole32MMDevice.NewProc("CoInitializeEx")
	procMMCoUninitialize   = ole32MMDevice.NewProc("CoUninitialize")
	procMMCoCreateInstance = ole32MMDevice.NewProc("CoCreateInstance")
	procMMCoTaskMemFree    = ole32MMDevice.NewProc("CoTaskMemFree")
	procMMPropVariantClear = ole32MMDevice.NewProc("PropVariantClear")
)

// Core Audio endpoint IDs ({0.0.0.00000000}.{guid}) are stable across renames
// and reboots. Friendly names are what SDL matches, so both are returned and
// the launcher resolves ID to name at each start.
const (
	mmDeviceEnumeratorCLSID = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
	mmDeviceEnumeratorIID   = "{A95664D2-9614-4F35-A746-DE8DB63617E6}"
	pkeyDeviceFriendlyName  = "{A45C254E-DF1C-4EFD-8020-67D146A850E0},14"
)

// These layouts match GUID, PROPERTYKEY and PROPVARIANT on 64-bit Windows.
// Named fields give the COM buffers the alignment that byte arrays alone do
// not guarantee.
type mmGUIDValue struct {
	Data1 uint32
	Data2 uint16
	Data3 uint16
	Data4 [8]byte
}

type mmPropertyKey struct {
	FormatID mmGUIDValue
	ID       uint32
}

type mmPropVariant struct {
	Type     uint16
	Reserved [3]uint16
	Value    uintptr
	Extra    uintptr
}

func listAudioEndpoints() (mmDeviceList, error) {
	type result struct {
		list mmDeviceList
		err  error
	}
	done := make(chan result, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		list, err := enumerateMMAudioEndpoints()
		done <- result{list, err}
	}()
	r := <-done
	return r.list, r.err
}

func mmGUID(text string) (g mmGUIDValue, err error) {
	wide, err := syscall.UTF16PtrFromString(text)
	if err != nil {
		return g, err
	}
	hr, _, _ := procMMCLSIDFromString.Call(
		uintptr(unsafe.Pointer(wide)), uintptr(unsafe.Pointer(&g)))
	if hr != 0 {
		return g, fmt.Errorf("bad guid %s", text)
	}
	return g, nil
}

func mmWideString(p uintptr) string {
	if p == 0 {
		return ""
	}
	var text []uint16
	for offset := uintptr(0); offset < 8192; offset += 2 {
		u := *(*uint16)(unsafe.Pointer(p + offset))
		if u == 0 {
			break
		}
		text = append(text, u)
	}
	return syscall.UTF16ToString(text)
}

func mmVCall(obj uintptr, slot int, a1, a2, a3, a4 uintptr) uintptr {
	if obj == 0 {
		return 0xC0000102 // E_POINTER-style failure
	}
	vtable := *(*uintptr)(unsafe.Pointer(obj))
	fn := *(*uintptr)(unsafe.Pointer(vtable + uintptr(slot)*unsafe.Sizeof(uintptr(0))))
	r, _, _ := syscall.SyscallN(fn, obj, a1, a2, a3, a4)
	return r
}

func enumerateMMAudioEndpoints() (mmDeviceList, error) {
	var list mmDeviceList
	hr, _, _ := procMMCoInitializeEx.Call(0, 2)
	if int32(hr) < 0 && uint32(hr) != 0x80010106 { // RPC_E_CHANGED_MODE
		return list, fmt.Errorf("COM init failed: 0x%x", hr)
	}
	if uint32(hr) != 0x80010106 {
		defer procMMCoUninitialize.Call()
	}
	clsid, err := mmGUID(mmDeviceEnumeratorCLSID)
	if err != nil {
		return list, err
	}
	iid, err := mmGUID(mmDeviceEnumeratorIID)
	if err != nil {
		return list, err
	}
	var enumerator uintptr
	hr, _, _ = procMMCoCreateInstance.Call(
		uintptr(unsafe.Pointer(&clsid)), 0, 1,
		uintptr(unsafe.Pointer(&iid)), uintptr(unsafe.Pointer(&enumerator)))
	if int32(hr) < 0 || enumerator == 0 {
		return list, fmt.Errorf("audio endpoint enumeration is unavailable")
	}
	defer mmVCall(enumerator, 2, 0, 0, 0, 0)
	pkey, err := mmGUID(pkeyDeviceFriendlyName[:38])
	if err != nil {
		return list, err
	}
	var pid uint32
	fmt.Sscanf(pkeyDeviceFriendlyName[39:], "%d", &pid)
	pkeyValue := mmPropertyKey{FormatID: pkey, ID: pid}
	for direction, out := range []*[]audioEndpointInfo{&list.Output, &list.Input} {
		var collection uintptr
		// IMMDeviceEnumerator::EnumAudioEndpoints(eRender=0, eCapture=1, DEVICE_STATE_ACTIVE=1)
		hr = mmVCall(enumerator, 3, uintptr(direction), 1,
			uintptr(unsafe.Pointer(&collection)), 0)
		if int32(hr) < 0 || collection == 0 {
			continue
		}
		var count uint32
		mmVCall(collection, 3, uintptr(unsafe.Pointer(&count)), 0, 0, 0)
		if count > 64 {
			count = 64
		}
		for i := uint32(0); i < count; i++ {
			var device uintptr
			if int32(mmVCall(collection, 4, uintptr(i), uintptr(unsafe.Pointer(&device)), 0, 0)) < 0 || device == 0 {
				continue
			}
			var idPtr uintptr
			if int32(mmVCall(device, 5, uintptr(unsafe.Pointer(&idPtr)), 0, 0, 0)) >= 0 && idPtr != 0 {
				id := mmWideString(idPtr)
				procMMCoTaskMemFree.Call(idPtr)
				name, nameErr := mmFriendlyName(device, &pkeyValue)
				if nameErr != nil {
					mmVCall(device, 2, 0, 0, 0, 0)
					mmVCall(collection, 2, 0, 0, 0, 0)
					return list, nameErr
				}
				if id != "" {
					*out = append(*out, audioEndpointInfo{ID: id, Name: name})
				}
			}
			mmVCall(device, 2, 0, 0, 0, 0)
		}
		mmVCall(collection, 2, 0, 0, 0, 0)
	}
	return list, nil
}

func mmFriendlyName(device uintptr, pkey *mmPropertyKey) (string, error) {
	var store uintptr
	hr := mmVCall(device, 4, 0, uintptr(unsafe.Pointer(&store)), 0, 0)
	if int32(hr) < 0 || store == 0 {
		return "", fmt.Errorf("opening audio endpoint properties failed: 0x%x", hr)
	}
	defer mmVCall(store, 2, 0, 0, 0, 0)
	var value mmPropVariant
	hr = mmVCall(store, 5, uintptr(unsafe.Pointer(pkey)),
		uintptr(unsafe.Pointer(&value)), 0, 0)
	if int32(hr) < 0 {
		return "", fmt.Errorf("reading audio endpoint name failed: 0x%x", hr)
	}
	defer procMMPropVariantClear.Call(uintptr(unsafe.Pointer(&value)))
	// VT_LPWSTR = 31
	if value.Type != 31 {
		return "", fmt.Errorf("audio endpoint name has unexpected type %d", value.Type)
	}
	text := mmWideString(value.Value)
	if text == "" {
		return "", fmt.Errorf("audio endpoint name is empty")
	}
	return text, nil
}
