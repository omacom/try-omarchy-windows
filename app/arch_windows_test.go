//go:build windows

package main

import (
	"runtime"
	"strings"
	"testing"
)

// The machine the test runs on decides which branch applies: CI and real
// user hardware are x64-on-x64 (must run), ARM64 Windows gets the stop
// message. Either way, when a message is produced it must name the hardware
// that works and where to ask for an ARM64 build.
func TestHostArchUnsupportedReason(t *testing.T) {
	reason := hostArchUnsupportedReason()
	supported := runtime.GOARCH == "amd64" && nativeMachine() == imageFileMachineAmd64
	if supported && reason != "" {
		t.Fatalf("x64 launcher on x64 Windows should be supported, got: %q", reason)
	}
	if !supported && reason == "" && nativeMachine() != 0 {
		t.Fatal("non-x64 machine got no explanation")
	}
	if reason == "" {
		return
	}
	for _, want := range []string{"Intel", "AMD", "https://github.com/tsouth89/try-omarchy-windows"} {
		if !strings.Contains(reason, want) {
			t.Errorf("message should mention %q, got: %q", want, reason)
		}
	}
}
