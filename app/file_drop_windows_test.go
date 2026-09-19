//go:build windows

package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func TestWindowsShellDropData(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	ole := syscall.NewLazyDLL("ole32.dll")
	hr, _, _ := ole.NewProc("OleInitialize").Call(0)
	if int32(hr) < 0 {
		t.Fatal("OLE initialization failed")
	}
	defer ole.NewProc("OleUninitialize").Call()
	root := t.TempDir()
	paths := []string{filepath.Join(root, "first 世界.txt"), filepath.Join(root, "second.txt")}
	for _, path := range paths {
		if err := os.WriteFile(path, []byte("preserved"), 0600); err != nil {
			t.Fatal(err)
		}
	}
	sequence := clipboardSequence()
	err := withHostDropData(paths, func(object uintptr) error {
		type formatEtc struct {
			format uint16
			target uintptr
			aspect uint32
			index  int32
			medium uint32
		}
		type storageMedium struct {
			kind   uint32
			handle uintptr
			owner  uintptr
		}
		format := formatEtc{format: cfHDrop, aspect: 1, index: -1, medium: 1}
		var medium storageMedium
		table := *(*uintptr)(unsafe.Pointer(object))
		getData := *(*uintptr)(unsafe.Pointer(table + 3*unsafe.Sizeof(uintptr(0))))
		result, _, _ := syscall.SyscallN(getData, object, uintptr(unsafe.Pointer(&format)), uintptr(unsafe.Pointer(&medium)))
		if int32(result) < 0 {
			return fmt.Errorf("Shell did not provide CF_HDROP: %x", result)
		}
		defer ole.NewProc("ReleaseStgMedium").Call(uintptr(unsafe.Pointer(&medium)))
		count, _, _ := shell32.NewProc("DragQueryFileW").Call(medium.handle, 0xffffffff, 0, 0)
		if count != uintptr(len(paths)) {
			return fmt.Errorf("unexpected dropped path count %d", count)
		}
		for i, want := range paths {
			var path [32769]uint16
			shell32.NewProc("DragQueryFileW").Call(medium.handle, uintptr(i), uintptr(unsafe.Pointer(&path[0])), uintptr(len(path)))
			got := syscall.UTF16ToString(path[:])
			actual, actualErr := os.Stat(got)
			expected, expectedErr := os.Stat(want)
			if actualErr != nil || expectedErr != nil || !os.SameFile(actual, expected) {
				return fmt.Errorf("Shell changed dropped file: %q instead of %q", got, want)
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if clipboardSequence() != sequence {
		t.Fatal("drag data preparation changed clipboard")
	}
}

func TestNativeFileDropWindow(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_UI_TEST") != "1" {
		t.Skip("interactive Windows desktop required")
	}
	path := filepath.Join(t.TempDir(), "received.txt")
	os.WriteFile(path, []byte("received"), 0600)
	done := make(chan struct{})
	go func() { defer close(done); showFileDropWindow([]string{path}) }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchyFileDrops")
	var hwnd uintptr
	deadline := time.Now().Add(5 * time.Second)
	for hwnd == 0 && time.Now().Before(deadline) {
		hwnd, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		time.Sleep(20 * time.Millisecond)
	}
	if hwnd == 0 {
		t.Fatal("file transfer window did not open")
	}
	defer procPostMessageW.Call(hwnd, wmClose, 0, 0)
	var count uintptr
	for time.Now().Before(deadline) {
		list, _, _ := user32.NewProc("GetDlgItem").Call(hwnd, 4600)
		count, _, _ = procSendMessageW.Call(list, 0x18b, 0, 0)
		if count == 1 {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if count != 1 {
		t.Fatal("received file not listed", count)
	}
	procPostMessageW.Call(hwnd, wmCommand, 2, 0)
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("file transfer window did not close")
	}
}

// This uses the real Windows OLE source and SDL destination. It requires an
// isolated interactive test desktop because it moves the pointer briefly.
func TestNativeQEMUFileDropEvent(t *testing.T) {
	if os.Getenv("TRYOMARCHY_NATIVE_DROP_TEST") != "1" {
		t.Skip("isolated Windows desktop and runtime r7 required")
	}
	tool := os.Getenv("QEMU_SYSTEM")
	if tool == "" {
		t.Fatal("QEMU_SYSTEM required")
	}
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	ole := syscall.NewLazyDLL("ole32.dll")
	hr, _, _ := ole.NewProc("OleInitialize").Call(0)
	if int32(hr) < 0 {
		t.Fatal("OLE initialization failed")
	}
	defer ole.NewProc("OleUninitialize").Call()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	address := listener.Addr().String()
	listener.Close()
	cmd := exec.Command(tool, "-machine", "q35,accel=tcg", "-m", "64", "-S", "-nodefaults", "-device", displayDevice(&config{displays: 1, displayWidth: 800, displayHeight: 600}, 256<<20), "-display", "sdl,gl=off", "-name", appTitle, "-qmp", "tcp:"+address+",server=on,wait=off")
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	defer func() { cmd.Process.Kill(); cmd.Wait(); qemuPid.Store(0); qemuHwnd.Store(0); enumTitlePid = 0 }()
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	var c *qmpClient
	for ctx.Err() == nil {
		c, err = dialQMPClient(ctx, address)
		if err == nil {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	dir := t.TempDir()
	qemuPid.Store(uint32(cmd.Process.Pid))
	for qemuHwnd.Load() == 0 && ctx.Err() == nil {
		enforceDisplayWindows(qemuPid.Load(), dir, false, 0)
		time.Sleep(50 * time.Millisecond)
	}
	hwnd := qemuHwnd.Load()
	if hwnd == 0 {
		t.Fatal("SDL display did not open")
	}
	procSetForegroundWindow.Call(hwnd)
	path := filepath.Join(dir, "dropped 世界.txt")
	if err := os.WriteFile(path, []byte("original"), 0600); err != nil {
		t.Fatal(err)
	}

	done := make(chan struct{})
	go func() { defer close(done); showFileDropWindow([]string{path}) }()
	class, _ := syscall.UTF16PtrFromString("TryOmarchyFileDrops")
	var source uintptr
	ready := false
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		source, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
		list, _, _ := user32.NewProc("GetDlgItem").Call(source, 4600)
		count, _, _ := procSendMessageW.Call(list, 0x18b, 0, 0)
		visible, _, _ := procIsWindowVisible.Call(source)
		if source != 0 && count == 1 && visible != 0 {
			ready = true
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	if !ready {
		if source != 0 {
			procPostMessageW.Call(source, wmClose, 0, 0)
		}
		t.Fatal("native drag source did not become ready")
	}
	defer func() {
		procPostMessageW.Call(source, wmClose, 0, 0)
		select {
		case <-done:
		case <-time.After(2 * time.Second):
			t.Error("native drag source did not close")
		}
	}()
	width, _, _ := user32.NewProc("GetSystemMetrics").Call(0)
	procShowWindow.Call(hwnd, 9) // SW_RESTORE, so both windows remain visible.
	procSetWindowPos.Call(hwnd, 0, width/2, 40, width/2, 500, 0x10)
	procSetWindowPos.Call(source, 0, 0, 40, width/2, 380, 0x10)
	// Foreground stealing is restricted after earlier UI tests. Activate the
	// source with the same caption click a user would make before dragging.
	mouse := user32.NewProc("mouse_event")
	cursor := user32.NewProc("SetCursorPos")
	defer mouse.Call(4, 0, 0, 0, 0)
	cursor.Call(width/4, 50)
	mouse.Call(2, 0, 0, 0, 0)
	mouse.Call(4, 0, 0, 0, 0)
	deadline = time.Now().Add(2 * time.Second)
	for {
		foreground, _, _ := procGetForegroundWindow.Call()
		if foreground == source {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("drag source did not become active")
		}
		time.Sleep(20 * time.Millisecond)
	}
	point := [2]int32{32, 72} // First received-file row, in client coordinates.
	user32.NewProc("ClientToScreen").Call(source, uintptr(unsafe.Pointer(&point[0])))
	cursor.Call(uintptr(point[0]), uintptr(point[1]))
	mouse.Call(2, 0, 0, 0, 0)
	time.Sleep(100 * time.Millisecond)
	cursor.Call(uintptr(point[0]+12), uintptr(point[1]+12))
	time.Sleep(200 * time.Millisecond)
	cursor.Call(width*3/4, 250)
	time.Sleep(500 * time.Millisecond)
	mouse.Call(4, 0, 0, 0, 0)
	c.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	for c.lines.Scan() {
		paths, _, ok := droppedFilesEvent(c.lines.Text())
		if ok {
			if len(paths) != 1 {
				t.Fatal("native drop changed path count", paths)
			}
			actual, actualErr := os.Stat(paths[0])
			expected, expectedErr := os.Stat(path)
			if actualErr != nil || expectedErr != nil || !os.SameFile(actual, expected) {
				t.Fatal("native drop changed file identity", paths)
			}
			return
		}
	}
	status := ""
	if value, ok := fileDropWindows.Load(source); ok {
		var text [1024]uint16
		procGetWindowTextW.Call(value.(*fileDropWindow).status, uintptr(unsafe.Pointer(&text[0])), uintptr(len(text)))
		status = syscall.UTF16ToString(text[:])
	}
	t.Fatal("native OLE drop did not reach the SDL QMP event", c.lines.Err(), status)
}
