//go:build windows

package main

import (
	"sync/atomic"
	"syscall"
	"unsafe"
)

// Closing the VM window must not hard-kill a running OS (it did: SDL's default
// close quits QEMU instantly - unsaved work in the guest, gone, no questions).
// QEMU now launches with window-close=off so the X does nothing by itself; a
// low-level mouse hook spots clicks on the caption's close button (the window
// itself reports what's under the cursor via WM_NCHITTEST) and the keyboard
// hook catches Alt+F4. Both funnel into one confirmation; Yes performs a
// GRACEFUL guest shutdown over QMP - autologin makes the next start seamless.

var (
	qemuHwnd            atomic.Uintptr // current VM window, set by the title enforcer
	confirmQuit         = make(chan struct{}, 1)
	confirmOpen         atomic.Bool
	procWindowFromPoint = user32.NewProc("WindowFromPoint")
)

const (
	whMouseLL       = 14
	wmLbuttondown   = 0x0201
	htCloseBtn      = 20
	vkF4            = 0x73
	mbDefbutton2    = 0x100
	mbSetForeground = 0x10000
	mbTopmost       = 0x40000
	// mbYesNo, mbIconQuestion, idYes: setup.go
)

// mouseHookCallback swallows left-clicks on the VM window's close button and
// asks for confirmation instead. Runs on the shared hook thread.
func mouseHookCallback(nCode, wParam, lParam uintptr) uintptr {
	if int32(nCode) >= 0 && wParam == wmLbuttondown {
		if hwnd := qemuHwnd.Load(); hwnd != 0 {
			pt := *(*[2]int32)(unsafe.Pointer(lParam)) // MSLLHOOKSTRUCT.pt
			packed := uintptr(uint64(uint32(pt[1]))<<32 | uint64(uint32(pt[0])))
			if under, _, _ := procWindowFromPoint.Call(packed); under == hwnd {
				lp := uintptr(uint32(pt[0])&0xFFFF | uint32(pt[1])<<16)
				if ht, _, _ := procSendMessageW.Call(hwnd, wmNchittest, 0, lp); ht == htCloseBtn {
					requestQuitConfirm()
					return 1 // swallow the click
				}
			}
		}
	}
	r, _, _ := procCallNextHookEx.Call(0, nCode, wParam, lParam)
	return r
}

func requestQuitConfirm() {
	select {
	case confirmQuit <- struct{}{}:
	default:
	}
}

// runCloseGuard owns the confirmation dialog and the graceful shutdown.
func runCloseGuard() {
	text, _ := syscall.UTF16PtrFromString("Shut down Omarchy?\n\nAnything unsaved inside Omarchy will be lost.")
	caption, _ := syscall.UTF16PtrFromString(appTitle)
	for range confirmQuit {
		if confirmOpen.Swap(true) {
			continue // dialog already up
		}
		// Owned by the VM window + SETFOREGROUND, or the dialog opens BEHIND
		// the (foreground, topmost-ish) SDL window it is asking about.
		r, _, _ := procMessageBoxW.Call(qemuHwnd.Load(), uintptr(unsafe.Pointer(text)), uintptr(unsafe.Pointer(caption)),
			mbYesNo|mbIconQuestion|mbDefbutton2|mbTopmost|mbSetForeground)
		if r == idYes {
			logf("close confirmed - graceful guest shutdown")
			if c := qmpConnect(qmpToolsPort, 8e9); c != nil {
				c.writeLine(`{"execute":"system_powerdown"}`)
				c.close()
			}
			// The guest shuts down; the supervisor reaps/exits as usual.
		}
		confirmOpen.Store(false)
	}
}
