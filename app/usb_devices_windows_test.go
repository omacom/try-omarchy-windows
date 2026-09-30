//go:build windows

package main

import (
	"context"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"sync/atomic"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestNativeUSBManagerRefreshAndClose(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("requires an interactive Windows desktop")
	}
	dir, err := os.MkdirTemp(os.TempDir(), "tom-usb-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	previous := qmpControlDirectory
	qmpControlDirectory = func() (string, error) { return dir, nil }
	defer func() { qmpControlDirectory = previous }()
	path, err := qmpControlPath(qmpToolsPort)
	if err != nil {
		t.Fatal(err)
	}
	listener, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	var refreshes atomic.Int32
	go func() {
		for {
			connection, err := listener.Accept()
			if err != nil {
				return
			}
			go func() {
				defer connection.Close()
				encoder := json.NewEncoder(connection)
				decoder := json.NewDecoder(connection)
				encoder.Encode(map[string]any{"QMP": map[string]any{"version": map[string]any{"qemu": map[string]int{"major": 11, "minor": 0, "micro": 0}}, "capabilities": []string{}}})
				for {
					var request struct {
						Execute string `json:"execute"`
						ID      any    `json:"id"`
					}
					if decoder.Decode(&request) != nil {
						return
					}
					var value any = map[string]any{}
					switch request.Execute {
					case "human-monitor-command":
						refreshes.Add(1)
						value = usbSample
					case "qom-list":
						value = []usbQOMEntry{}
					}
					if encoder.Encode(map[string]any{"return": value, "id": request.ID}) != nil {
						return
					}
				}
			}()
		}
	}()
	done := make(chan error, 1)
	go func() { done <- runUSBDeviceUI() }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchyUSBDevices")
	find := user32.NewProc("FindWindowW")
	getItem := user32.NewProc("GetDlgItem")
	var window uintptr
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		window, _, _ = find.Call(uintptr(unsafe.Pointer(class)), 0)
		if window != 0 {
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	if window == 0 {
		t.Fatal("USB window did not open")
	}
	defer procPostMessageW.Call(window, wmClose, 0, 0)
	var list uintptr
	ready := false
	for time.Now().Before(deadline) {
		list, _, _ = getItem.Call(window, 4300)
		count, _, _ := procSendMessageW.Call(list, 0x18b, 0, 0)
		if count == 1 {
			ready = true
			break
		}
		time.Sleep(25 * time.Millisecond)
	}
	if !ready {
		t.Fatal("USB inventory did not reach the list")
	}
	procPostMessageW.Call(window, wmCommand, 4301, 0)
	for refreshes.Load() < 2 && time.Now().Before(deadline) {
		time.Sleep(25 * time.Millisecond)
	}
	if refreshes.Load() < 2 {
		t.Fatal("refresh did not query current devices")
	}
	procPostMessageW.Call(window, wmClose, 0, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(10 * time.Second):
		t.Fatal("USB manager did not close")
	}
}

func TestNativeUSBSelectionConfirmDisableAndClose(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("requires interactive Windows desktop")
	}
	dir := t.TempDir()
	previous := usbSelectionInventory
	usbSelectionInventory = func(context.Context, string) ([]usbDevice, error) { return parseUSBHostDevices(usbSample) }
	defer func() { usbSelectionInventory = previous }()
	done := make(chan error, 1)
	go func() { done <- runUSBSelectionUI(dir, "fixture-runtime") }()
	wait := func(condition func() bool) {
		t.Helper()
		deadline := time.Now().Add(8 * time.Second)
		for time.Now().Before(deadline) {
			if condition() {
				return
			}
			time.Sleep(20 * time.Millisecond)
		}
		t.Fatal("native USB state did not appear")
	}
	find := func(class, title string) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		p, _ := syscall.UTF16PtrFromString(title)
		h, _, _ := user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(p)))
		if h != 0 {
			var pid uint32
			user32.NewProc("GetWindowThreadProcessId").Call(h, uintptr(unsafe.Pointer(&pid)))
			if pid != uint32(os.Getpid()) {
				return 0
			}
		}
		return h
	}
	var h, list uintptr
	wait(func() bool {
		h = find("TryOmarchyUSBDevices", "USB devices")
		if h == 0 {
			return false
		}
		list, _, _ = user32.NewProc("GetDlgItem").Call(h, 4300)
		count, _, _ := procSendMessageW.Call(list, 0x18b, 0, 0)
		return count == 1
	})
	defer procPostMessageW.Call(h, wmClose, 0, 0)
	for _, reply := range []uintptr{idNo, idYes} {
		procPostMessageW.Call(h, wmCommand, 4302, 0)
		var confirmation uintptr
		wait(func() bool { confirmation = find("#32770", "Try Omarchy"); return confirmation != 0 })
		procPostMessageW.Call(confirmation, wmCommand, reply, 0)
		wait(func() bool { return find("#32770", "Try Omarchy") == 0 })
		if reply == idNo {
			if _, err := os.Stat(filepath.Join(dir, usbPreferencesFilename)); !os.IsNotExist(err) {
				t.Fatal("declining granted startup USB access", err)
			}
		} else {
			wait(func() bool { p, e := loadUSBPreferences(dir); return e == nil && p.Enabled && p.Device != nil })
		}
	}
	procPostMessageW.Call(h, wmCommand, 4303, 0)
	wait(func() bool { p, e := loadUSBPreferences(dir); return e == nil && !p.Enabled && p.Device != nil })
	procPostMessageW.Call(h, wmClose, 0, 0)
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(8 * time.Second):
		t.Fatal("USB selection did not close")
	}
}
