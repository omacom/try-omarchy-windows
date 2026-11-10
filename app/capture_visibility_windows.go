//go:build windows

package main

import (
	"fmt"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// Windows side of captureVisibility: the tray window hears when this
// session's display turns off or on and when the session is locked,
// unlocked, connected or disconnected.

var (
	procRegisterPowerSettingNotification   = user32.NewProc("RegisterPowerSettingNotification")
	procUnregisterPowerSettingNotification = user32.NewProc("UnregisterPowerSettingNotification")
	wtsapi32                               = syscall.NewLazyDLL("wtsapi32.dll")
	procWTSRegisterSessionNotification     = wtsapi32.NewProc("WTSRegisterSessionNotification")
	procWTSUnRegisterSessionNotification   = wtsapi32.NewProc("WTSUnRegisterSessionNotification")
)

// GUID_SESSION_DISPLAY_STATUS: the display of this session turned off, on or
// dimmed. Windows sends the current state as soon as an app registers.
var guidSessionDisplayStatus = comGUID{0x2B84C20E, 0xAD23, 0x4DDF, [8]byte{0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5}}

const (
	pbtPowerSettingChange = 0x8013
	wmWtsSessionChange    = 0x02B1
)

// powerBroadcastSetting is the head of POWERBROADCAST_SETTING; Data follows.
type powerBroadcastSetting struct {
	powerSetting comGUID
	dataLength   uint32
}

// sessionDisplayStatus reads a PBT_POWERSETTINGCHANGE message. It reports
// false for any other power setting.
func sessionDisplayStatus(setting *powerBroadcastSetting) (uint32, bool) {
	if setting == nil || setting.powerSetting != guidSessionDisplayStatus || setting.dataLength < 4 {
		return 0, false
	}
	data := unsafe.Add(unsafe.Pointer(setting), unsafe.Sizeof(*setting))
	return *(*uint32)(data), true
}

func logCaptureChange(state string, changed bool) {
	if changed {
		logf("display health: %s; desktop checks %s", state, map[bool]string{true: "resume", false: "pause"}[hostCapture.usable()])
	}
}

// captureWindowMessage handles the tray window's display and session
// messages. It reports false for any other message.
func captureWindowMessage(message uint32, wParam, lParam uintptr) bool {
	switch {
	case message == wmPowerbroadcast && wParam == pbtPowerSettingChange:
		if state, ok := sessionDisplayStatus((*powerBroadcastSetting)(unsafe.Pointer(lParam))); ok {
			logCaptureChange(hostCapture.displayState(time.Now(), state))
		}
		return true
	case message == wmWtsSessionChange:
		logCaptureChange(hostCapture.sessionEvent(time.Now(), wParam))
		return true
	}
	return false
}

// registerCaptureNotifications asks Windows for the display and session
// messages. Without them the blank-desktop check keeps its old behaviour.
func registerCaptureNotifications(hwnd uintptr) func() {
	var cleanups []func()
	handle, _, err := procRegisterPowerSettingNotification.Call(hwnd, uintptr(unsafe.Pointer(&guidSessionDisplayStatus)), 0) // DEVICE_NOTIFY_WINDOW_HANDLE
	if handle == 0 {
		logf("display health: display power notifications unavailable: %v", err)
	} else {
		cleanups = append(cleanups, func() { procUnregisterPowerSettingNotification.Call(handle) })
	}
	if err := wtsRegister(hwnd); err != nil {
		logf("display health: session lock notifications unavailable: %v", err)
	} else {
		cleanups = append(cleanups, func() { procWTSUnRegisterSessionNotification.Call(hwnd) })
	}
	var once sync.Once
	return func() {
		once.Do(func() {
			for _, cleanup := range cleanups {
				cleanup()
			}
		})
	}
}

func wtsRegister(hwnd uintptr) error {
	if err := procWTSRegisterSessionNotification.Find(); err != nil {
		return err
	}
	if ok, _, err := procWTSRegisterSessionNotification.Call(hwnd, 0); ok == 0 { // NOTIFY_FOR_THIS_SESSION
		return fmt.Errorf("WTSRegisterSessionNotification: %w", err)
	}
	return nil
}
