//go:build windows

package main

import (
	"fmt"
	"runtime"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

// The Windows-key forwarder, ported from scripts/winkey-forwarder.ps1.
// While the QEMU window is foreground: swallow Win on the host and forward it
// to the guest as Super (meta_l) over a dedicated QMP socket. Otherwise the key
// behaves normally. Pair with SDL_GRAB_KEYBOARD=0 so SDL never installs its own
// (system-wide) hook.

var (
	qemuPid   atomic.Uint32 // current QEMU child, set by the supervisor
	guestUp   atomic.Bool   // supervisor handshake succeeded for this launch
	winDown   bool          // hook-thread only
	keyEvents = make(chan bool, 64)
)

func hookCallback(nCode, wParam, lParam uintptr) uintptr {
	if int32(nCode) >= 0 {
		vk := *(*uint32)(unsafe.Pointer(lParam))  // KBDLLHOOKSTRUCT.vkCode
		if vk == vkF4 && wParam == wmSyskeydown { // Alt+F4 on the VM window
			if pid := qemuPid.Load(); pid != 0 && foregroundPid() == pid {
				requestQuitConfirm()
				return 1 // swallow; the close guard takes it from here
			}
		}
		if vk == vkLwin || vk == vkRwin {
			down := wParam == wmKeydown || wParam == wmSyskeydown
			pid := qemuPid.Load()
			if pid != 0 && foregroundPid() == pid {
				if down != winDown {
					winDown = down
					select {
					case keyEvents <- down:
					default:
					}
				}
				return 1 // swallow on host; QMP delivers it to the guest
			}
			if winDown { // focus left mid-press: release in the guest
				winDown = false
				select {
				case keyEvents <- false:
				default:
				}
			}
			// VM not focused: approve the key and SKIP the rest of the hook
			// chain. QEMU installs its own LL hook on every grab which
			// swallows Win even when unfocused; returning 0 without
			// CallNextHookEx bypasses it.
			return 0
		}
	}
	r, _, _ := procCallNextHookEx.Call(0, nCode, wParam, lParam)
	return r
}

// runWinKeyHook owns the hook and its message pump. LL hooks run newest-first
// and QEMU re-installs its own on every grab toggle, so the hook is torn down
// and re-installed every ~800ms to stay at the front of the chain.
func runWinKeyHook() {
	runtime.LockOSThread()
	cb := syscall.NewCallback(hookCallback)
	mcb := syscall.NewCallback(mouseHookCallback)
	install := func() uintptr {
		h, _, _ := procSetWindowsHookExW.Call(whKeyboardLL, cb, 0, 0)
		return h
	}
	h := install()
	if h == 0 {
		logf("winkey: SetWindowsHookEx failed - Super forwarding disabled")
		return
	}
	// The close guard's mouse hook shares this thread's pump. Installed once;
	// only the keyboard hook needs the front-of-chain rehook dance (QEMU's
	// competing hook is keyboard-only).
	if mh, _, _ := procSetWindowsHookExW.Call(whMouseLL, mcb, 0, 0); mh == 0 {
		logf("closeguard: mouse hook failed - X clicks will be ignored (window-close=off)")
	}
	var m msgStruct
	for {
		procMsgWaitForMultipleObj.Call(0, 0, 0, 800, qsAllinput)
		for {
			r, _, _ := procPeekMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0, pmRemove)
			if r == 0 {
				break
			}
		}
		procUnhookWindowsHookEx.Call(h)
		h = install()
		if h == 0 {
			time.Sleep(500 * time.Millisecond)
			h = install()
		}
	}
}

// runWinKeyQmp drains forwarded key events into the guest, reconnecting to the
// forwarder QMP socket whenever QEMU restarts. It never dials before the
// supervisor's handshake succeeds: a QMP connection during early guest boot
// reliably wedges QEMU's main loop under WHPX (see docs/FINDINGS.md).
func runWinKeyQmp() {
	for {
		if !guestUp.Load() {
			time.Sleep(time.Second)
			continue
		}
		c := qmpConnect(qmpFwdPort, 8*time.Second)
		if c == nil {
			time.Sleep(2 * time.Second)
			continue
		}
		logf("winkey: QMP connected on %d", qmpFwdPort)
		lines := c.readLines()
	drain:
		for {
			select {
			case down := <-keyEvents:
				ev := fmt.Sprintf(`{"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"down":%t,"key":{"type":"qcode","data":"meta_l"}}}]}}`, down)
				if err := c.writeLine(ev); err != nil {
					break drain
				}
			case _, ok := <-lines:
				if !ok {
					break drain
				}
			}
		}
		c.close()
		time.Sleep(2 * time.Second)
	}
}

// runTitleEnforcer keeps the VM window branded (QEMU resets its title on every
// grab toggle, so this reasserts every second), maximizes it once per launch
// when the window first appears, and stamps our icon over QEMU's on the
// window + taskbar (the SDL window belongs to qemu-system-*.exe, so without
// this the taskbar shows the QEMU logo - the last piece of QEMU chrome).
// It also remembers where the user leaves the window: the placement is saved
// whenever it changes and restored, in place of the maximized default, on
// the next windowed launch if that spot is still on a connected display.
func runTitleEnforcer(dir string, fullscreen bool) {
	hInst, _, _ := procGetModuleHandleW.Call(0)
	appIcon, _, _ := procLoadIconW.Call(hInst, 1) // the embedded Omarchy .ico
	lastPid := uint32(0)
	maximize := false
	var restore, last *windowPlacement
	if !fullscreen {
		restore = rememberedWindow(dir)
		last = restore
	}
	for {
		if pid := qemuPid.Load(); pid != 0 {
			if pid != lastPid {
				lastPid = pid
				maximize = !fullscreen
			}
			enforceTitle(pid, &maximize, appIcon, restore)
			if hwnd := qemuHwnd.Load(); hwnd != 0 && !fullscreen && !maximize {
				if now := capturePlacement(hwnd); now != nil && !now.sameAs(last) {
					now.SavedAt = time.Now()
					if err := saveWindowPlacement(dir, *now); err == nil {
						last = now
					}
				}
			}
		} else {
			lastPid = 0
			qemuHwnd.Store(0)
		}
		time.Sleep(time.Second)
	}
}

// runCursorReleaseGuard keeps the SDL frontend from confining the Windows
// cursor to the VM. SDL re-applies its grab whenever the window gains focus,
// so this must watch the full lifetime rather than run only at launch.
func runCursorReleaseGuard() {
	ticker := time.NewTicker(50 * time.Millisecond)
	defer ticker.Stop()
	for range ticker.C {
		releaseQemuCursor()
	}
}
