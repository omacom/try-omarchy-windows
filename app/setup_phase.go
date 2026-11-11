package main

import (
	"errors"
	"time"
)

// Long setup steps log when they start and how long they took, so a slow
// machine's shell.log shows where the time went between the downloads and
// the first boot.
var (
	setupPhaseNow = time.Now
	setupPhaseLog = func(format string, a ...any) { logf(format, a...) }
)

type setupPhase struct {
	name  string
	start time.Time
}

func startSetupPhase(name string) setupPhase {
	setupPhaseLog("setup: %s started", name)
	return setupPhase{name: name, start: setupPhaseNow()}
}

// end logs the step's duration. Failures are reported by the caller; this
// only says that the step stopped.
func (p setupPhase) end(err error) {
	elapsed := setupPhaseNow().Sub(p.start).Round(time.Millisecond)
	switch {
	case err == nil:
		setupPhaseLog("setup: %s finished after %s", p.name, elapsed)
	case errors.Is(err, errSetupCancelled):
		setupPhaseLog("setup: %s cancelled after %s", p.name, elapsed)
	default:
		setupPhaseLog("setup: %s stopped after %s", p.name, elapsed)
	}
}
