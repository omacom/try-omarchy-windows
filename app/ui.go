//go:build windows

package main

import (
	"fmt"
	"os"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// A minimal first-run progress window: one line of status text and a progress
// bar. The download goroutine writes atomics; a WM_TIMER repaints from them.
// Closing the window cancels the setup (nothing is running yet at that point).

var (
	comctl32                 = syscall.NewLazyDLL("comctl32.dll")
	procInitCommonControlsEx = comctl32.NewProc("InitCommonControlsEx")
	procCreateWindowExW      = user32.NewProc("CreateWindowExW")
	procDefWindowProcW       = user32.NewProc("DefWindowProcW")
	procRegisterClassExW     = user32.NewProc("RegisterClassExW")
	procShowWindow           = user32.NewProc("ShowWindow")
	procGetMessageW          = user32.NewProc("GetMessageW")
	procTranslateMessage     = user32.NewProc("TranslateMessage")
	procDispatchMessageW     = user32.NewProc("DispatchMessageW")
	procSetTimer             = user32.NewProc("SetTimer")
	procSendMessageW         = user32.NewProc("SendMessageW")
	procPostQuitMessage      = user32.NewProc("PostQuitMessage")
	procDestroyWindow        = user32.NewProc("DestroyWindow")
	procGetSystemMetrics     = user32.NewProc("GetSystemMetrics")
	procSetForegroundWindow  = user32.NewProc("SetForegroundWindow")
	procCreateFontW          = syscall.NewLazyDLL("gdi32.dll").NewProc("CreateFontW")
	procGetModuleHandleW     = kernel32.NewProc("GetModuleHandleW")
)

const (
	wsOverlapped   = 0x00CF0000 &^ (0x00040000 | 0x00010000) // caption+sysmenu, no resize/maximize
	wsVisible      = 0x10000000
	wsChild        = 0x40000000
	wmDestroy      = 0x0002
	wmClose        = 0x0010
	wmTimer        = 0x0113
	wmSetfont      = 0x0030
	wmSettext      = 0x000C
	pbmSetrange32  = 0x0406
	pbmSetpos      = 0x0402
	swShow         = 5
	smCxscreen     = 0
	smCyscreen     = 1
	iccProgress    = 0x20
)

type progressUI struct {
	status  atomic.Value // string
	cur     atomic.Int64
	total   atomic.Int64
	done    atomic.Bool
	ready   chan struct{}
}

func newProgressUI() *progressUI {
	ui := &progressUI{ready: make(chan struct{})}
	ui.status.Store("Preparing...")
	go ui.run()
	<-ui.ready
	return ui
}

func (ui *progressUI) setStatus(format string, a ...any) { ui.status.Store(fmt.Sprintf(format, a...)) }
func (ui *progressUI) setProgress(cur, total int64)      { ui.cur.Store(cur); ui.total.Store(total) }
func (ui *progressUI) finish()                           { ui.done.Store(true) }

func (ui *progressUI) run() {
	runtime.LockOSThread()
	type iccex struct{ size, icc uint32 }
	ic := iccex{8, iccProgress}
	procInitCommonControlsEx.Call(uintptr(unsafe.Pointer(&ic)))
	hInst, _, _ := procGetModuleHandleW.Call(0)

	className, _ := syscall.UTF16PtrFromString("TryOmarchySetup")
	var hText, hBar uintptr
	lastStatus := ""

	wndProc := syscall.NewCallback(func(hwnd, msg, wParam, lParam uintptr) uintptr {
		switch msg {
		case wmTimer:
			if ui.done.Load() {
				procDestroyWindow.Call(hwnd)
				return 0
			}
			if s := ui.status.Load().(string); s != lastStatus {
				lastStatus = s
				t, _ := syscall.UTF16PtrFromString(s)
				procSendMessageW.Call(hText, wmSettext, 0, uintptr(unsafe.Pointer(t)))
			}
			total := ui.total.Load()
			if total > 0 {
				procSendMessageW.Call(hBar, pbmSetrange32, 0, 1000)
				procSendMessageW.Call(hBar, pbmSetpos, uintptr(ui.cur.Load()*1000/total), 0)
			}
			return 0
		case wmClose:
			// Cancel: setup is the only thing running when this window exists.
			os.Exit(1)
		case wmDestroy:
			procPostQuitMessage.Call(0)
			return 0
		}
		r, _, _ := procDefWindowProcW.Call(hwnd, msg, wParam, lParam)
		return r
	})

	// Field types must mirror WNDCLASSEXW exactly: cbClsExtra/cbWndExtra are C
	// ints (4 bytes), NOT pointer-sized. Getting this wrong inflates cbSize and
	// RegisterClassExW rejects the struct - silently, if nobody checks (it
	// shipped that way once: no progress window, no error, download running
	// blind. Check every return value here).
	type wndclassex struct {
		size, style         uint32
		wndProc             uintptr
		clsExtra, wndExtra  int32
		inst                uintptr
		icon, cursor, brush uintptr
		menuName, className *uint16
		iconSm              uintptr
	}
	wc := wndclassex{
		size: uint32(unsafe.Sizeof(wndclassex{})), wndProc: wndProc, inst: hInst,
		brush: 16, className: className, // 15+1 = COLOR_3DFACE+1
	}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		logf("progress UI: RegisterClassExW failed: %v", err)
		close(ui.ready)
		return
	}

	const w, h = 420, 130
	sx, _, _ := procGetSystemMetrics.Call(smCxscreen)
	sy, _, _ := procGetSystemMetrics.Call(smCyscreen)
	title, _ := syscall.UTF16PtrFromString(appTitle)
	hwnd, _, err := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		wsOverlapped|wsVisible, (sx-w)/2, (sy-h)/2, w, h, 0, 0, hInst, 0)
	if hwnd == 0 {
		logf("progress UI: CreateWindowExW failed: %v", err)
		close(ui.ready)
		return
	}

	staticClass, _ := syscall.UTF16PtrFromString("STATIC")
	empty, _ := syscall.UTF16PtrFromString("")
	hText, _, _ = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(staticClass)), uintptr(unsafe.Pointer(empty)),
		wsChild|wsVisible, 16, 16, w-48, 22, hwnd, 0, hInst, 0)
	barClass, _ := syscall.UTF16PtrFromString("msctls_progress32")
	hBar, _, _ = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(barClass)), uintptr(unsafe.Pointer(empty)),
		wsChild|wsVisible, 16, 48, w-48, 20, hwnd, 0, hInst, 0)

	fontName, _ := syscall.UTF16PtrFromString("Segoe UI")
	font, _, _ := procCreateFontW.Call(^uintptr(15)+1, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 5, 0, uintptr(unsafe.Pointer(fontName)))
	procSendMessageW.Call(hText, wmSetfont, font, 1)

	procSetTimer.Call(hwnd, 1, 100, 0)
	procShowWindow.Call(hwnd, swShow)
	// Launched without foreground rights (shortcut helpers, background shells)
	// the window opens buried; ask for the front anyway - best effort.
	procSetForegroundWindow.Call(hwnd)
	close(ui.ready)

	var m msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			return
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
}
