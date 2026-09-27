package main

import (
	"fmt"
	"strings"
	"sync"
)

// Applying port-forward changes from Settings while Omarchy runs. QEMU's user
// network can add and remove host forwards at runtime through the monitor's
// hostfwd_add and hostfwd_remove commands. Only local forwards change live: a
// LAN forward needs its Windows firewall rule, which takes an elevation
// prompt, and a forward to guest port 22 needs sshd, which the guest starts at
// boot. Those wait for the next launch.

// liveForwardState is the forward list QEMU should have now. The watcher
// updates it after each live change, and every QEMU start, including the
// relaunch after an in-guest reboot, builds its network from it, so a forward
// removed while running stays removed.
var liveForwardState struct {
	sync.Mutex
	active []portForward
	set    bool
}

func setLiveForwards(active []portForward) {
	liveForwardState.Lock()
	liveForwardState.active = append([]portForward(nil), active...)
	liveForwardState.set = true
	liveForwardState.Unlock()
}

// forwardsForBoot returns the live list when there is one, else the forwards
// from launch.
func forwardsForBoot(launched []portForward) []portForward {
	liveForwardState.Lock()
	defer liveForwardState.Unlock()
	if !liveForwardState.set {
		return append([]portForward(nil), launched...)
	}
	return append([]portForward(nil), liveForwardState.active...)
}

// canonicalForward treats an explicit 127.0.0.1 binding like the default, so
// rewriting tcp:8080:80 as tcp:127.0.0.1:8080:80 is not a change.
func canonicalForward(f portForward) portForward {
	if f.bind == "127.0.0.1" {
		f.bind = ""
	}
	return f
}

func liveForwardEligible(f portForward) bool {
	return !f.exposedToLAN() && f.guestPort != 22
}

func hostfwdRule(f portForward) string {
	return fmt.Sprintf("%s:%s:%d", f.proto, f.address(), f.hostPort)
}

type forwardPlan struct {
	remove, add, deferred []portForward
}

// planLiveForwards compares the forwards QEMU has with the ones Settings now
// asks for. Removals come first so a changed guest port can reuse its host port.
func planLiveForwards(active, desired []portForward) forwardPlan {
	var plan forwardPlan
	has := func(list []portForward, f portForward) bool {
		for _, other := range list {
			if canonicalForward(other) == canonicalForward(f) {
				return true
			}
		}
		return false
	}
	for _, f := range active {
		if !has(desired, f) {
			if liveForwardEligible(f) {
				plan.remove = append(plan.remove, f)
			} else {
				plan.deferred = append(plan.deferred, f)
			}
		}
	}
	for _, f := range desired {
		if !has(active, f) {
			if liveForwardEligible(f) {
				plan.add = append(plan.add, f)
			} else {
				plan.deferred = append(plan.deferred, f)
			}
		}
	}
	return plan
}

// applyLiveForwards runs the plan through the monitor and returns the
// forwards QEMU has afterwards. The monitor reports failures as text, so
// hostfwd_add succeeds only with an empty reply and hostfwd_remove only when
// it says the rule was removed.
func applyLiveForwards(active []portForward, plan forwardPlan, monitor func(string) (string, error)) ([]portForward, []error) {
	var errs []error
	current := append([]portForward(nil), active...)
	for _, f := range plan.remove {
		reply, err := monitor("hostfwd_remove n0 " + hostfwdRule(f))
		if err == nil && !strings.Contains(reply, "removed") {
			err = fmt.Errorf("%s", strings.TrimSpace(reply))
		}
		if err != nil {
			errs = append(errs, fmt.Errorf("removing %s: %w", f, err))
			continue
		}
		for i, other := range current {
			if canonicalForward(other) == canonicalForward(f) {
				current = append(current[:i], current[i+1:]...)
				break
			}
		}
	}
	for _, f := range plan.add {
		reply, err := monitor(fmt.Sprintf("hostfwd_add n0 %s-:%d", hostfwdRule(f), f.guestPort))
		if err == nil && strings.TrimSpace(reply) != "" {
			err = fmt.Errorf("%s", strings.TrimSpace(reply))
		}
		if err != nil {
			errs = append(errs, fmt.Errorf("adding %s: %w", f, err))
			continue
		}
		current = append(current, f)
	}
	return current, errs
}
