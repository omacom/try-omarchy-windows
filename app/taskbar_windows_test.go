//go:build windows

package main

import (
	"runtime"
	"testing"
)

func TestTaskbarIdentityDoesNotLeakCOMApartment(t *testing.T) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	// Confirm this thread starts without an apartment, then exercise the
	// taskbar error path. A leaked STA would prevent later camera MTA setup.
	hr := procCall(procCoInitializeEx, 0, coInitMultithreaded)
	if hr < 0 {
		t.Skipf("test thread already has a different apartment: %#x", uint32(hr))
	}
	procCoUninitialize.Call()
	if hr != sOK {
		t.Skip("test thread already has a COM apartment")
	}
	setTaskbarIdentity(0)
	hr = procCall(procCoInitializeEx, 0, coInitMultithreaded)
	if hr < 0 {
		t.Fatalf("taskbar setup leaked an incompatible COM apartment: %#x", uint32(hr))
	}
	procCoUninitialize.Call()
}
