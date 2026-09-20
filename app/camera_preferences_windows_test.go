//go:build windows

package main

import (
	"strings"
	"testing"
)

func TestDisabledCameraOverridesSyntheticCapture(t *testing.T) {
	t.Setenv("TRYOMARCHY_FAKE_CAMERA", "1")
	source := configuredCameraSource(desktopPreferences{CameraDisabled: true})
	defer source.stop()
	frames, err := source.start()
	if frames != nil || err == nil || !strings.Contains(err.Error(), "Camera access is off") {
		t.Fatalf("disabled camera allowed capture: %v", err)
	}
}
