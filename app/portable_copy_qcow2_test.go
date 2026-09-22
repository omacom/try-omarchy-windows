package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"testing"
	"time"
)

func TestDirectPortableCopyDetachesAndPreservesBacking(t *testing.T) {
	tool := os.Getenv("QEMU_IMG")
	if tool == "" {
		var err error
		tool, err = exec.LookPath("qemu-img")
		if err != nil {
			t.Skip("qemu-img unavailable")
		}
	}
	dir, _ := backupFixture(t)
	if err := os.Remove(filepath.Join(dir, "vm", "disk.raw")); err != nil {
		t.Fatal(err)
	}
	base := bytes.Repeat([]byte{0x51}, 1<<20)
	digest := writePortableGuestReceipt(t, filepath.Join(dir, "guest"), base)
	overlay := filepath.Join(dir, "vm", "disk.qcow2")
	if err := createQcow2Overlay(overlay, "../guest/rootfs.ext4", 32<<20); err != nil {
		t.Fatal(err)
	}
	qemu, err := exec.LookPath("qemu-system-x86_64")
	if runtime.GOOS == "windows" {
		qemu = filepath.Join(filepath.Dir(tool), "qemu-system-x86_64w.exe")
		_, err = os.Stat(qemu)
	}
	if err != nil {
		t.Skip("QEMU runtime unavailable")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	vm := exec.CommandContext(ctx, qemu, "-machine", "none", "-nodefaults", "-display", "none", "-drive", "if=none,id=portable,format=qcow2,file="+overlay, "-qmp", "stdio")
	configureDiskTool(vm)
	stdin, err := vm.StdinPipe()
	if err != nil {
		t.Fatal(err)
	}
	stdout, err := vm.StdoutPipe()
	if err != nil {
		t.Fatal(err)
	}
	if err := vm.Start(); err != nil {
		t.Fatal(err)
	}
	defer vm.Process.Kill()
	decoder, encoder := json.NewDecoder(stdout), json.NewEncoder(stdin)
	var greeting map[string]json.RawMessage
	if err := decoder.Decode(&greeting); err != nil || greeting["QMP"] == nil {
		t.Fatalf("QMP greeting: %v", err)
	}
	for _, request := range []map[string]any{
		{"execute": "qmp_capabilities"},
		{"execute": "human-monitor-command", "arguments": map[string]string{"command-line": "qemu-io portable \"write -q -P 0x5a 2097152 4096\""}},
		{"execute": "quit"},
	} {
		if err := encoder.Encode(request); err != nil {
			t.Fatal(err)
		}
		for {
			var reply map[string]json.RawMessage
			if err := decoder.Decode(&reply); err != nil {
				t.Fatal(err)
			}
			if reply["error"] != nil {
				t.Fatalf("QMP error: %s", reply["error"])
			}
			if reply["return"] != nil {
				break
			}
		}
	}
	if err := vm.Wait(); err != nil {
		t.Fatal(err)
	}
	marker := bytes.Repeat([]byte{0x5a}, 4096)
	if err := writePortableBackingState(overlay, digest); err != nil {
		t.Fatal(err)
	}
	disk, err := inspectInstallationDisk(dir)
	if err != nil {
		t.Fatal(err)
	}
	before, err := os.ReadFile(overlay)
	if err != nil {
		t.Fatal(err)
	}
	previous := diskFreeBytes
	diskFreeBytes = func(string) (int64, error) { return diskSpaceReserve + (8 << 20), nil }
	t.Cleanup(func() { diskFreeBytes = previous })
	output := filepath.Join(t.TempDir(), "data")
	if err := stagePortableData(dir, output, tool, nil); err != nil {
		t.Fatal(err)
	}
	after, err := os.ReadFile(overlay)
	if err != nil || !bytes.Equal(before, after) {
		t.Fatal("source overlay changed")
	}
	standalone, err := inspectInstallationDisk(output)
	if err != nil || standalone.Backing != "" {
		t.Fatalf("not independent: %+v (%v)", standalone, err)
	}
	// A second-generation portable copy also needs no raw materialization.
	second := filepath.Join(t.TempDir(), "data")
	if err := stagePortableData(output, second, tool, nil); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(disk.Backing, []byte("changed factory"), 0600); err != nil {
		t.Fatal(err)
	}
	// Corrupt original backing must fail before creating any target files.
	rejected := filepath.Join(t.TempDir(), "data")
	if err := stagePortableData(dir, rejected, tool, nil); err == nil {
		t.Fatal("accepted corrupt factory")
	}
	if _, err := os.Lstat(rejected); !os.IsNotExist(err) {
		t.Fatal("corrupt backing left output")
	}
	// The standalone copy retains the old bytes despite the changed original.
	raw := filepath.Join(t.TempDir(), "verify.raw")
	cmd := exec.Command(tool, "convert", "-f", "qcow2", "-O", "raw", standalone.Path, raw)
	configureDiskTool(cmd)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("verify conversion: %v %s", err, out)
	}
	contents, err := os.ReadFile(raw)
	if err != nil || len(contents) != 32<<20 {
		t.Fatalf("standalone length=%d err=%v", len(contents), err)
	}
	if !bytes.Equal(contents[:len(base)], base) ||
		!zeroBytes(contents[len(base):2<<20]) || !bytes.Equal(contents[2<<20:(2<<20)+len(marker)], marker) || !zeroBytes(contents[(2<<20)+len(marker):]) {
		t.Fatal("standalone contents differ from factory and overlay writes")
	}
}
