package main

import (
	"context"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestUSBInventoryHelperListsWithoutClaimsAndCleansUp(t *testing.T) {
	qemu := os.Getenv("QEMU_SYSTEM")
	if qemu == "" {
		var err error
		qemu, err = exec.LookPath("qemu-system-x86_64")
		if err != nil {
			t.Skip("QEMU unavailable")
		}
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	devices, err := inventoryUSBDevices(ctx, qemu)
	if err != nil {
		if os.Getenv("QEMU_SYSTEM") == "" {
			t.Skip("requires USB-capable runtime: ", err)
		}
		t.Fatal(err)
	}
	for _, d := range devices {
		if d.Claimed {
			t.Fatal("inventory claimed hardware", d)
		}
	}
}
func TestUSBInventoryCancellationAndMissingRuntime(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := inventoryUSBDevices(ctx, filepath.Join(t.TempDir(), "missing")); err == nil {
		t.Fatal("ignored cancellation")
	}
	ctx, cancel = context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if _, err := inventoryUSBDevices(ctx, filepath.Join(t.TempDir(), "missing")); err == nil {
		t.Fatal("accepted missing runtime")
	}
}
