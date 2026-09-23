package main

import (
	"os"
	"path/filepath"
	"slices"
	"testing"
)

func TestFreePageReportingRequiresPatchedRuntime(t *testing.T) {
	qemu := filepath.Join(t.TempDir(), "bin", "qemu.exe")
	cfg := &config{qemu: qemu, memMiB: 4096, cpus: 4, guestDir: "guest", vmDir: "vm", disk: "disk.raw", diskFormat: "raw", audio: "none"}
	const device = "virtio-balloon-pci,free-page-reporting=on"
	if slices.Contains(buildQemuArgs(cfg, ""), device) {
		t.Fatal("an older runtime would report free pages without reclaiming them")
	}
	provenance := filepath.Join(filepath.Dir(filepath.Dir(qemu)), "provenance")
	if err := os.MkdirAll(provenance, 0755); err != nil {
		t.Fatal(err)
	}
	manifest := `{"qemu":{"patches":[{"file":"patches/qemu/0015-reclaim-free-guest-pages-on-whpx.patch"}]}}`
	if err := os.WriteFile(filepath.Join(provenance, "sources.lock.json"), []byte(manifest), 0600); err != nil {
		t.Fatal(err)
	}
	if !slices.Contains(buildQemuArgs(cfg, ""), device) {
		t.Fatal("patched runtime did not enable free page reporting")
	}
}
