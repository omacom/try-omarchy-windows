package main

import (
	"encoding/json"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
)

const (
	resourceBalanced            = "balanced"
	resourceMaximum             = "maximum-performance"
	resourceManual              = "manual"
	resourcePreferencesFilename = "resources.json"
)

// Separate from settings.json so an older launcher can still roll back: its
// decoder rejects unknown fields. The empty profile preserves legacy overrides.
type resourcePreferences struct {
	SchemaVersion int    `json:"schemaVersion"`
	Profile       string `json:"profile"`
}

func validateResourceProfile(profile string) error {
	switch profile {
	case "", resourceBalanced, resourceMaximum, resourceManual:
		return nil
	}
	return fmt.Errorf("resource profile must be balanced, maximum-performance, or manual")
}

func loadResourcePreferences(dir string) (resourcePreferences, error) {
	p := resourcePreferences{SchemaVersion: 1}
	f, err := os.Open(filepath.Join(dir, resourcePreferencesFilename))
	if os.IsNotExist(err) {
		return p, nil
	}
	if err != nil {
		return p, err
	}
	defer f.Close()
	info, err := f.Stat()
	if err != nil {
		return p, err
	}
	if !info.Mode().IsRegular() || info.Size() > 4096 {
		return p, fmt.Errorf("invalid resource preferences file")
	}
	d := json.NewDecoder(io.LimitReader(f, 4097))
	d.DisallowUnknownFields()
	p = resourcePreferences{}
	if err = d.Decode(&p); err != nil {
		return p, err
	}
	if d.Decode(&struct{}{}) != io.EOF {
		return p, fmt.Errorf("resource preferences contain trailing data")
	}
	if p.SchemaVersion != 1 {
		return p, fmt.Errorf("unsupported resource preferences version %d", p.SchemaVersion)
	}
	return p, validateResourceProfile(p.Profile)
}

func saveResourcePreferences(dir, profile string) error {
	if err := validateResourceProfile(profile); err != nil {
		return err
	}
	data, err := json.MarshalIndent(resourcePreferences{1, profile}, "", "  ")
	if err != nil {
		return err
	}
	if err = os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	f, err := os.CreateTemp(dir, ".resources-*")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	if _, err = f.Write(append(data, '\n')); err != nil {
		f.Close()
		return err
	}
	if err = f.Sync(); err != nil {
		f.Close()
		return err
	}
	if err = f.Close(); err != nil {
		return err
	}
	return os.Rename(f.Name(), filepath.Join(dir, resourcePreferencesFilename))
}

type hostResources struct {
	LogicalCPUs, TotalMiB, AvailableMiB int
	// Fraction busy over a bounded sample, not lifetime CPU use. CPUKnown
	// distinguishes an idle host from a failed/unavailable counter.
	CPUBusy  float64
	CPUKnown bool
}

type guestResources struct{ CPUs, MemoryMiB int }

func effectiveResourceProfile(profile string, cpus, memory int) string {
	if profile != "" {
		return profile
	}
	if cpus != 0 || memory != 0 {
		return resourceManual
	}
	return resourceBalanced
}

// Presets choose boot-time capacity; they do not reserve physical cores or
// continuously resize a running VM. Explicit per-resource flags always win.
func planGuestResources(profile string, host hostResources, gpu bool, cpus, memory int, explicitCPU, explicitMemory bool) (guestResources, error) {
	if err := validateResourceProfile(profile); err != nil {
		return guestResources{}, err
	}
	profile = effectiveResourceProfile(profile, cpus, memory)
	plan := guestResources{pickGuestCPUs(host.LogicalCPUs), pickGuestMemMiB(gpu, host.TotalMiB, host.AvailableMiB)}
	if profile == resourceMaximum {
		if host.CPUKnown && !math.IsNaN(host.CPUBusy) && host.CPUBusy >= 0 && host.CPUBusy <= 1 && host.LogicalCPUs > 0 {
			// Leave at least two logical processors' worth of headroom beyond
			// the sampled host workload (one eighth on larger machines).
			reserve := max(2, (host.LogicalCPUs+7)/8)
			idle := int(math.Floor(float64(host.LogicalCPUs) * (1 - host.CPUBusy)))
			plan.CPUs = max(1, min(maximumGuestCPUs, idle-reserve))
		}
		if host.TotalMiB > 0 && host.AvailableMiB >= 0 {
			// Available RAM already excludes the current Windows workload.
			// Reserve additional space for Windows, QEMU and graphics overhead.
			reserve := max(4096, host.TotalMiB/8)
			available := min(host.TotalMiB, host.AvailableMiB) - reserve
			if available < minimumGuestMemoryMiB && !(explicitMemory && memory != 0) {
				return guestResources{}, fmt.Errorf("not enough available RAM for Maximum performance while keeping %d MiB of Windows headroom; close some Windows apps or choose Balanced", reserve)
			}
			plan.MemoryMiB = max(minimumGuestMemoryMiB, min(maximumGuestMemoryMiB, available/256*256))
		}
	}
	if cpus != 0 && (profile == resourceManual || explicitCPU) {
		plan.CPUs = cpus
	}
	if memory != 0 && (profile == resourceManual || explicitMemory) {
		plan.MemoryMiB = memory
	}
	if plan.CPUs < minimumGuestCPUs || plan.CPUs > maximumGuestCPUs || (host.LogicalCPUs > 0 && plan.CPUs > host.LogicalCPUs) {
		return guestResources{}, fmt.Errorf("guest CPUs must be between 1 and %d on this PC", max(1, min(maximumGuestCPUs, host.LogicalCPUs)))
	}
	if plan.MemoryMiB < minimumGuestMemoryMiB || plan.MemoryMiB > maximumGuestMemoryMiB {
		return guestResources{}, fmt.Errorf("guest RAM must be between 1024 and 65536 MiB")
	}
	if host.TotalMiB > 0 && plan.MemoryMiB > host.TotalMiB-hostMemReserveMiB {
		return guestResources{}, fmt.Errorf("guest RAM must leave at least %d MiB of physical memory for Windows", hostMemReserveMiB)
	}
	return plan, nil
}

// GetSystemTimes includes idle time in kernel time. Reject counter resets and
// missing samples rather than treating failed measurement as an idle machine.
func cpuBusyFraction(idleBefore, kernelBefore, userBefore, idleAfter, kernelAfter, userAfter uint64) (float64, bool) {
	if idleAfter < idleBefore || kernelAfter < kernelBefore || userAfter < userBefore {
		return 0, false
	}
	total := kernelAfter - kernelBefore + userAfter - userBefore
	idle := idleAfter - idleBefore
	if total == 0 || idle > total {
		return 0, false
	}
	return float64(total-idle) / float64(total), true
}
