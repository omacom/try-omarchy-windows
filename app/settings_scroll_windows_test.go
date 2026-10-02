//go:build windows

package main

import (
	"bytes"
	"os"
	"os/exec"
	"runtime"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// Compare the displayed viewport after scrolling with a clean repaint at the
// same position. Run on an isolated native Windows desktop at 150% scaling with
// enough work area for the full Settings window (e.g. 2560x1600). No guest runs.
func TestNativeSettingsScrollRepaint(t *testing.T) {
	launcher := os.Getenv("TRYOMARCHY_LAUNCHER_TEST_EXE")
	if os.Getenv("TRYOMARCHY_UI_TEST") != "1" || launcher == "" {
		t.Skip("requires interactive Windows and candidate executable")
	}
	for _, language := range []string{"en", "zh-Hans"} {
		t.Run(language, func(t *testing.T) {
			runtime.LockOSThread()
			defer runtime.UnlockOSThread()
			// Capture screen pixels in physical coordinates even when the launcher is
			// DPI virtualized. Restore the test thread's original awareness afterward.
			dpi := user32.NewProc("SetThreadDpiAwarenessContext")
			previous, _, _ := dpi.Call(^uintptr(3)) // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
			if previous == 0 {
				t.Fatal("could not set capture DPI awareness")
			}
			defer dpi.Call(previous)
			screenHeight, _, _ := procGetSystemMetrics.Call(smCyscreen)
			if screenHeight < 1200 {
				t.Skip("requires at least 1200 physical screen pixels vertically")
			}
			dir := t.TempDir()
			if err := saveResourcePreferences(dir, resourceManual); err != nil {
				t.Fatal(err)
			}
			cmd := exec.Command(launcher, "-dir", dir, "-settings")
			cmd.Env = append(os.Environ(), "TRY_OMARCHY_UI_LANGUAGE="+language)
			if err := cmd.Start(); err != nil {
				t.Fatal(err)
			}
			defer func() { _ = cmd.Process.Kill(); _ = cmd.Wait() }()
			class, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
			var window, viewport uintptr
			for deadline := time.Now().Add(15 * time.Second); time.Now().Before(deadline); {
				window, _, _ = user32.NewProc("FindWindowW").Call(uintptr(unsafe.Pointer(class)), 0)
				var owner uint32
				procGetWindowThreadProcessId.Call(window, uintptr(unsafe.Pointer(&owner)))
				if owner == uint32(cmd.Process.Pid) {
					viewport = findSettingsControl(window, settingsViewportID)
					if viewport != 0 {
						break
					}
				}
				time.Sleep(25 * time.Millisecond)
			}
			if viewport == 0 {
				t.Fatal("Settings viewport did not appear")
			}
			procSetWindowPos.Call(window, hwndTopmost, 20, 20, 0, 0, swpNoSize|swpShowWindow)
			time.Sleep(300 * time.Millisecond)
			for step := 0; step < 20; step++ {
				delta := int32(-120)
				if step%10 >= 5 {
					delta = 120
				}
				procSendMessageW.Call(viewport, 0x020a, uintptr(uint32(delta)<<16), 0)
				time.Sleep(120 * time.Millisecond)
				before := settingsScreenPixels(t, viewport)
				procRedrawWindow.Call(viewport, 0, 0, 0x185)
				time.Sleep(120 * time.Millisecond)
				after := settingsScreenPixels(t, viewport)
				if !bytes.Equal(before, after) {
					t.Fatalf("scroll step %d (wheel %d) leaves pixels changed by a clean repaint", step, delta)
				}
			}
		})
	}
}

func settingsScreenPixels(t *testing.T, window uintptr) []byte {
	t.Helper()
	var rect [4]int32
	procGetWindowRect.Call(window, uintptr(unsafe.Pointer(&rect)))
	width, height := rect[2]-rect[0], rect[3]-rect[1]
	if width <= 0 || height <= 0 {
		t.Fatalf("invalid viewport bounds %v", rect)
	}
	gdi := syscall.NewLazyDLL("gdi32.dll")
	screen, _, _ := user32.NewProc("GetDC").Call(0)
	if screen == 0 {
		t.Fatal("GetDC failed")
	}
	defer user32.NewProc("ReleaseDC").Call(0, screen)
	dc, _, _ := gdi.NewProc("CreateCompatibleDC").Call(screen)
	if dc == 0 {
		t.Fatal("CreateCompatibleDC failed")
	}
	defer gdi.NewProc("DeleteDC").Call(dc)
	bitmap, _, _ := gdi.NewProc("CreateCompatibleBitmap").Call(screen, uintptr(width), uintptr(height))
	if bitmap == 0 {
		t.Fatal("CreateCompatibleBitmap failed")
	}
	defer procDeleteObject.Call(bitmap)
	old, _, _ := procSelectObject.Call(dc, bitmap)
	copied, _, _ := gdi.NewProc("BitBlt").Call(dc, 0, 0, uintptr(width), uintptr(height), screen, uintptr(rect[0]), uintptr(rect[1]), 0x00cc0020) // SRCCOPY
	procSelectObject.Call(dc, old)
	if copied == 0 {
		t.Fatal("BitBlt failed")
	}
	info := struct {
		size                   uint32
		width, height          int32
		planes, bits           uint16
		compression, imageSize uint32
		xppm, yppm             int32
		used, important        uint32
	}{size: 40, width: width, height: -height, planes: 1, bits: 32}
	pixels := make([]byte, int(width)*int(height)*4)
	lines, _, _ := gdi.NewProc("GetDIBits").Call(dc, bitmap, 0, uintptr(height), uintptr(unsafe.Pointer(&pixels[0])), uintptr(unsafe.Pointer(&info)), 0)
	if lines != uintptr(height) {
		t.Fatalf("GetDIBits returned %d of %d lines", lines, height)
	}
	return pixels
}
