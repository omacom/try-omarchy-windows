package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRenderModeAcceptsDxg(t *testing.T) {
	mode, err := parseRenderMode(" DXG ")
	if err != nil || mode != renderDXG {
		t.Fatalf("parseRenderMode(dxg) = %q, %v", mode, err)
	}
	if gpu, _ := startWithGPU(renderDXG, nil, "", "", time.Now()); gpu {
		t.Fatal("dxg must not start the virgl GPU path")
	}
	if shouldRecordRenderResult("sha256:x", renderDXG, false, false) {
		t.Fatal("a dxg launch must not overwrite the GPU/CPU render history")
	}
}

func TestDxgSharesAreTheWSLLayout(t *testing.T) {
	shares := dxgHostShares(`C:\Windows`, `C:\Program Files`)
	want := map[string]string{
		"wsllib":     filepath.Join(`C:\Windows`, "System32", "lxss", "lib"),
		"wslcore":    filepath.Join(`C:\Program Files`, "WSL", "lib"),
		"wsldrivers": filepath.Join(`C:\Windows`, "System32", "DriverStore", "FileRepository"),
	}
	if len(shares) != len(want) {
		t.Fatalf("shares: %v", shares)
	}
	for _, s := range shares {
		if want[s.tag] != s.path {
			t.Fatalf("share %s = %s, want %s", s.tag, s.path, want[s.tag])
		}
	}
}

func TestPrepareDxgLaunchNamesWhatIsMissing(t *testing.T) {
	dir := t.TempDir()
	_, _, err := prepareDxgLaunch(dir)
	if err == nil || !strings.Contains(err.Error(), "virtio-dxg runtime") ||
		!strings.Contains(err.Error(), "guest Mesa build") {
		t.Fatalf("missing files not reported: %v", err)
	}
}

func TestPrepareDxgLaunchFindsItsFiles(t *testing.T) {
	dir := t.TempDir()
	root := dxgDir(dir)
	for _, f := range []string{
		filepath.Join(root, "runtime", "bin", "qemu-system-x86_64w.exe"),
		filepath.Join(root, "initramfs-linux.img"),
		filepath.Join(root, "payload", "mesa-virtio-dxg.tar"),
	} {
		if err := os.MkdirAll(filepath.Dir(f), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(f, nil, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	// The host shares come from the real system; point them at the test.
	for _, d := range []string{
		filepath.Join(dir, "win", "System32", "lxss", "lib"),
		filepath.Join(dir, "pf", "WSL", "lib"),
		filepath.Join(dir, "win", "System32", "DriverStore", "FileRepository"),
	} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	t.Setenv("SystemRoot", filepath.Join(dir, "win"))
	t.Setenv("ProgramFiles", filepath.Join(dir, "pf"))
	l, qemu, err := prepareDxgLaunch(dir)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.HasSuffix(qemu, filepath.Join("virtio-dxg", "runtime", "bin", "qemu-system-x86_64w.exe")) {
		t.Fatalf("runtime %s", qemu)
	}
	if !l.enabled || len(l.shares) != 4 || l.shares[3].tag != "vdxgpayload" {
		t.Fatalf("launch %+v", l)
	}
}

func TestBuildQemuArgsForDxg(t *testing.T) {
	cfg := &config{vmDir: "vm", guestDir: "guest", disk: "disk.raw", diskFormat: "raw",
		memMiB: 4096, audio: "none", diskSnapshot: true,
		dxg: dxgLaunch{enabled: true, initramfs: filepath.Join("vdxg", "initramfs-linux.img"),
			shares: []dxgShare{{"wsllib", `C:\Windows\System32\lxss\lib`}, {"vdxgpayload", `D:\x,y`}}}}
	args := strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	for _, want := range []string{
		"-cpu host ",
		"-device virtio-dxg-pci",
		"-initrd " + filepath.Join("vdxg", "initramfs-linux.img") + " ",
		"tryomarchy.render=gpu virtio_dxg.desktop=1",
		"mount_tag=wsllib,security_model=none,readonly=on",
		// QEMU option escaping: a comma in a path is doubled.
		`path=D:\x,,y,mount_tag=vdxgpayload`,
		",if=virtio,snapshot=on",
		// Zero-copy scanout draws GPU textures with OpenGL.
		"-display sdl,gl=on,",
	} {
		if !strings.Contains(args, want) {
			t.Fatalf("missing %q in %s", want, args)
		}
	}
	cfg.dxg, cfg.diskSnapshot = dxgLaunch{}, false
	args = strings.Join(buildQemuArgs(cfg, "root=/dev/vda"), " ")
	if strings.Contains(args, "virtio-dxg") || strings.Contains(args, "snapshot=on") ||
		!strings.Contains(args, "tryomarchy.render=cpu") || strings.Contains(args, "-cpu host") ||
		!strings.Contains(args, "-display sdl,gl=off,") {
		t.Fatalf("CPU mode changed by dxg support: %s", args)
	}
}
