package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// Rendering modes chosen by the user through settings.json or -render.
const (
	renderAuto = "auto"
	renderGPU  = "gpu"
	renderCPU  = "cpu"
)

// renderProbeFilename remembers how the last launch ended up rendering, so a
// machine whose graphics driver cannot run the GPU path stops paying for two
// failed GPU attempts and a runtime rollback on every launch.
const renderProbeFilename = "render-probe.json"

// renderProbeRetryAfter bounds how long a remembered CPU result is trusted.
// Drivers and remote sessions change without touching the two identities
// below, so the GPU path gets a fresh chance periodically.
const renderProbeRetryAfter = 7 * 24 * time.Hour

type renderProbe struct {
	Schema int `json:"schema"`
	// Result is renderGPU or renderCPU: how the guest last reached userspace.
	Result string `json:"result"`
	// RuntimeID identifies the QEMU runtime the result was observed with.
	RuntimeID string `json:"runtimeID"`
	// DisplayDriver identifies the Windows display drivers at the time.
	DisplayDriver string    `json:"displayDriver"`
	RecordedAt    time.Time `json:"recordedAt"`
}

func loadRenderProbe(dir string) (*renderProbe, error) {
	data, err := os.ReadFile(filepath.Join(dir, renderProbeFilename))
	if errors.Is(err, os.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	if len(data) > maxSettingsBytes {
		return nil, fmt.Errorf("%s is too large", renderProbeFilename)
	}
	var p renderProbe
	if err := json.Unmarshal(data, &p); err != nil {
		return nil, err
	}
	if p.Schema != 1 || (p.Result != renderGPU && p.Result != renderCPU) {
		return nil, fmt.Errorf("%s is not a supported render probe record", renderProbeFilename)
	}
	return &p, nil
}

func saveRenderProbe(dir string, p renderProbe) error {
	p.Schema = 1
	data, err := json.MarshalIndent(p, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(dir, renderProbeFilename)
	tmp := path + ".part"
	if err := os.WriteFile(tmp, append(data, '\n'), 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// current reports whether the record describes this runtime and driver set
// and is recent enough to act on.
func (p *renderProbe) current(runtimeID, displayDriver string, now time.Time) bool {
	if p == nil || p.RuntimeID == "" || p.RuntimeID != runtimeID || p.DisplayDriver != displayDriver {
		return false
	}
	return !p.RecordedAt.IsZero() && now.Sub(p.RecordedAt) < renderProbeRetryAfter && !p.RecordedAt.After(now.Add(time.Hour))
}

// startWithGPU decides the first launch attempt's rendering path. GPU stays
// the default; only a remembered CPU result for the same runtime and driver
// set skips it, and an explicit -render gpu always retries.
func startWithGPU(mode string, probe *renderProbe, runtimeID, displayDriver string, now time.Time) (bool, string) {
	switch mode {
	case renderCPU:
		return false, "CPU rendering chosen in settings"
	case renderGPU:
		return true, "GPU rendering chosen in settings"
	}
	if probe != nil && probe.Result == renderCPU && probe.current(runtimeID, displayDriver, now) {
		return false, fmt.Sprintf("GPU rendering failed with this runtime and driver on %s; using CPU rendering (choose GPU in Settings to retry now)",
			probe.RecordedAt.Local().Format("2006-01-02"))
	}
	return true, ""
}

// keepUpdatedRuntimeOnCPU decides what a GPU startup failure means while a
// runtime update is still pending. A machine that already reached the desktop
// on CPU rendering with the previous runtime has a graphics stack that never
// ran GPU mode, so the failure says nothing about the new runtime: keep it and
// fall back to CPU. Any other history rolls the runtime back, since a working
// GPU path may have been broken by the update.
func keepUpdatedRuntimeOnCPU(probe *renderProbe) bool {
	return probe != nil && probe.Result == renderCPU
}

func parseRenderMode(value string) (string, error) {
	switch v := strings.ToLower(strings.TrimSpace(value)); v {
	case "", renderAuto:
		return renderAuto, nil
	case renderGPU, renderCPU:
		return v, nil
	}
	return "", fmt.Errorf("render must be auto, gpu, or cpu")
}

// runtimeIdentity names the QEMU runtime for the probe record: the recorded
// executable hash for a bundled runtime, or the executable's size and
// modification time for a user-managed install without a receipt.
func runtimeIdentity(gpuRoot string) string {
	if data, err := os.ReadFile(filepath.Join(gpuRoot, runtimeReceiptFilename)); err == nil && len(data) <= maxInstallReceiptBytes {
		var receipt runtimeReceipt
		if json.Unmarshal(data, &receipt) == nil && validSHA256(receipt.Executable.SHA256) {
			return "sha256:" + receipt.Executable.SHA256
		}
	}
	info, err := os.Stat(filepath.Join(gpuRoot, "bin", "qemu-system-x86_64w.exe"))
	if err != nil {
		return ""
	}
	return fmt.Sprintf("stat:%d:%d", info.Size(), info.ModTime().UnixNano())
}
