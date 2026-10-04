package main

import (
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const gpuLaunchFilename = "gpu-launch.json"

// Store launch-time facts for the separate diagnostics process, whose config
// has not supervised QEMU. Adapter preference order is not device selection.
func saveGPULaunchFacts(dir string, facts map[string]string) error {
	data, err := json.MarshalIndent(facts, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(dir, gpuLaunchFilename)
	if err := os.WriteFile(path+".part", append(data, '\n'), 0o644); err != nil {
		return err
	}
	if err := os.Rename(path+".part", path); err != nil {
		os.Remove(path + ".part")
		return err
	}
	return nil
}

func gpuLaunchFacts(dir string) map[string]string {
	data, err := os.ReadFile(filepath.Join(dir, gpuLaunchFilename))
	var facts map[string]string
	if err == nil && len(data) <= maxSettingsBytes {
		_ = json.Unmarshal(data, &facts)
	}
	return facts
}

func gpuEnvironmentFacts(env []string) string {
	var out []string
	for _, entry := range env {
		key, _, _ := strings.Cut(entry, "=")
		switch strings.ToUpper(key) {
		case "DRI_PRIME", "MESA_VK_DEVICE_SELECT", "MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "VK_ICD_FILENAMES", "VK_DRIVER_FILES", "VK_LOADER_DRIVERS_SELECT", "VK_LOADER_DRIVERS_DISABLE", "GALLIUM_DRIVER", "SDL_VIDEODRIVER":
			out = append(out, entry)
		}
	}
	if len(out) == 0 {
		return "no renderer selectors set"
	}
	return strings.Join(out, "; ")
}

// Preserve only labelled runtime reports, never infer the selected GPU from
// DXGI order, an installed adapter or an environment selector. stderr itself
// remains in the bundle, including messages whose format we do not recognize.
func gpuRuntimeReports(vmDir string) string {
	f, err := os.Open(filepath.Join(vmDir, "qemu-stderr.log"))
	if err != nil {
		return "unreported (stderr unavailable)"
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return "unreported (stderr unavailable)"
	}
	if info.Size() > diagnosticTailBytes {
		if _, err := f.Seek(info.Size()-diagnosticTailBytes, io.SeekStart); err != nil {
			return "unreported (stderr unavailable)"
		}
	}
	data, err := io.ReadAll(io.LimitReader(f, diagnosticTailBytes))
	if err != nil {
		return "unreported (stderr unavailable)"
	}
	return rendererReports(string(data))
}

func rendererReports(stderr string) string {
	if len(stderr) > diagnosticTailBytes {
		stderr = stderr[len(stderr)-diagnosticTailBytes:]
	}
	var out []string
	for _, line := range strings.Split(stderr, "\n") {
		lower := strings.ToLower(line)
		if strings.Contains(lower, "gl_renderer") || strings.Contains(lower, "gl_vendor") || strings.Contains(lower, "gl renderer") || strings.Contains(lower, "gl vendor") || strings.Contains(lower, "vulkan device") || strings.Contains(lower, "selected physical device") || (strings.Contains(lower, "vulkan") && strings.Contains(lower, "devicename")) {
			out = append(out, strings.TrimSpace(line))
			if len(out) == 16 {
				break
			}
		}
	}
	if len(out) == 0 {
		return "unreported by runtime; adapter order and environment are not proof of selection"
	}
	return strings.Join(out, " | ")
}

func logGPULaunchFacts(cfg *config, env []string, adapters map[string]string, preference string) {
	facts := adapters
	facts["gpu.launch.runtimeReport"] = "unreported at launch; see qemu-stderr.log"
	facts["gpu.launch.time"] = time.Now().Format(time.RFC3339)
	facts["gpu.launch.runtime"] = cfg.runtimeID
	facts["gpu.launch.drivers"] = cfg.displayDriver
	facts["gpu.launch.executable"] = cfg.qemu
	facts["gpu.launch.userPreference"] = preference
	facts["gpu.launch.environment"] = gpuEnvironmentFacts(env)
	for _, key := range []string{"gpu.launch.time", "gpu.launch.runtime", "gpu.launch.drivers", "gpu.launch.executable", "gpu.launch.userPreference", "gpu.launch.environment", "gpu.adapters", "gpu.order.highPerformance", "gpu.order.minimumPower"} {
		logf("%s: %s", key, facts[key])
	}
	if err := saveGPULaunchFacts(cfg.dir, facts); err != nil {
		logf("could not save GPU launch facts: %v", err)
	}
}

func recordGPURuntimeReport(dir, vmDir string) {
	report := gpuRuntimeReports(vmDir)
	logf("GPU runtime renderer report: %s", report)
	facts := gpuLaunchFacts(dir)
	if facts == nil {
		return
	}
	facts["gpu.launch.runtimeReport"] = report
	if err := saveGPULaunchFacts(dir, facts); err != nil {
		logf("could not save GPU renderer report: %v", err)
	}
}
