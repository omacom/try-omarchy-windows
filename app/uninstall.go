package main

import (
	"crypto/sha256"
	"encoding/hex"
	"strings"
)

// Apps & features registration. One key per installation, so a default
// install and a copy on another drive can be removed independently.
const uninstallRegistryParent = `Software\Microsoft\Windows\CurrentVersion\Uninstall`

func uninstallKeyName(dir, defaultDir string) string {
	if pathsEqual(dir, defaultDir) {
		return "TryOmarchy"
	}
	sum := sha256.Sum256([]byte(strings.ToLower(strings.TrimRight(dir, `\/`))))
	return "TryOmarchy-" + hex.EncodeToString(sum[:4])
}

func uninstallDisplayName(dir, defaultDir string) string {
	if pathsEqual(dir, defaultDir) {
		return "Try Omarchy"
	}
	return "Try Omarchy (" + dir + ")"
}

// uninstallCommand is the string Windows runs from Apps & features. Paths
// cannot contain double quotes, so quoting each one is enough.
func uninstallCommand(target, dir string) string {
	return `"` + target + `" -dir "` + dir + `" -uninstall`
}

func displayVersion(version string) string {
	return strings.TrimPrefix(version, "v")
}
