//go:build windows

package main

import (
	"archive/zip"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

// First-run machine setup: everything bootstrap.ps1 did, done by the app
// itself so tryomarchy.com can honestly say "download one file and open it".
// Two machine-wide pieces are handled here:
//
//   - Windows Hypervisor Platform. Checked unprivileged via WinHvPlatform.dll
//     (WHvGetCapability answers "is the hypervisor running", which is the
//     question - the feature being enabled but unbooted still means WHPX
//     fails). Enabling relaunches this exe elevated (-enable-whp) to run dism,
//     and Windows then needs its one restart.
//   - QEMU. A portable WINQ-EMU tree (the validated GPU stack, same bin/
//     layout as C:\WINQ-EMU) is downloaded from the release into
//     %LOCALAPPDATA%\TryOmarchy\runtime, SHA256-verified like the image.
//     Existing installs (C:\WINQ-EMU, stock QEMU from the old bootstrap)
//     still win, so nothing already set up changes behavior.

const runtimeZip = "winq-emu-alpha10-portable.zip"

var (
	winhv                   = syscall.NewLazyDLL("WinHvPlatform.dll")
	procWHvGetCapability    = winhv.NewProc("WHvGetCapability")
	shell32                 = syscall.NewLazyDLL("shell32.dll")
	procShellExecuteExW     = shell32.NewProc("ShellExecuteExW")
	procWaitForSingleObject = kernel32.NewProc("WaitForSingleObject")
	procGetExitCodeProcess  = kernel32.NewProc("GetExitCodeProcess")
	procCloseHandle         = kernel32.NewProc("CloseHandle")
	procGetTickCount64      = kernel32.NewProc("GetTickCount64")
)

const (
	mbIconInfo     = 0x40
	mbIconQuestion = 0x20
	mbYesNo        = 0x04
	mbOkCancel     = 0x01
	idOk           = 1
	idYes          = 6

	seeMaskNoCloseProcess = 0x40
	errorCancelled        = 1223 // user said No to the UAC prompt
	dismRebootRequired    = 3010
	createNoWindow        = 0x08000000
)

func msgBox(text string, flags uintptr) int {
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString(appTitle)
	r, _, _ := procMessageBoxW.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), flags|mbFront)
	return int(r)
}

// whpPresent reports whether the Windows hypervisor is actually running - the
// exact condition WHPX needs. False on a fresh machine (feature off), after
// enabling but before the reboot, and when virtualization is off in firmware.
func whpPresent() bool {
	if winhv.Load() != nil || procWHvGetCapability.Find() != nil {
		return false
	}
	var present, written uint32
	// capability code 0 = WHvCapabilityCodeHypervisorPresent
	hr, _, _ := procWHvGetCapability.Call(0, uintptr(unsafe.Pointer(&present)), 4, uintptr(unsafe.Pointer(&written)))
	return hr == 0 && present != 0
}

// runDismEnable is the whole life of the elevated instance (-enable-whp):
// enable the feature, hand dism's exit code (0 ok, 3010 reboot needed) back
// to the waiting unelevated parent.
func runDismEnable() int {
	cmd := exec.Command(system32("dism.exe"), "/online", "/enable-feature",
		"/featurename:HypervisorPlatform", "/all", "/norestart", "/quiet")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	err := cmd.Run()
	if err == nil {
		return 0
	}
	if ee, ok := err.(*exec.ExitError); ok {
		return ee.ExitCode()
	}
	return 1
}

type shellExecuteInfo struct {
	cbSize         uint32
	fMask          uint32
	hwnd           uintptr
	lpVerb         *uint16
	lpFile         *uint16
	lpParameters   *uint16
	lpDirectory    *uint16
	nShow          int32
	hInstApp       uintptr
	lpIDList       uintptr
	lpClass        *uint16
	hkeyClass      uintptr
	dwHotKey       uint32
	hIconOrMonitor uintptr
	hProcess       uintptr
}

// runElevated relaunches this exe with args through the UAC prompt and waits
// for its exit code. Returns errorCancelled when the user declines the prompt.
func runElevated(args string) (int, error) {
	self, err := os.Executable()
	if err != nil {
		return 0, err
	}
	verb, _ := syscall.UTF16PtrFromString("runas")
	file, _ := syscall.UTF16PtrFromString(self)
	params, _ := syscall.UTF16PtrFromString(args)
	si := shellExecuteInfo{fMask: seeMaskNoCloseProcess, lpVerb: verb, lpFile: file, lpParameters: params, nShow: 1}
	si.cbSize = uint32(unsafe.Sizeof(si))
	r, _, lastErr := procShellExecuteExW.Call(uintptr(unsafe.Pointer(&si)))
	if r == 0 {
		if errno, ok := lastErr.(syscall.Errno); ok && int(errno) == errorCancelled {
			return errorCancelled, nil
		}
		return 0, fmt.Errorf("ShellExecuteEx: %v", lastErr)
	}
	if si.hProcess == 0 {
		return 0, fmt.Errorf("elevation returned no process handle")
	}
	defer procCloseHandle.Call(si.hProcess)
	procWaitForSingleObject.Call(si.hProcess, uintptr(0xFFFFFFFF))
	var code uint32
	procGetExitCodeProcess.Call(si.hProcess, uintptr(unsafe.Pointer(&code)))
	return int(code), nil
}

// system32 resolves a tool absolutely - the elevated helper and the restart
// must not depend on whatever PATH the launching shell had.
func system32(tool string) string {
	if windir := os.Getenv("WINDIR"); windir != "" {
		return filepath.Join(windir, "System32", tool)
	}
	return tool
}

func restartWindows() {
	cmd := exec.Command(system32("shutdown.exe"), "/r", "/t", "3")
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}
	cmd.Start()
	os.Exit(0)
}

// ensureWHP walks the user through switching the hypervisor on. Returns only
// when WHPX is usable; every other path explains itself and exits.
func ensureWHP(cfg *config) {
	if whpPresent() {
		return
	}
	marker := filepath.Join(cfg.dir, "whp-requested")
	if st, err := os.Stat(marker); err == nil {
		// The feature was already enabled on an earlier run. If the machine
		// has rebooted since and the hypervisor is still absent, dism did its
		// part and firmware is the blocker; otherwise the restart just hasn't
		// happened yet.
		uptimeMs, _, _ := procGetTickCount64.Call()
		bootTime := time.Now().Add(-time.Duration(uptimeMs) * time.Millisecond)
		if st.ModTime().Before(bootTime) {
			fatal("Windows' virtualization is switched on, but your PC's hardware virtualization looks disabled.\n\nEnable it in your PC's BIOS/UEFI settings (usually called Intel VT-x, AMD-V, or SVM), then start Try Omarchy again.")
		}
		if msgBox("Windows still needs to restart once to finish setting up. Restart now?", mbYesNo|mbIconQuestion) == idYes {
			restartWindows()
		}
		os.Exit(0)
	}
	if msgBox("Try Omarchy uses virtualization that Windows already includes (the same feature WSL2 uses), but it isn't switched on yet.\n\nWindows will ask for permission, and will need to restart once. Ready?", mbOkCancel|mbIconInfo) != idOk {
		os.Exit(0)
	}
	logf("enabling WHP (elevated dism)")
	// dism can take a minute or two; without a window the app looks hung.
	ui := getUI()
	ui.setStatus("Switching on Windows' virtualization...")
	code, err := runElevated("-enable-whp")
	if err != nil {
		fatal("Couldn't switch on Windows' virtualization: %v", err)
	}
	if code == errorCancelled {
		fatal("Try Omarchy can't run without Windows' virtualization. Start it again when you're ready to allow it.")
	}
	if code != 0 && code != dismRebootRequired {
		fatal("Windows couldn't enable its virtualization feature (error %d).\n\nYou can enable it manually: Windows Features > Windows Hypervisor Platform.", code)
	}
	os.WriteFile(marker, []byte(time.Now().Format(time.RFC3339)+"\n"), 0o644)
	logf("WHP enable requested (dism exit %d)", code)
	if code == 0 && whpPresent() {
		return
	}
	if msgBox("Done. Windows needs to restart once to finish setting up. Restart now?", mbYesNo|mbIconQuestion) == idYes {
		restartWindows()
	}
	os.Exit(0)
}

// ensureRuntime downloads and unpacks the portable WINQ-EMU tree on first run.
// Same trust chain as the image: fetched from the release, SHA256-verified.
func ensureRuntime(cfg *config, release string) (string, error) {
	root := filepath.Join(cfg.dir, "runtime")
	if _, err := os.Stat(filepath.Join(root, "bin", "qemu-system-x86_64w.exe")); err == nil {
		return root, nil
	}
	ui := getUI()
	client := &http.Client{Timeout: 0}
	sums, err := fetchSums(client, release)
	if err != nil {
		return "", fmt.Errorf("downloading SHA256SUMS: %w", err)
	}
	zipPath := filepath.Join(cfg.dir, runtimeZip)
	if _, err := os.Stat(zipPath); err != nil {
		ui.setStatus("Downloading the graphics engine...")
		if err := download(client, release+"/"+runtimeZip, zipPath, sums[runtimeZip], ui); err != nil {
			return "", fmt.Errorf("downloading %s: %w", runtimeZip, err)
		}
	}
	ui.setStatus("Unpacking the graphics engine...")
	tmp := root + ".part"
	os.RemoveAll(tmp)
	if err := unzipTree(zipPath, tmp, ui); err != nil {
		os.RemoveAll(tmp)
		os.Remove(zipPath)
		return "", fmt.Errorf("unpacking %s: %w", runtimeZip, err)
	}
	os.RemoveAll(root)
	// Defender (and indexers) briefly hold handles on freshly unpacked
	// binaries, failing the directory rename with access-denied - it happened
	// on hardware on the first try. Retry over a few seconds.
	var renameErr error
	for attempt := 0; attempt < 15; attempt++ {
		if renameErr = os.Rename(tmp, root); renameErr == nil {
			break
		}
		time.Sleep(500 * time.Millisecond)
	}
	if renameErr != nil {
		return "", renameErr
	}
	os.Remove(zipPath)
	return root, nil
}

func unzipTree(src, dest string, ui *progressUI) error {
	r, err := zip.OpenReader(src)
	if err != nil {
		return err
	}
	defer r.Close()
	var total, done int64
	for _, f := range r.File {
		total += int64(f.UncompressedSize64)
	}
	for _, f := range r.File {
		name := filepath.Clean(f.Name)
		if name == "." || strings.HasPrefix(name, "..") || filepath.IsAbs(name) {
			continue
		}
		path := filepath.Join(dest, name)
		if f.FileInfo().IsDir() {
			if err := os.MkdirAll(path, 0o755); err != nil {
				return err
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		in, err := f.Open()
		if err != nil {
			return err
		}
		out, err := os.Create(path)
		if err != nil {
			in.Close()
			return err
		}
		n, err := io.Copy(out, in)
		in.Close()
		if cerr := out.Close(); err == nil {
			err = cerr
		}
		if err != nil {
			return err
		}
		done += n
		ui.setProgress(done, total)
	}
	return nil
}
