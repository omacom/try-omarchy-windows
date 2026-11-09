package main

// virtio-dxg rendering (-render dxg): the whole guest desktop, OpenGL,
// Vulkan, CUDA and video run on the host GPU through NVIDIA's own Windows
// driver (see virtio-dxg/ and docs/VIRTIO-DXG.md). It is opt-in and keeps
// everything else about a launch as it is: the default runtime, the GPU
// and CPU modes, their render history and the upstream updates.
//
// Its files live in <data>\virtio-dxg:
//
//	runtime\bin\qemu-system-x86_64w.exe  a WINQ-EMU runtime with virtio-dxg-pci
//	initramfs-linux.img                  the guest's initramfs plus the module
//	                                     and its boot hook (mkdevinitramfs.py)
//	payload\mesa-virtio-dxg.tar          the guest's Mesa build (guest/mesa)
//
// The NVIDIA user-mode drivers are never copied: the guest reads them from
// this machine's driver store and WSL libraries, read-only, as WSL2 does.

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const renderDXG = "dxg"

type dxgLaunch struct {
	enabled   bool
	dir       string
	initramfs string
	payload   string
	shares    []dxgShare
}

type dxgShare struct{ tag, path string }

func dxgDir(dataDir string) string { return filepath.Join(dataDir, "virtio-dxg") }

// dxgHostShares lists the host folders the guest's GPU stack reads: NVIDIA's
// dxgkrnl libraries (installed by its driver), Microsoft's D3D12 and DXCore
// for Linux (installed with WSL) and the driver store.
func dxgHostShares(systemRoot, programFiles string) []dxgShare {
	system32 := filepath.Join(systemRoot, "System32")
	return []dxgShare{
		{"wsllib", filepath.Join(system32, "lxss", "lib")},
		{"wslcore", filepath.Join(programFiles, "WSL", "lib")},
		{"wsldrivers", filepath.Join(system32, "DriverStore", "FileRepository")},
	}
}

// prepareDxgLaunch checks everything a virtio-dxg boot needs and returns the
// runtime to start, or an error that says what is missing.
func prepareDxgLaunch(dataDir string) (dxgLaunch, string, error) {
	dir := dxgDir(dataDir)
	l := dxgLaunch{
		enabled:   true,
		dir:       dir,
		initramfs: filepath.Join(dir, "initramfs-linux.img"),
		payload:   filepath.Join(dir, "payload"),
	}
	qemu := filepath.Join(dir, "runtime", "bin", "qemu-system-x86_64w.exe")
	need := []struct{ path, what string }{
		{qemu, "the virtio-dxg runtime"},
		{l.initramfs, "the virtio-dxg initramfs"},
		{filepath.Join(l.payload, "mesa-virtio-dxg.tar"), "the guest Mesa build"},
	}
	systemRoot := os.Getenv("SystemRoot")
	if systemRoot == "" {
		systemRoot = `C:\Windows`
	}
	programFiles := os.Getenv("ProgramFiles")
	if programFiles == "" {
		programFiles = `C:\Program Files`
	}
	l.shares = dxgHostShares(systemRoot, programFiles)
	for _, s := range l.shares {
		need = append(need, struct{ path, what string }{s.path, s.tag})
	}
	var missing []string
	for _, n := range need {
		if _, err := os.Stat(n.path); err != nil {
			missing = append(missing, fmt.Sprintf("%s (%s)", n.what, n.path))
		}
	}
	if len(missing) > 0 {
		return dxgLaunch{}, "", fmt.Errorf("virtio-dxg rendering is not set up: missing %s", strings.Join(missing, ", "))
	}
	l.shares = append(l.shares, dxgShare{"vdxgpayload", l.payload})
	return l, qemu, nil
}

// dxgQemuArgs adds the device and the read-only shares. The display stays
// the 2D virtio-gpu of CPU mode: frames reach it from the GPU (KMS mirrors).
func dxgQemuArgs(l dxgLaunch) []string {
	if !l.enabled {
		return nil
	}
	args := []string{"-device", "virtio-dxg-pci"}
	for _, s := range l.shares {
		args = append(args, "-virtfs",
			"local,path="+qemuOptionValue(s.path)+",mount_tag="+s.tag+",security_model=none,readonly=on")
	}
	return args
}

// dxgCmdline is what the boot hook reads (virtio-dxg/guest/test/initcpio).
func dxgCmdline(l dxgLaunch) string {
	if !l.enabled {
		return ""
	}
	return " virtio_dxg.desktop=1"
}
