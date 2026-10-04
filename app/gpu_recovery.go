package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
)

// Keep the existing five-second probe cadence, but allow nineteen missed
// replies spanning at least 90 seconds rather than nine (about 45 seconds).
// The first miss starts the interval; eighteen more must follow. Any QMP line resets
// the counter. Counting ticks rather than wall time also avoids treating a
// suspended host as a frozen guest on resume.
const qmpHangMisses = 19

type qmpSilence struct{ misses int }

func (s *qmpSilence) answered()   { s.misses = 0 }
func (s *qmpSilence) probe() bool { s.misses++; return s.misses >= qmpHangMisses }

const gpuFreezeFilename = "gpu-freezes.json"

type gpuFreezes struct {
	Schema        int    `json:"schema"`
	RuntimeID     string `json:"runtimeID"`
	DisplayDriver string `json:"displayDriver"`
	Count         int    `json:"count"`
}

func loadGPUFreezes(dir string) (gpuFreezes, error) {
	var p gpuFreezes
	data, err := os.ReadFile(filepath.Join(dir, gpuFreezeFilename))
	if errors.Is(err, os.ErrNotExist) {
		return p, nil
	}
	if err != nil {
		return p, err
	}
	if len(data) > maxSettingsBytes {
		return p, fmt.Errorf("GPU freeze record is too large")
	}
	if err := json.Unmarshal(data, &p); err != nil {
		return gpuFreezes{}, err
	}
	if p.Schema != 1 || p.Count < 0 || p.Count > 2 {
		return gpuFreezes{}, fmt.Errorf("unsupported GPU freeze record")
	}
	return p, nil
}

// Unknown runtime/driver identities cannot establish that two freezes came
// from the same stack. Successful boots do not erase post-ready freeze history.
func (p gpuFreezes) observe(mode string, gpu, ready bool, runtimeID, drivers string) (gpuFreezes, bool) {
	if !gpu || !ready || mode == renderCPU || runtimeID == "" || drivers == "" {
		return p, false
	}
	if p.RuntimeID != runtimeID || p.DisplayDriver != drivers {
		p = gpuFreezes{Schema: 1, RuntimeID: runtimeID, DisplayDriver: drivers}
	}
	p.Count = min(p.Count+1, 2)
	return p, mode == renderAuto && p.Count >= 2
}

func recordGPUFreeze(dir, mode string, gpu, ready bool, runtimeID, drivers string) (bool, error) {
	p, err := loadGPUFreezes(dir)
	if err != nil {
		return false, err
	}
	next, suggest := p.observe(mode, gpu, ready, runtimeID, drivers)
	if next == p {
		return suggest, nil
	}
	data, err := json.MarshalIndent(next, "", "  ")
	if err != nil {
		return false, err
	}
	path := filepath.Join(dir, gpuFreezeFilename)
	if err := os.WriteFile(path+".part", append(data, '\n'), 0o644); err != nil {
		return false, err
	}
	if err := os.Rename(path+".part", path); err != nil {
		os.Remove(path + ".part")
		return false, err
	}
	return suggest, nil
}

const (
	gpuRecoveryCPUOnce   = 1
	gpuRecoveryGPU       = 2
	gpuRecoveryClose     = 3
	gpuRecoveryCPUAlways = 4
)

type gpuRecoveryDecision struct{ restart, gpu, temporaryCPU, persistCPU bool }

func decideGPURecovery(action int) gpuRecoveryDecision {
	switch action {
	case gpuRecoveryCPUOnce:
		return gpuRecoveryDecision{restart: true, temporaryCPU: true}
	case gpuRecoveryGPU:
		return gpuRecoveryDecision{restart: true, gpu: true}
	case gpuRecoveryCPUAlways:
		return gpuRecoveryDecision{restart: true, persistCPU: true}
	default:
		return gpuRecoveryDecision{}
	}
}

// Only the separately labelled Always choice may write Settings. In
// particular, a one-time CPU restart must not leave an Auto CPU probe either.
func saveGPURecoveryChoice(dir string, decision gpuRecoveryDecision) error {
	if !decision.persistCPU {
		return nil
	}
	s, err := loadSettings(settingsPath(dir))
	if err != nil {
		return err
	}
	s.Render = renderCPU
	return saveSettings(settingsPath(dir), s)
}

func shouldRecordRenderResult(runtimeID, mode string, gpu, temporaryCPU bool) bool {
	return runtimeID != "" && !temporaryCPU && !(mode == renderCPU && !gpu)
}
