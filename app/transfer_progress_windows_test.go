//go:build windows

package main

import (
	"os"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestNativeTransferProgressCancellation(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("interactive Windows desktop required")
	}
	progress := newTransferProgress("Preparing files")
	defer progress.finish()
	done := make(chan struct{})
	go func() { defer close(done); showTransferProgress(progress) }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchyFileTransfer")
	find := user32.NewProc("FindWindowW")
	var hwnd uintptr
	deadline := time.Now().Add(5 * time.Second)
	for hwnd == 0 && time.Now().Before(deadline) {
		hwnd, _, _ = find.Call(uintptr(unsafe.Pointer(class)), 0)
		time.Sleep(20 * time.Millisecond)
	}
	if hwnd == 0 {
		t.Fatal("transfer window did not open")
	}
	progress.report(8<<20, 16<<20, "Receiving files")
	status, _, _ := user32.NewProc("GetDlgItem").Call(hwnd, 4500)
	var text [256]uint16
	for time.Now().Before(deadline) {
		procGetWindowTextW.Call(status, uintptr(unsafe.Pointer(&text[0])), 256)
		if strings.Contains(syscall.UTF16ToString(text[:]), "8.0 / 16.0 MiB") {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !strings.Contains(syscall.UTF16ToString(text[:]), "8.0 / 16.0 MiB") {
		t.Fatal("progress did not update")
	}
	procPostMessageW.Call(hwnd, wmCommand, 2, 0)
	select {
	case <-progress.ctx.Done():
	case <-time.After(2 * time.Second):
		t.Fatal("Cancel did not cancel transfer")
	}
	progress.finish()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("transfer window did not close")
	}
}

func TestNativeTransferProgressCloseKeepsCopying(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("interactive Windows desktop required")
	}
	progress := newTransferProgress("Copying in background")
	defer progress.finish()
	done := make(chan struct{})
	go func() { defer close(done); showTransferProgress(progress) }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchyFileTransfer")
	var hwnd uintptr
	deadline := time.Now().Add(5 * time.Second)
	for hwnd == 0 && time.Now().Before(deadline) {
		hwnd, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		time.Sleep(20 * time.Millisecond)
	}
	if hwnd == 0 {
		t.Fatal("transfer window did not open")
	}
	procPostMessageW.Call(hwnd, wmClose, 0, 0)
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Close did not dismiss progress")
	}
	if progress.ctx.Err() != nil {
		t.Fatal("dismissing progress cancelled the copy")
	}
}
