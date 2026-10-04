package main

import (
	"archive/zip"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestGPULaunchFactsSurviveSeparateDiagnosticsProcess(t *testing.T) {
	dir := t.TempDir()
	exe := filepath.Join(dir, "runtime", "bin", "qemu-system-x86_64w.exe")
	logGPULaunchFacts(&config{dir: dir, qemu: exe, runtimeID: "runtime", displayDriver: "driver"}, nil, map[string]string{
		"gpu.adapters":              "0: Intel vendor=0x8086 LUID=00000000:00000001; 1: NVIDIA vendor=0x10de LUID=00000000:00000002",
		"gpu.order.highPerformance": "NVIDIA; Intel", "gpu.order.minimumPower": "Intel; NVIDIA",
	}, "GpuPreference=2;")
	facts := launcherFacts(&config{dir: dir}) // no running VM in this process
	if facts["gpu.launch.runtime"] != "runtime" || facts["gpu.launch.userPreference"] != "GpuPreference=2;" || facts["gpu.order.minimumPower"] != "Intel; NVIDIA" {
		t.Fatalf("missing launch facts: %+v", facts)
	}
	if err := os.MkdirAll(filepath.Join(dir, "vm"), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "vm", "qemu-stderr.log"), []byte("GL_RENDERER: NVIDIA RTX 5090\n"), 0644); err != nil {
		t.Fatal(err)
	}
	recordGPURuntimeReport(dir, filepath.Join(dir, "vm"))
	facts = launcherFacts(&config{dir: dir})
	if !strings.Contains(facts["gpu.launch.runtimeReport"], "NVIDIA RTX 5090") {
		t.Fatalf("runtime report missing: %+v", facts)
	}
	bundle, err := writeDiagnostics(dir, facts)
	if err != nil {
		t.Fatal(err)
	}
	z, err := zip.OpenReader(bundle)
	if err != nil {
		t.Fatal(err)
	}
	defer z.Close()
	for _, file := range z.File {
		if file.Name != "facts.txt" {
			continue
		}
		r, err := file.Open()
		if err != nil {
			t.Fatal(err)
		}
		data, err := io.ReadAll(r)
		r.Close()
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(data), dir) {
			t.Fatalf("private path in facts: %s", data)
		}
		if !strings.Contains(string(data), "GpuPreference=2;") || !strings.Contains(string(data), "vendor=0x10de") {
			t.Fatalf("GPU facts missing: %s", data)
		}
		return
	}
	t.Fatal("facts.txt missing")
}

func TestGPURendererEvidenceDoesNotInferSelection(t *testing.T) {
	for _, stderr := range []string{"", "Vulkan initialized\n", "NVIDIA RTX 5090 adapter available\n"} {
		if !strings.Contains(rendererReports(stderr), "unreported") {
			t.Fatalf("inferred renderer from %q", stderr)
		}
	}
	report := rendererReports("unrelated\nGL_VENDOR: NVIDIA\nGL_RENDERER: RTX 5090\nVulkan device: Intel Arc\n")
	if !strings.Contains(report, "GL_RENDERER: RTX 5090") || !strings.Contains(report, "Vulkan device: Intel Arc") || strings.Contains(report, "unrelated") {
		t.Fatalf("report=%q", report)
	}
	env := gpuEnvironmentFacts([]string{"SECRET=do-not-log", "VK_DRIVER_FILES=C:\\drivers\\nv.json", "DRI_PRIME=1"})
	if strings.Contains(env, "SECRET") || !strings.Contains(env, "VK_DRIVER_FILES=") {
		t.Fatalf("environment=%q", env)
	}
}
