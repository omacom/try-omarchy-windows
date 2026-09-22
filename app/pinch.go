package main

import "strings"

// Opt-in until real gesture, scrolling and guest configuration acceptance.
func pinchEnabled(cfg *config) bool {
	return cfg.experimentalPinch && cfg.useGpu && cfg.displays <= 1 &&
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
