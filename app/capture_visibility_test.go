package main

import (
	"testing"
	"time"
)

func TestCaptureVisibilityCountsTimeTheScreenCannotBeSeen(t *testing.T) {
	start := time.Unix(1000, 0)
	var v captureVisibility
	if !v.usable() || v.blindTime(start) != 0 {
		t.Fatal("the screen starts visible")
	}
	if state, changed := v.displayState(start, sessionDisplayDimmed); changed || !v.usable() {
		t.Fatalf("a dimmed display still shows the window (%q)", state)
	}
	if state, changed := v.displayState(start.Add(time.Minute), sessionDisplayOff); !changed || v.usable() || state != "the display is off" {
		t.Fatalf("display off: changed=%t usable=%t state=%q", changed, v.usable(), state)
	}
	// A lock while the display is already off changes nothing visible.
	if _, changed := v.sessionEvent(start.Add(2*time.Minute), wtsSessionLock); changed {
		t.Fatal("locking a dark screen reported a change")
	}
	if got := v.blindTime(start.Add(3 * time.Minute)); got != 2*time.Minute {
		t.Fatalf("blind time while still dark = %s", got)
	}
	if _, changed := v.displayState(start.Add(4*time.Minute), sessionDisplayOn); changed || v.usable() {
		t.Fatal("the locked session still cannot be seen")
	}
	state, changed := v.sessionEvent(start.Add(5*time.Minute), wtsSessionUnlock)
	if !changed || !v.usable() || state != "the screen is visible again" {
		t.Fatalf("unlock: changed=%t usable=%t state=%q", changed, v.usable(), state)
	}
	if got := v.blindTime(start.Add(time.Hour)); got != 4*time.Minute {
		t.Fatalf("blind time after the screen came back = %s", got)
	}
	v.sessionEvent(start.Add(2*time.Hour), wtsConsoleDisconnect)
	v.sessionEvent(start.Add(2*time.Hour+time.Minute), wtsRemoteConnect)
	if !v.usable() || v.blindTime(start.Add(3*time.Hour)) != 5*time.Minute {
		t.Fatal("a disconnected session counts as unseen until it connects again")
	}
}

// A first boot that runs while the displays sleep must not ask about a blank
// window: only the time the screen could be seen counts toward the wait.
func TestBlankDesktopWaitLeavesOutTimeTheScreenWasDark(t *testing.T) {
	start := time.Unix(1000, 0)
	var v captureVisibility
	v.displayState(start, sessionDisplayOff)
	session := start.Add(time.Minute)
	times := bootCurtainTimes{boot: start, ready: session, session: session, readyBlind: v.blindTime(session), sessionBlind: v.blindTime(session)}
	awake := start.Add(30 * time.Minute)
	v.displayState(awake, sessionDisplayOn)
	if times.blankDesktopDue(awake.Add(time.Minute), blankDesktopLooks, v.blindTime(awake.Add(time.Minute))) {
		t.Fatal("asked after one minute of a visible window")
	}
	if !times.blankDesktopDue(awake.Add(blankDesktopWait), blankDesktopLooks, v.blindTime(awake.Add(blankDesktopWait))) {
		t.Fatal("a window that stayed blank for the whole visible wait must still be asked about")
	}
	// Dark time before the session started is not subtracted again.
	var early captureVisibility
	early.displayState(start, sessionDisplayOff)
	early.displayState(session, sessionDisplayOn)
	times.sessionBlind = early.blindTime(session)
	if !times.blankDesktopDue(session.Add(blankDesktopWait), blankDesktopLooks, early.blindTime(session.Add(blankDesktopWait))) {
		t.Fatal("dark time before the session delayed the question")
	}
}
