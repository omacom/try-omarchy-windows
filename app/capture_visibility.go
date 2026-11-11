package main

import (
	"strings"
	"sync"
	"time"
)

// While the session's displays are off, the session is locked or it has no
// display at all, every window capture on Windows comes back black. The
// blank-desktop check must not take those looks for a window that stayed
// blank, so it pauses while Windows says the screen cannot be seen.
type captureVisibility struct {
	mu                               sync.Mutex
	displayOff, locked, disconnected bool
	// blindSince is when the capture last became unusable, zero while it is
	// usable; blind is the unusable time before that.
	blindSince time.Time
	blind      time.Duration
}

var hostCapture captureVisibility

// Session display states from GUID_SESSION_DISPLAY_STATUS.
const (
	sessionDisplayOff    = 0
	sessionDisplayOn     = 1
	sessionDisplayDimmed = 2
)

// Session change events (WM_WTSSESSION_CHANGE).
const (
	wtsConsoleConnect    = 0x1
	wtsConsoleDisconnect = 0x2
	wtsRemoteConnect     = 0x3
	wtsRemoteDisconnect  = 0x4
	wtsSessionLock       = 0x7
	wtsSessionUnlock     = 0x8
)

// displayState records a display power notification. A dimmed display still
// shows the window, so only off counts.
func (v *captureVisibility) displayState(now time.Time, state uint32) (string, bool) {
	return v.update(now, func() { v.displayOff = state == sessionDisplayOff })
}

// sessionEvent records a lock, unlock, connect or disconnect of this session.
func (v *captureVisibility) sessionEvent(now time.Time, event uintptr) (string, bool) {
	return v.update(now, func() {
		switch event {
		case wtsSessionLock:
			v.locked = true
		case wtsSessionUnlock:
			v.locked = false
		case wtsConsoleDisconnect, wtsRemoteDisconnect:
			v.disconnected = true
		case wtsConsoleConnect, wtsRemoteConnect:
			v.disconnected = false
		}
	})
}

// update applies a change and, when the capture became usable or unusable,
// returns a description of the new state for the log.
func (v *captureVisibility) update(now time.Time, change func()) (string, bool) {
	v.mu.Lock()
	defer v.mu.Unlock()
	was := v.blindLocked()
	change()
	is := v.blindLocked()
	if was == is {
		return "", false
	}
	if is {
		v.blindSince = now
	} else {
		if d := now.Sub(v.blindSince); d > 0 {
			v.blind += d
		}
		v.blindSince = time.Time{}
	}
	return v.describeLocked(), true
}

func (v *captureVisibility) blindLocked() bool { return v.displayOff || v.locked || v.disconnected }

func (v *captureVisibility) describeLocked() string {
	var reasons []string
	if v.displayOff {
		reasons = append(reasons, "the display is off")
	}
	if v.locked {
		reasons = append(reasons, "the session is locked")
	}
	if v.disconnected {
		reasons = append(reasons, "the session is disconnected")
	}
	if len(reasons) == 0 {
		return "the screen is visible again"
	}
	return strings.Join(reasons, ", ")
}

// usable reports whether a capture can show what is on screen.
func (v *captureVisibility) usable() bool {
	v.mu.Lock()
	defer v.mu.Unlock()
	return !v.blindLocked()
}

// blindTime is the total time the capture has been unusable up to now. Only
// differences between two readings mean anything.
func (v *captureVisibility) blindTime(now time.Time) time.Duration {
	v.mu.Lock()
	defer v.mu.Unlock()
	total := v.blind
	if !v.blindSince.IsZero() && now.After(v.blindSince) {
		total += now.Sub(v.blindSince)
	}
	return total
}
