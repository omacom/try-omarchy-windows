package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const (
	provisionModeFilename = "provision-mode"
	provisionModePersonal = "personal"
	provisionModeInstant  = "instant"
	trialUsername         = "omarchy"
	trialPassword         = "omarchy"
)

func provisionAccountHint(instant bool) string {
	if instant {
		return "Trial account: " + trialUsername + "    Password: " + trialPassword
	}
	return "Use the username and password you choose inside Omarchy"
}

func readProvisionMode(dir string) (string, bool) {
	data, err := os.ReadFile(filepath.Join(dir, provisionModeFilename))
	if err != nil {
		return "", false
	}
	mode := strings.TrimSpace(string(data))
	if mode != provisionModePersonal && mode != provisionModeInstant {
		return "", false
	}
	return mode, true
}

func writeProvisionMode(dir, mode string) error {
	if mode != provisionModePersonal && mode != provisionModeInstant {
		return fmt.Errorf("unknown provisioning mode %q", mode)
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	path := filepath.Join(dir, provisionModeFilename)
	staged := path + ".part"
	if err := os.WriteFile(staged, []byte(mode+"\n"), 0o644); err != nil {
		return err
	}
	if err := os.Rename(staged, path); err != nil {
		os.Remove(staged)
		return err
	}
	return nil
}
