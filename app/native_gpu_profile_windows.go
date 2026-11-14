//go:build windows

package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"unicode/utf16"
	"unicode/utf8"
)

// This prepared profile is not selected by released settings. A matching
// normal guest graphics runtime must be installed and verified before use.
type nativeGPULauncherProfile struct {
	bridge, auditDirectory string
}

func (profile nativeGPULauncherProfile) prepare(original *exec.Cmd) (*exec.Cmd, error) {
	if original == nil || !filepath.IsAbs(original.Path) || !filepath.IsAbs(profile.bridge) || !filepath.IsAbs(profile.auditDirectory) || len(original.Args) < 2 {
		return nil, errors.New("native launcher profile requires absolute runtime paths and original arguments")
	}
	for _, value := range append([]string{original.Path, profile.bridge, profile.auditDirectory}, original.Args[1:]...) {
		if !utf8.ValidString(value) || strings.ContainsAny(value, "\x00\r\n") {
			return nil, errors.New("invalid native launcher profile string")
		}
	}
	encoded, err := json.Marshal(struct {
		Protocol       uint32   `json:"protocol"`
		QEMU           string   `json:"qemu"`
		Arguments      []string `json:"arguments"`
		AuditDirectory string   `json:"auditDirectory"`
	}{1, original.Path, original.Args[1:], profile.auditDirectory})
	if err != nil {
		return nil, err
	}
	// The native program's legacy char argv remains ASCII. Preserve every
	// original Unicode string with JSON surrogate escapes, then decode to
	// UTF-16 only inside its CreateProcessW argument path.
	var ascii strings.Builder
	for _, value := range string(encoded) {
		if value < 128 {
			ascii.WriteByte(byte(value))
			continue
		}
		for _, word := range utf16.Encode([]rune{value}) {
			fmt.Fprintf(&ascii, "\\u%04x", word)
		}
	}
	if ascii.Len() > 20000 {
		return nil, errors.New("native launcher profile exceeds its command bound")
	}
	arguments := []string{"--run-qemu", "launcher-owned", "launcher-owned", "launcher-owned", "launcher-owned", "launcher-owned",
		"--driver-launcher-profile", ascii.String(),
		"--driver-contexts", "--driver-queries", "--driver-allocations", "--driver-gpuva", "--driver-residency", "--driver-cpu",
		"--driver-cpu-slots", "1024", "--driver-fence-slots", "512", "--driver-native-metadata", "--driver-shared-resources",
		"--driver-consume-shared", "--driver-present-shared", "--driver-translation", "--driver-hwqueues", "--driver-syncs",
		"--driver-reservation", "--driver-gpu-state", "--driver-submit", "--driver-retirement", "--driver-async-submit",
		"--driver-coordinator", "--driver-sdl-relay", "--driver-actor-slots", "8", "--driver-resource-reference-slots", "32",
		"--driver-deferred-residency", "--driver-mapped-residency-admission", "--driver-application-memory", "--driver-application-objects"}
	// Include the capability that startNativeGPUOwner appends. Windows quoting
	// can expand much further than the JSON payload's own encoded length.
	complete := append([]string{profile.bridge}, arguments...)
	complete = append(complete, "--driver-launcher-cancel-handle", "18446744073709551613")
	characters := len(complete) - 1
	for _, value := range complete {
		characters += len(utf16.Encode([]rune(syscall.EscapeArg(value))))
	}
	if characters >= 32767 {
		return nil, errors.New("native launcher command exceeds the Windows command-line bound")
	}
	return exec.Command(profile.bridge, arguments...), nil
}
