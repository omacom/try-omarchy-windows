//go:build windows

package main

import (
	"context"
	"time"
)

// Pause the guest while Windows sleeps and resume afterwards. A suspended host
// freezes the QEMU process mid-instruction; letting the guest run again from a
// known paused state avoids the clock jump and wedged timers a suspend leaves
// behind. The tray window receives the power broadcasts and also re-syncs the
// guest clock on resume.

const pbtApmSuspend = 0x0004

func pauseGuest(reason string) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	client, err := dialQMPControl(ctx, qmpToolsPort)
	if err != nil {
		return
	}
	defer client.Close()
	if err := client.Call(ctx, "stop", nil, nil); err != nil {
		logf("power: pausing for %s failed: %v", reason, err)
		return
	}
	logf("power: paused the guest for %s", reason)
}

func resumeGuest() {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	client, err := dialQMPControl(ctx, qmpToolsPort)
	if err != nil {
		return
	}
	defer client.Close()
	if err := client.Call(ctx, "cont", nil, nil); err != nil {
		logf("power: resuming after sleep failed: %v", err)
		return
	}
	logf("power: resumed the guest after host sleep")
}
