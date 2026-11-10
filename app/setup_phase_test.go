package main

import (
	"errors"
	"fmt"
	"slices"
	"testing"
	"time"
)

func TestSetupPhaseLogsStartAndDuration(t *testing.T) {
	now := time.Date(2026, 11, 9, 18, 33, 44, 0, time.UTC)
	var lines []string
	oldNow, oldLog := setupPhaseNow, setupPhaseLog
	t.Cleanup(func() { setupPhaseNow, setupPhaseLog = oldNow, oldLog })
	setupPhaseNow = func() time.Time { return now }
	setupPhaseLog = func(format string, a ...any) { lines = append(lines, fmt.Sprintf(format, a...)) }

	phase := startSetupPhase("writable disk copy")
	now = now.Add(15*time.Minute + 39*time.Second + 1234*time.Microsecond)
	phase.end(nil)
	phase = startSetupPhase("system image unpack")
	now = now.Add(2 * time.Second)
	phase.end(fmt.Errorf("unpacking: %w", errSetupCancelled))
	phase = startSetupPhase("system image download")
	now = now.Add(time.Second)
	phase.end(errors.New("connection reset"))

	want := []string{
		"setup: writable disk copy started",
		"setup: writable disk copy finished after 15m39.001s",
		"setup: system image unpack started",
		"setup: system image unpack cancelled after 2s",
		"setup: system image download started",
		"setup: system image download stopped after 1s",
	}
	if !slices.Equal(lines, want) {
		t.Fatalf("log lines:\n%q\nwant:\n%q", lines, want)
	}
}
