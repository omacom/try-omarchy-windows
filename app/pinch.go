package main

import (
	"slices"
	"strings"
)

const pinchDevice = "virtio-pinch-pci"

// guestAcceptsPinch reports whether the guest image declares the dedicated
// touchpad. Those images keep libinput away from it until every desktop
// user's Hyprland config turns off tap-to-click for it, so older images and
// unmigrated disks never turn synthetic contacts into clicks.
func guestAcceptsPinch(spec buildSpec) bool {
	return slices.Contains(spec.Runtime.OptionalDevices, pinchDevice)
}

// Pinch is on by default for guests that accept the device. The experimental
// flag still forces it for hand-configured test guests, and -disable-pinch
// keeps ordinary Windows two-finger input.
func pinchEnabled(cfg *config) bool {
	requested := cfg.experimentalPinch || cfg.guestPinch
	return requested && !cfg.disablePinch && cfg.useGpu && cfg.displays <= 1 &&
		runtimeHasPatch(cfg.qemu, "patches/qemu/0014-forward-windows-pinch.patch")
}

func pinchEnvironment(env []string, enabled bool) []string {
	result := make([]string, 0, len(env)+1)
	for _, value := range env {
		key, _, _ := strings.Cut(value, "=")
		if !strings.EqualFold(key, "OMARCHY_EXPERIMENTAL_PINCH") {
			result = append(result, value)
		}
	}
	if enabled {
		result = append(result, "OMARCHY_EXPERIMENTAL_PINCH=1")
	}
	return result
}
