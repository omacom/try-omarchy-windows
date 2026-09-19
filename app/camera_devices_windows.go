//go:build windows

package main

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"
)

var guidCameraName = newGUID(0x60d0e559, 0x52f8, 0x4fa2, 0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01)
var guidCameraLink = newGUID(0x58f0aad8, 0x22bf, 0x4f8a, 0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f)

type cameraDevice struct{ ID, Name string }

func cameraAttribute(object unsafe.Pointer, key *comGUID) string {
	var text *uint16
	var count uint32
	if hr := mfCall(object, 13, uintptr(unsafe.Pointer(key)), uintptr(unsafe.Pointer(&text)), uintptr(unsafe.Pointer(&count))); hr < 0 {
		return ""
	}
	if text == nil {
		return ""
	}
	defer procCoTaskMemFree.Call(uintptr(unsafe.Pointer(text)))
	if count > 4096 {
		return ""
	}
	return syscall.UTF16ToString(unsafe.Slice(text, int(count)+1))
}

// Enumeration does not activate a camera or begin capture.
func listCameraDevices() ([]cameraDevice, error) {
	type result struct {
		devices []cameraDevice
		err     error
	}
	done := make(chan result, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		if hr := procCall(procCoInitializeEx, 0, coInitMultithreaded); hr < 0 {
			done <- result{err: fmt.Errorf("camera enumeration could not initialize (0x%08x)", uint32(hr))}
			return
		}
		defer procCoUninitialize.Call()
		if err := startMediaFoundation(); err != nil {
			done <- result{err: err}
			return
		}
		api, _ := mediaFoundation()
		var attrs unsafe.Pointer
		if hr := procCall(api.createAttributes, uintptr(unsafe.Pointer(&attrs)), 1); hr < 0 {
			done <- result{err: fmt.Errorf("camera enumeration unavailable")}
			return
		}
		defer mfRelease(&attrs)
		if hr := setGUID(attrs, &guidDeviceSourceType, &guidDeviceSourceTypeVidcap); hr < 0 {
			done <- result{err: fmt.Errorf("camera enumeration unavailable")}
			return
		}
		var devices *unsafe.Pointer
		var count uint32
		if hr := procCall(api.enumDeviceSources, uintptr(attrs), uintptr(unsafe.Pointer(&devices)), uintptr(unsafe.Pointer(&count))); hr < 0 {
			done <- result{err: fmt.Errorf("Windows could not list cameras (0x%08x)", uint32(hr))}
			return
		}
		if devices == nil {
			done <- result{}
			return
		}
		defer procCoTaskMemFree.Call(uintptr(unsafe.Pointer(devices)))
		var out []cameraDevice
		for _, object := range unsafe.Slice(devices, int(count)) {
			id := cameraAttribute(object, &guidCameraLink)
			name := cameraAttribute(object, &guidCameraName)
			mfRelease(&object)
			if id != "" {
				if name == "" {
					name = "Windows camera"
				}
				out = append(out, cameraDevice{id, name})
			}
		}
		done <- result{devices: out}
	}()
	r := <-done
	return r.devices, r.err
}
