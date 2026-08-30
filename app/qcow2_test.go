package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
)

func TestCreateQcow2Overlay(t *testing.T) {
	configureSetupCancellation(false)
	root := t.TempDir()
	guest := filepath.Join(root, "guest")
	vm := filepath.Join(root, "vm")
	if err := os.MkdirAll(guest, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(vm, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(guest, "rootfs.ext4"), make([]byte, 4096), 0o644); err != nil {
		t.Fatal(err)
	}
	disk := filepath.Join(vm, "disk.qcow2")
	cfg := &config{portable: true, guestDir: guest, vmDir: vm, disk: disk, diskFormat: "qcow2"}
	const virtualMiB = int64(24 * 1024)
	if err := prepareDisk(cfg, virtualMiB); err != nil {
		t.Fatal(err)
	}
	const virtualSize = virtualMiB * 1024 * 1024
	ok, err := qcow2OverlayMatches(disk, "../guest/rootfs.ext4", virtualSize)
	if err != nil {
		t.Fatal(err)
	}
	if !ok {
		t.Fatal("published QCOW2 metadata does not match")
	}
	if _, err := os.Stat(disk + ".part"); !os.IsNotExist(err) {
		t.Fatalf("staging file remains: %v", err)
	}
	if qemuImg := os.Getenv("QEMU_IMG"); qemuImg != "" {
		out, err := exec.Command(qemuImg, "info", "--output=json", "--backing-chain", disk).Output()
		if err != nil {
			t.Fatalf("qemu-img rejected overlay: %v", err)
		}
		var chain []struct {
			Format      string `json:"format"`
			VirtualSize int64  `json:"virtual-size"`
		}
		if err := json.Unmarshal(out, &chain); err != nil {
			t.Fatal(err)
		}
		if len(chain) != 2 || chain[0].Format != "qcow2" || chain[0].VirtualSize != virtualSize {
			t.Fatalf("unexpected backing chain: %+v", chain)
		}
		if out, err := exec.Command(qemuImg, "check", "-f", "qcow2", disk).CombinedOutput(); err != nil {
			t.Fatalf("qemu-img found invalid metadata: %v (%s)", err, out)
		}
	}
}

func TestPortableDiskQuarantinesInvalidFinalFile(t *testing.T) {
	configureSetupCancellation(false)
	dir := t.TempDir()
	vm := filepath.Join(dir, "vm")
	if err := os.MkdirAll(vm, 0o755); err != nil {
		t.Fatal(err)
	}
	disk := filepath.Join(vm, "disk.qcow2")
	if err := os.WriteFile(disk, []byte("partial"), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := &config{portable: true, vmDir: vm, disk: disk, diskFormat: "qcow2"}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	matches, err := filepath.Glob(disk + ".incomplete-*")
	if err != nil {
		t.Fatal(err)
	}
	if len(matches) != 1 {
		t.Fatalf("quarantined disks = %d, want 1", len(matches))
	}
}
