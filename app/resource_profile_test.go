package main

import (
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestResourceProfilesRespectLoadAndOverrides(t *testing.T) {
	host := hostResources{LogicalCPUs: 32, TotalMiB: 65536, AvailableMiB: 51200, CPUBusy: 0.25, CPUKnown: true}
	for _, tc := range []struct {
		name, profile               string
		cpus, memory                int
		explicitCPU, explicitMemory bool
		want                        guestResources
	}{
		{"legacy automatic", "", 0, 0, false, false, guestResources{8, 6144}},
		{"legacy manual preserved", "", 12, 16384, false, false, guestResources{12, 16384}},
		{"balanced ignores remembered manual", resourceBalanced, 12, 16384, false, false, guestResources{8, 6144}},
		{"maximum leaves measured load plus headroom", resourceMaximum, 12, 16384, false, false, guestResources{20, 43008}},
		{"manual", resourceManual, 16, 24576, false, false, guestResources{16, 24576}},
		{"manual zero retains automatic per field", resourceManual, 12, 0, false, false, guestResources{12, 6144}},
		{"explicit CPU wins over maximum", resourceMaximum, 10, 16384, true, false, guestResources{10, 43008}},
		{"explicit memory wins over maximum", resourceMaximum, 12, 8192, false, true, guestResources{20, 8192}},
		{"explicit zero uses preset", resourceMaximum, 0, 0, true, true, guestResources{20, 43008}},
		{"explicit values win over balanced", resourceBalanced, 14, 10240, true, true, guestResources{14, 10240}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := planGuestResources(tc.profile, host, true, tc.cpus, tc.memory, tc.explicitCPU, tc.explicitMemory)
			if err != nil || got != tc.want {
				t.Fatalf("got %+v, %v; want %+v", got, err, tc.want)
			}
		})
	}
}

func TestMaximumResourcesHandlePressureAndMissingMeasurements(t *testing.T) {
	for _, tc := range []struct {
		name      string
		host      hostResources
		want      guestResources
		wantError bool
	}{
		{"busy CPU", hostResources{32, 65536, 51200, 1, true}, guestResources{1, 43008}, false},
		{"small host", hostResources{4, 8192, 7168, 0, true}, guestResources{2, 3072}, false},
		{"one CPU", hostResources{1, 8192, 7168, 0, true}, guestResources{1, 3072}, false},
		{"low memory", hostResources{32, 65536, 9000, 0, true}, guestResources{}, true},
		{"no free memory", hostResources{32, 65536, 0, 0, true}, guestResources{}, true},
		{"capped large host", hostResources{128, 262144, 240000, 0, false}, guestResources{8, 65536}, false},
		{"unknown measurements", hostResources{32, 0, 0, 0, false}, guestResources{8, 6144}, false},
		{"bad CPU fraction", hostResources{32, 65536, 51200, math.NaN(), true}, guestResources{8, 43008}, false},
		{"CPU query failure", hostResources{32, 65536, 51200, 0, false}, guestResources{8, 43008}, false},
		{"RAM rounded down", hostResources{8, 16384, 10001, 0, true}, guestResources{6, 5888}, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			got, err := planGuestResources(resourceMaximum, tc.host, true, 0, 0, false, false)
			if (err != nil) != tc.wantError || (!tc.wantError && got != tc.want) {
				t.Fatalf("got %+v, %v; want %+v error=%v", got, err, tc.want, tc.wantError)
			}
		})
	}
}

func TestResourcePlanRejectsImpossibleManualAllocations(t *testing.T) {
	host := hostResources{LogicalCPUs: 8, TotalMiB: 16384, AvailableMiB: 12288}
	for _, tc := range []struct{ cpus, memory int }{{9, 4096}, {-1, 4096}, {4, 16384}, {4, 512}, {4, 65537}} {
		if _, err := planGuestResources(resourceManual, host, true, tc.cpus, tc.memory, false, false); err == nil {
			t.Errorf("accepted %+v", tc)
		}
	}
	if _, err := planGuestResources("fastest", host, true, 0, 0, false, false); err == nil {
		t.Fatal("accepted unknown profile")
	}
}

func TestCPUUsageCounterDeltas(t *testing.T) {
	busy, ok := cpuBusyFraction(100, 200, 300, 150, 275, 325)
	if !ok || busy != 0.5 {
		t.Fatalf("got %v %t", busy, ok)
	}
	for _, counters := range [][6]uint64{{0, 0, 0, 0, 0, 0}, {100, 200, 300, 99, 250, 350}, {0, 200, 300, 10, 199, 350}, {0, 200, 300, 10, 250, 299}, {0, 0, 0, 100, 50, 0}} {
		if _, ok := cpuBusyFraction(counters[0], counters[1], counters[2], counters[3], counters[4], counters[5]); ok {
			t.Errorf("accepted invalid counters %v", counters)
		}
	}
}

func TestResourcePreferencesRoundTripAndRollback(t *testing.T) {
	dir := t.TempDir()
	if p, err := loadResourcePreferences(dir); err != nil || p.Profile != "" {
		t.Fatalf("missing preferences: %+v %v", p, err)
	}
	if err := saveSettings(settingsPath(dir), settings{CPUs: 4, MemoryMiB: 4096}); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(settingsPath(dir))
	for _, profile := range []string{resourceMaximum, resourceBalanced, resourceManual} {
		if err := saveResourcePreferences(dir, profile); err != nil {
			t.Fatal(err)
		}
		p, err := loadResourcePreferences(dir)
		if err != nil || p.Profile != profile {
			t.Fatalf("got %+v %v", p, err)
		}
	}
	after, _ := os.ReadFile(settingsPath(dir))
	if string(before) != string(after) {
		t.Fatal("profile changed settings used by older launchers")
	}
	if _, err := loadSettings(settingsPath(dir)); err != nil {
		t.Fatal(err)
	}
	if !backupNameAllowed(resourcePreferencesFilename) {
		t.Fatal("profile excluded from backup")
	}
	staged, _ := filepath.Glob(filepath.Join(dir, ".resources-*"))
	if len(staged) != 0 {
		t.Fatal("staging file left behind")
	}
}

func TestResourcePreferencesRejectDamage(t *testing.T) {
	dir := t.TempDir()
	for _, data := range []string{`{}`, `null`, `{"schemaVersion":2}`, `{"schemaVersion":1,"profile":"fastest"}`, `{"schemaVersion":1,"unknown":true}`, `{"schemaVersion":1} {}`, strings.Repeat(" ", 4097)} {
		if err := os.WriteFile(filepath.Join(dir, resourcePreferencesFilename), []byte(data), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := loadResourcePreferences(dir); err == nil {
			t.Fatalf("accepted %q", data)
		}
	}
}

func TestResourcePlanReachesQEMU(t *testing.T) {
	host := hostResources{32, 65536, 51200, 0.25, true}
	for _, gpu := range []bool{true, false} {
		plan, err := planGuestResources(resourceMaximum, host, gpu, 0, 0, false, false)
		if err != nil {
			t.Fatal(err)
		}
		args := buildQemuArgs(&config{cpus: plan.CPUs, memMiB: plan.MemoryMiB, useGpu: gpu}, "")
		joined := strings.Join(args, " ")
		if !strings.Contains(joined, "-smp 20") || !strings.Contains(joined, "-m 43008M") {
			t.Fatal(joined)
		}
	}
}
