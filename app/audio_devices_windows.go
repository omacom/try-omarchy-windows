//go:build windows

package main

import (
	"fmt"
	"path/filepath"
	"runtime"
	"syscall"
	"unsafe"
)

type audioDeviceCatalog struct{ Output, Input []string }

// Enumerate through the same SDL library as QEMU so duplicate-name suffixes
// and backend-specific names agree exactly. Enumeration never opens a stream.
func listAudioDevices(qemu string) (audioDeviceCatalog, error) {
	type result struct {
		catalog audioDeviceCatalog
		err     error
	}
	done := make(chan result, 1)
	go func() {
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		catalog, err := enumerateSDLAudio(filepath.Join(filepath.Dir(qemu), "SDL2.dll"))
		done <- result{catalog, err}
	}()
	r := <-done
	return r.catalog, r.err
}

func enumerateSDLAudio(path string) (audioDeviceCatalog, error) {
	var catalog audioDeviceCatalog
	full, err := filepath.Abs(path)
	if err != nil {
		return catalog, err
	}
	wide, err := syscall.UTF16PtrFromString(full)
	if err != nil {
		return catalog, err
	}
	// Search dependencies only beside the selected runtime and in System32.
	h, _, loadErr := kernel32.NewProc("LoadLibraryExW").Call(uintptr(unsafe.Pointer(wide)), 0, 0x100|0x800)
	if h == 0 {
		return catalog, fmt.Errorf("cannot load runtime audio library: %w", loadErr)
	}
	dll := &syscall.DLL{Name: full, Handle: syscall.Handle(h)}
	defer dll.Release()
	init, err := dll.FindProc("SDL_InitSubSystem")
	if err != nil {
		return catalog, err
	}
	quit, err := dll.FindProc("SDL_QuitSubSystem")
	if err != nil {
		return catalog, err
	}
	count, err := dll.FindProc("SDL_GetNumAudioDevices")
	if err != nil {
		return catalog, err
	}
	name, err := dll.FindProc("SDL_GetAudioDeviceName")
	if err != nil {
		return catalog, err
	}
	r, _, _ := init.Call(0x10)
	if int32(r) != 0 {
		return catalog, fmt.Errorf("Windows audio enumeration is unavailable")
	}
	defer quit.Call(0x10)
	for direction, devices := range []*[]string{&catalog.Output, &catalog.Input} {
		n, _, _ := count.Call(uintptr(direction))
		if int32(n) < 0 || n > 1024 {
			return catalog, fmt.Errorf("Windows could not list audio devices")
		}
		for i := uintptr(0); i < n; i++ {
			address, _, _ := name.Call(i, uintptr(direction))
			if address == 0 {
				continue
			}
			var text []byte
			for offset := uintptr(0); offset < 4096; offset++ {
				b := *(*byte)(unsafe.Pointer(address + offset))
				if b == 0 {
					break
				}
				text = append(text, b)
			}
			if len(text) > 0 && len(text) < 4096 {
				*devices = append(*devices, string(text))
			}
		}
	}
	return catalog, nil
}
