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
// WHPX. After QEMU restarts for an in-guest reboot it starts from the list
// that boot was given (forwardsForBoot) and compares again.
func runLiveForwardWatcher(dir string, launched []portForward) {
	path := settingsPath(dir)
	var active []portForward
	var pid uint32
	var seenTime time.Time
	var seenSize int64 = -1
	for {
		time.Sleep(2 * time.Second)
		current := qemuPid.Load()
		if current == 0 || !guestUp.Load() {
			continue
		}
		if current != pid {
			pid, active, seenTime, seenSize = current, forwardsForBoot(launched), time.Time{}, -1
		}
		info, err := os.Stat(path)
		// Size as well as time: a coarse or restored timestamp alone can hide an edit.
		if err != nil || (info.ModTime().Equal(seenTime) && info.Size() == seenSize) {
			continue
		}
		seenTime, seenSize = info.ModTime(), info.Size()
		saved, err := loadSettings(path)
		if err != nil {
			logf("forwards: settings not readable, keeping the current forwards: %v", err)
			continue
		}
		var desired forwardList
		var invalid error
		for _, line := range saved.Forwards {
			if invalid = desired.Set(line); invalid != nil {
				break
			}
		}
		if invalid != nil {
			logf("forwards: saved list is invalid, keeping the current forwards: %v", invalid)
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
		setLiveForwards(active)
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
