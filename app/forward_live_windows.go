//go:build windows

package main

import (
	"context"
	"os"
	"time"
)

// runLiveForwardWatcher applies local port-forward changes saved in Settings
// to the running QEMU. Like the key forwarder, it waits for the supervisor's
// QMP handshake before dialing, since an earlier QMP connection can wedge
// WHPX, and starts over from the launch forwards whenever QEMU restarts for an
// in-guest reboot.
func runLiveForwardWatcher(dir string, launched []portForward) {
	path := settingsPath(dir)
	var active []portForward
	var pid uint32
	var applied time.Time
	for {
		time.Sleep(2 * time.Second)
		current := qemuPid.Load()
		if current == 0 || !guestUp.Load() {
			continue
		}
		if current != pid {
			pid, active, applied = current, append([]portForward(nil), launched...), time.Time{}
		}
		info, err := os.Stat(path)
		if err != nil || !info.ModTime().After(applied) {
			continue
		}
		applied = info.ModTime()
		saved, err := loadSettings(path)
		if err != nil {
			continue
		}
		var desired forwardList
		valid := true
		for _, line := range saved.Forwards {
			if desired.Set(line) != nil {
				valid = false
				break
			}
		}
		if !valid {
			continue
		}
		plan := planLiveForwards(active, desired)
		for _, f := range plan.deferred {
			logf("forwards: %s changes at the next launch", f)
		}
		if len(plan.add)+len(plan.remove) == 0 {
			continue
		}
		var errs []error
		active, errs = applyLiveForwards(active, plan, monitorCommand)
		for _, err := range errs {
			logf("forwards: %v", err)
		}
		logf("forwards: applied %d added, %d removed while running", len(plan.add), len(plan.remove))
	}
}

func monitorCommand(line string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	client, err := dialQMPControl(ctx, qmpToolsPort)
	if err != nil {
		return "", err
	}
	defer client.Close()
	var reply string
	err = client.Call(ctx, "human-monitor-command", map[string]any{"command-line": line}, &reply)
	return reply, err
}
