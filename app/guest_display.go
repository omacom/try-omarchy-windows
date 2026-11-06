package main

import (
	"sync/atomic"
	"time"
)

// guestFollowsWindow is true while the guest's desktop is up and sizes its
// display to the VM window. Before that, while the guest boots, and from the
// moment it starts shutting down (its agent disconnects), QEMU resizes the
// window to whatever the guest's display is, and the title enforcer puts the
// window back where it belongs instead of remembering those sizes.
var (
	guestFollowsWindow atomic.Bool
	bootAnnouncedReady atomic.Bool
)

func guestBootStarted() {
	bootAnnouncedReady.Store(false)
	guestFollowsWindow.Store(false)
	updateCurtainTimes(func(t *bootCurtainTimes) { *t = bootCurtainTimes{boot: time.Now()} })
	desktopSeen.Store(false)
	desktopLooks.Store(0)
	desktopShown.reset()
}

func guestDesktopReady() {
	bootAnnouncedReady.Store(true)
	guestFollowsWindow.Store(true)
	updateCurtainTimes(func(t *bootCurtainTimes) { t.ready = time.Now() })
}

// guestSessionStarted runs when the desktop session's clipboard bridge connects.
func guestSessionStarted() {
	updateCurtainTimes(func(t *bootCurtainTimes) {
		if t.session.IsZero() {
			t.session = time.Now()
		}
	})
}

func guestAgentConnected() {
	if bootAnnouncedReady.Load() {
		guestFollowsWindow.Store(true)
	}
}

func guestAgentDisconnected() { guestFollowsWindow.Store(false) }
