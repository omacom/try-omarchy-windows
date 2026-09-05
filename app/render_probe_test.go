package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRenderProbeRoundTrip(t *testing.T) {
	dir := t.TempDir()
	if p, err := loadRenderProbe(dir); err != nil || p != nil {
		t.Fatalf("missing probe should be nil, got %v %v", p, err)
	}
	now := time.Date(2026, 9, 5, 12, 0, 0, 0, time.UTC)
	want := renderProbe{Result: renderCPU, RuntimeID: "sha256:abc", DisplayDriver: "Intel=31.0.1", RecordedAt: now}
	if err := saveRenderProbe(dir, want); err != nil {
		t.Fatal(err)
	}
	got, err := loadRenderProbe(dir)
	if err != nil || got == nil || got.Schema != 1 || got.Result != renderCPU || got.RuntimeID != want.RuntimeID || !got.RecordedAt.Equal(now) {
		t.Fatalf("round trip mismatch: %+v %v", got, err)
	}
	if err := os.WriteFile(filepath.Join(dir, renderProbeFilename), []byte(`{"schema":1,"result":"maybe"}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := loadRenderProbe(dir); err == nil {
		t.Fatal("unknown result must be rejected")
	}
}

func TestStartWithGPUSkipsOnlyAMatchingRecentCPUResult(t *testing.T) {
	now := time.Date(2026, 9, 5, 12, 0, 0, 0, time.UTC)
	cpu := &renderProbe{Result: renderCPU, RuntimeID: "rt1", DisplayDriver: "drv1", RecordedAt: now.Add(-time.Hour)}
	cases := []struct {
		name          string
		mode          string
		probe         *renderProbe
		runtime, drv  string
		wantGPU       bool
		reasonContain string
	}{
		{"no history", renderAuto, nil, "rt1", "drv1", true, ""},
		{"matching cpu result", renderAuto, cpu, "rt1", "drv1", false, "using CPU rendering"},
		{"runtime changed", renderAuto, cpu, "rt2", "drv1", true, ""},
		{"driver changed", renderAuto, cpu, "rt1", "drv2", true, ""},
		{"gpu result", renderAuto, &renderProbe{Result: renderGPU, RuntimeID: "rt1", DisplayDriver: "drv1", RecordedAt: now}, "rt1", "drv1", true, ""},
		{"stale cpu result", renderAuto, &renderProbe{Result: renderCPU, RuntimeID: "rt1", DisplayDriver: "drv1", RecordedAt: now.Add(-25 * time.Hour)}, "rt1", "drv1", true, ""},
		{"future timestamp", renderAuto, &renderProbe{Result: renderCPU, RuntimeID: "rt1", DisplayDriver: "drv1", RecordedAt: now.Add(48 * time.Hour)}, "rt1", "drv1", true, ""},
		{"empty runtime id never matches", renderAuto, &renderProbe{Result: renderCPU, RecordedAt: now}, "", "", true, ""},
		{"forced gpu", renderGPU, cpu, "rt1", "drv1", true, "GPU rendering chosen"},
		{"forced cpu", renderCPU, nil, "rt1", "drv1", false, "CPU rendering chosen"},
	}
	for _, c := range cases {
		gpu, reason := startWithGPU(c.mode, c.probe, c.runtime, c.drv, now)
		if gpu != c.wantGPU || !strings.Contains(reason, c.reasonContain) {
			t.Errorf("%s: got gpu=%v reason=%q, want gpu=%v containing %q", c.name, gpu, reason, c.wantGPU, c.reasonContain)
		}
	}
}

func TestKeepUpdatedRuntimeOnlyAfterACPUResult(t *testing.T) {
	if keepUpdatedRuntimeOnCPU(nil) {
		t.Fatal("no history must roll the runtime back")
	}
	if keepUpdatedRuntimeOnCPU(&renderProbe{Result: renderGPU}) {
		t.Fatal("a working GPU path must roll the runtime back")
	}
	if !keepUpdatedRuntimeOnCPU(&renderProbe{Result: renderCPU, RuntimeID: "old"}) {
		t.Fatal("a CPU-only machine must keep the updated runtime")
	}
}

func TestParseRenderMode(t *testing.T) {
	for input, want := range map[string]string{"": renderAuto, " Auto ": renderAuto, "GPU": renderGPU, "cpu": renderCPU} {
		if got, err := parseRenderMode(input); err != nil || got != want {
			t.Errorf("%q: got %q %v, want %q", input, got, err, want)
		}
	}
	if _, err := parseRenderMode("software"); err == nil {
		t.Fatal("unknown mode must be rejected")
	}
}

func TestRuntimeIdentityPrefersTheReceiptHash(t *testing.T) {
	root := t.TempDir()
	if runtimeIdentity(root) != "" {
		t.Fatal("missing runtime must have no identity")
	}
	bin := filepath.Join(root, "bin")
	if err := os.MkdirAll(bin, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(bin, "qemu-system-x86_64w.exe"), []byte("qemu"), 0o644); err != nil {
		t.Fatal(err)
	}
	if id := runtimeIdentity(root); !strings.HasPrefix(id, "stat:4:") {
		t.Fatalf("receipt-less runtime should use size and mtime, got %q", id)
	}
	sum := strings.Repeat("ab", 32)
	receipt := `{"schema":1,"release":"r","manifestSHA256":"` + sum + `","archiveSHA256":"` + sum + `","executable":{"sha256":"` + sum + `","size":4,"modTimeUnixNano":1}}`
	if err := os.WriteFile(filepath.Join(root, runtimeReceiptFilename), []byte(receipt), 0o644); err != nil {
		t.Fatal(err)
	}
	if id := runtimeIdentity(root); id != "sha256:"+sum {
		t.Fatalf("receipt hash expected, got %q", id)
	}
}
