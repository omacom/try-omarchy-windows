package main

import (
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func writePortableGuestReceipt(t *testing.T, guest string, rootfs []byte) string {
	t.Helper()
	if err := os.MkdirAll(guest, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(guest, "rootfs.ext4"), rootfs, 0o644); err != nil {
		t.Fatal(err)
	}
	digest := testSHA256(rootfs)
	if err := writeInstallReceipt(guest, "https://example.invalid/v0.0.9-preview", testSHA256([]byte("manifest")),
		[]string{"rootfs.ext4"}, map[string]string{"rootfs.ext4": digest}); err != nil {
		t.Fatal(err)
	}
	return digest
}

func TestCreateQcow2Overlay(t *testing.T) {
	configureSetupCancellation(false)
	root := t.TempDir()
	guest := filepath.Join(root, "guest")
	vm := filepath.Join(root, "vm")
	if err := os.MkdirAll(vm, 0o755); err != nil {
		t.Fatal(err)
	}
	backingSHA256 := writePortableGuestReceipt(t, guest, make([]byte, 4096))
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
	state, err := os.ReadFile(portableBackingStatePath(disk))
	if err != nil || strings.TrimSpace(string(state)) != backingSHA256 {
		t.Fatalf("portable backing state = %q, %v", state, err)
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
	guest := filepath.Join(dir, "guest")
	vm := filepath.Join(dir, "vm")
	if err := os.MkdirAll(vm, 0o755); err != nil {
		t.Fatal(err)
	}
	disk := filepath.Join(vm, "disk.qcow2")
	writePortableGuestReceipt(t, guest, []byte("factory"))
	if err := os.WriteFile(disk, []byte("partial"), 0o644); err != nil {
		t.Fatal(err)
	}
	cfg := &config{portable: true, guestDir: guest, vmDir: vm, disk: disk, diskFormat: "qcow2"}
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

func TestPortableDiskRejectsChangedBackingImage(t *testing.T) {
	configureSetupCancellation(false)
	dir := t.TempDir()
	guest := filepath.Join(dir, "guest")
	vm := filepath.Join(dir, "vm")
	if err := os.MkdirAll(vm, 0o755); err != nil {
		t.Fatal(err)
	}
	writePortableGuestReceipt(t, guest, []byte("factory-one"))
	cfg := &config{portable: true, guestDir: guest, vmDir: vm, disk: filepath.Join(vm, "disk.qcow2"), diskFormat: "qcow2"}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	writePortableGuestReceipt(t, guest, []byte("factory-two"))
	if err := prepareDisk(cfg, 1); err == nil || !strings.Contains(err.Error(), "different factory image") {
		t.Fatalf("changed backing image error = %v", err)
	}
	if err := prepareDisk(&config{portable: true, fresh: true, guestDir: guest, vmDir: vm, disk: cfg.disk, diskFormat: "qcow2"}, 1); err != nil {
		t.Fatalf("fresh portable disk did not adopt the new backing: %v", err)
	}
}

func TestPortableDiskRejectsMissingBackingIdentity(t *testing.T) {
	configureSetupCancellation(false)
	dir := t.TempDir()
	guest := filepath.Join(dir, "guest")
	vm := filepath.Join(dir, "vm")
	if err := os.MkdirAll(vm, 0o755); err != nil {
		t.Fatal(err)
	}
	writePortableGuestReceipt(t, guest, []byte("factory"))
	cfg := &config{portable: true, guestDir: guest, vmDir: vm, disk: filepath.Join(vm, "disk.qcow2"), diskFormat: "qcow2"}
	if err := prepareDisk(cfg, 1); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(portableBackingStatePath(cfg.disk)); err != nil {
		t.Fatal(err)
	}
	if err := prepareDisk(cfg, 1); err == nil || !strings.Contains(err.Error(), "backing identity is missing") {
		t.Fatalf("missing backing identity error = %v", err)
	}
}
