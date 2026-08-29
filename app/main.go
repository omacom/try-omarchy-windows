//go:build windows

package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"
)

// Try Omarchy for Windows - the native app shell. One exe replacing
// launch-omarchy.ps1 + winkey-forwarder.ps1 + clipboard-bridge.ps1:
// launches QEMU (WINQ-EMU GPU stack when installed, stock CPU fallback),
// supervises it through WHPX's rough edges, scopes the Windows key to the VM
// window, keeps the window branded, and bridges the clipboard. The SDL window
// IS the app - the shell itself shows nothing but error dialogs.

const (
	appTitle      = "Try Omarchy"
	qmpToolsPort  = 4445 // free for qmp.ps1 / provisioning tooling
	qmpFwdPort    = 4446 // winkey forwarder
	qmpSupPort    = 4447 // supervisor watchdog + lifecycle
	clipPushPort  = 4448 // clipboard: guest -> host
	clipPullPort  = 4449 // clipboard: host -> guest
	lifecyclePort = 4450 // guest shutdown intent ("reboot") - see runLifecycleListener
)

type config struct {
	dir, winqEmu, share       string
	fresh, fullscreen, noGpu  bool
	guestDir, vmDir, disk     string
	qemu                      string
	useGpu                    bool
	audio                     string
}

type buildSpec struct {
	Runtime struct {
		KernelCommandLine string `json:"kernelCommandLine"`
		Storage           struct {
			ExpandedSizeMiB int64 `json:"expandedSizeMiB"`
		} `json:"storage"`
	} `json:"runtime"`
}

var logFile *os.File

func logf(format string, a ...any) {
	if logFile != nil {
		fmt.Fprintf(logFile, "%s %s\n", time.Now().Format("15:04:05"), fmt.Sprintf(format, a...))
	}
}

func fatal(format string, a ...any) {
	msg := fmt.Sprintf(format, a...)
	logf("FATAL %s", msg)
	errorBox(msg)
	os.Exit(1)
}

func main() {
	cfg := &config{}
	flag.StringVar(&cfg.dir, "dir", filepath.Join(os.Getenv("LOCALAPPDATA"), "TryOmarchy"), "data directory")
	flag.StringVar(&cfg.winqEmu, "winq", `C:\WINQ-EMU`, "WINQ-EMU install path (GPU mode)")
	flag.StringVar(&cfg.share, "share", "", "host folder shared into the guest at /mnt/host (GPU mode)")
	flag.BoolVar(&cfg.fresh, "fresh", false, "discard the writable disk and start over")
	flag.BoolVar(&cfg.fullscreen, "fullscreen", false, "start fullscreen")
	flag.BoolVar(&cfg.noGpu, "nogpu", false, "force CPU rendering even if WINQ-EMU is installed")
	release := flag.String("release", "https://github.com/tsouth89/try-omarchy-windows/releases/download/v0.0.2-preview",
		"base URL the guest image is downloaded from on first run")
	flag.Parse()

	cfg.guestDir = filepath.Join(cfg.dir, "guest")
	cfg.vmDir = filepath.Join(cfg.dir, "vm")
	cfg.disk = filepath.Join(cfg.vmDir, "disk.raw")
	os.MkdirAll(cfg.vmDir, 0o755)
	logFile, _ = os.OpenFile(filepath.Join(cfg.vmDir, "shell.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	logf("---- %s starting ----", appTitle)

	// First run: the exe is the stub - fetch the guest image itself.
	if err := ensureGuest(cfg, *release); err != nil {
		fatal("Setting up the Omarchy image failed: %v\n\nCheck your connection and start Try Omarchy again.", err)
	}

	gpuQemu := filepath.Join(cfg.winqEmu, "bin", "qemu-system-x86_64w.exe")
	if !cfg.noGpu {
		if _, err := os.Stat(gpuQemu); err == nil {
			cfg.useGpu = true
		}
	}
	if cfg.useGpu {
		cfg.qemu = gpuQemu
	} else {
		cfg.qemu = `C:\Program Files\qemu\qemu-system-x86_64w.exe`
	}
	if _, err := os.Stat(cfg.qemu); err != nil {
		fatal("QEMU not found at %s.\n\nRun the bootstrap first (see the README).", cfg.qemu)
	}
	if cfg.share != "" {
		if st, err := os.Stat(cfg.share); err != nil || !st.IsDir() {
			fatal("Share folder not found: %s", cfg.share)
		}
		if !cfg.useGpu {
			fatal("Folder sharing needs the WINQ-EMU build (stock QEMU for Windows has no virtio-9p).")
		}
	}

	specData, err := os.ReadFile(filepath.Join(cfg.guestDir, "build-spec.json"))
	if err != nil {
		fatal("Cannot read build-spec.json: %v", err)
	}
	var spec buildSpec
	if err := json.Unmarshal(specData, &spec); err != nil {
		fatal("Cannot parse build-spec.json: %v", err)
	}
	// Serial log only - no console= on the display, so no kernel text or
	// blinking cursor flashes in the window before SDDM (boot problems: read
	// vm\serial*.log).
	cmdline := strings.ReplaceAll(spec.Runtime.KernelCommandLine, "console=tty0 ", "")
	cmdline = strings.ReplaceAll(cmdline, "console=hvc0", "console=ttyS0")
	cmdline += " vt.global_cursor_default=0"

	if err := prepareDisk(cfg, spec.Runtime.Storage.ExpandedSizeMiB); err != nil {
		fatal("Preparing the writable disk failed: %v", err)
	}

	// SDL's keyboard grab installs a system-wide Win-key hook that leaks past
	// window focus; our hook does it right (focus-scoped).
	os.Setenv("SDL_GRAB_KEYBOARD", "0")
	// Launch-UX contract (NOTES.md): guest console sized to the window it will
	// actually get, so the picture fills it from the first frame.
	conW, conH := screenSize(cfg.fullscreen)
	cmdline += fmt.Sprintf(" video=%dx%d", conW, conH)

	go runWinKeyHook()
	go runWinKeyQmp()
	go runTitleEnforcer(cfg.fullscreen)
	runClipboardBridge()
	runLifecycleListener()

	cfg.audio = "dsound"
	mode := "CPU rendering (llvmpipe)"
	if cfg.useGpu {
		mode = "GPU accelerated (virgl + Venus Vulkan)"
	}

	for relaunch := true; relaunch; {
		relaunch = supervise(cfg, cmdline, mode)
	}
	logf("---- exiting ----")
}

// supervise runs one guest lifetime: launch with the wedge watchdog, watch it
// until the guest goes down, reap a wedged QEMU. Returns true when the guest
// rebooted (with -no-reboot a guest reset exits QEMU; relaunching IS the
// reboot). The two hard-won subtleties from launch-omarchy.ps1 are preserved:
// liveness is probed (a QEMU wedged at guest poweroff cannot deliver its
// SHUTDOWN event), and a read stays permanently pending so a fast exit after
// guest-reset cannot discard the event (see docs/FINDINGS.md).
func supervise(cfg *config, cmdline string, mode string) bool {
	var proc *exec.Cmd
	var qmp *qmpConn
	for attempt := 1; attempt <= 4; attempt++ {
		logf("booting - %s (attempt %d)", mode, attempt)
		pendingReboot.Store(false)
		proc = exec.Command(cfg.qemu, buildQemuArgs(cfg, cmdline)...)
		// The w-binary's startup errors (bad args, SDL init) only ever reach
		// stderr; without this they vanish and a dead QEMU is undebuggable.
		if ef, err := os.OpenFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"),
			os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644); err == nil {
			proc.Stdout = ef
			proc.Stderr = ef
			defer ef.Close()
		}
		if err := proc.Start(); err != nil {
			fatal("QEMU failed to start: %v", err)
		}
		qemuPid.Store(uint32(proc.Process.Pid))
		exited := make(chan error, 1)
		go func() { exited <- proc.Wait() }()

		// Do NOT touch QMP during early guest boot: a monitor connection in
		// the first seconds reliably wedges QEMU's main loop under WHPX (the
		// "launch wedge" - near-certain nested, intermittent on hardware).
		// Give the guest a head start, then probe gently.
		deadline := time.Now().Add(60 * time.Second)
		wait := 10 * time.Second
		startupDead := false
	probe:
		for qmp == nil && time.Now().Before(deadline) {
			select {
			case <-exited:
				startupDead = true
				// No DirectSound device (VMs, some remote sessions) kills
				// QEMU at startup; retry silent rather than dying.
				if cfg.audio == "dsound" {
					logf("QEMU exited at startup - retrying without audio")
					cfg.audio = "none"
					break probe
				}
				fatal("QEMU exited at startup - see %s\\qemu-stderr.log.", cfg.vmDir)
			case <-time.After(wait):
				wait = 3 * time.Second
				qmp = qmpConnect(qmpSupPort, 8*time.Second)
			}
		}
		if qmp != nil {
			guestUp.Store(true)
			defer qmp.close()
			return watch(cfg, qmp, exited)
		}
		qemuPid.Store(0)
		if !startupDead {
			logf("QEMU is not answering (known WHPX launch wedge) - killing and retrying")
			proc.Process.Kill()
			<-exited
		}
		time.Sleep(2 * time.Second)
	}
	fatal("QEMU failed to come up healthy after 4 attempts.")
	return false
}

func watch(cfg *config, qmp *qmpConn, exited <-chan error) bool {
	lines := qmp.readLines()
	reason := ""
	silent := 0
	tick := 0
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	procDown := false
	for reason == "" && !procDown {
		select {
		case <-exited:
			procDown = true
		case line, ok := <-lines:
			if !ok {
				lines = nil // connection gone; QEMU is exiting
				procDown = waitExit(exited, 15*time.Second, cfg)
				break
			}
			silent = 0
			if r := shutdownReason(line); r != "" {
				reason = r
			}
		case <-ticker.C:
			tick++
			if tick%5 == 0 {
				if err := qmp.writeLine(`{"execute":"query-status"}`); err != nil {
					procDown = waitExit(exited, 15*time.Second, cfg)
					break
				}
				silent++
				if silent >= 9 {
					logf("QEMU main loop stopped answering - guest is down")
					procDown = waitExit(exited, 15*time.Second, cfg)
				}
			}
		}
	}
	// Collect a SHUTDOWN the pending read completed with after the loop ended.
	if reason == "" && lines != nil {
		for {
			select {
			case line, ok := <-lines:
				if !ok {
					lines = nil
				} else if r := shutdownReason(line); r != "" {
					reason = r
				}
				if reason != "" || lines == nil {
					goto drained
				}
			case <-time.After(500 * time.Millisecond):
				goto drained
			}
		}
	}
drained:
	if !procDown {
		waitExit(exited, 15*time.Second, cfg)
	}
	guestUp.Store(false)
	qemuPid.Store(0)
	// A QEMU wedged during the guest's reset can die without ever delivering
	// its SHUTDOWN event, making reboot and poweroff indistinguishable over
	// QMP (and the wedge also loses the serial file's final flush, so the
	// kernel's "Restarting system" line can't be sniffed either). The guest
	// image closes the gap: a shutdown unit reports reboot intent on the
	// lifecycle port before the network goes down.
	if reason == "" && pendingReboot.Swap(false) {
		reason = "reboot"
	}
	if reason == "reboot" {
		logf("guest rebooted - relaunching")
		return true
	}
	logf("guest powered off (%s)", reason)
	return false
}

var pendingReboot atomic.Bool

// runLifecycleListener receives the guest's shutdown intent: the image's
// try-omarchy-reboot-notify unit connects to 10.0.2.2:4450 (this listener via
// user-net) and says "reboot" when the guest is rebooting rather than
// powering off.
func runLifecycleListener() {
	l, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", lifecyclePort))
	if err != nil {
		fatal("Try Omarchy looks like it's already running (port %d is in use).", lifecyclePort)
	}
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				defer c.Close()
				c.SetReadDeadline(time.Now().Add(3 * time.Second))
				line, _ := bufio.NewReader(c).ReadString('\n')
				if strings.TrimSpace(line) == "reboot" {
					logf("guest announced reboot")
					pendingReboot.Store(true)
				}
			}(c)
		}
	}()
}

// waitExit reaps QEMU: stock WHPX wedges instead of exiting after a guest
// shutdown, so after a grace period the husk is killed. Returns true once the
// process is gone.
func waitExit(exited <-chan error, grace time.Duration, cfg *config) bool {
	select {
	case <-exited:
		return true
	case <-time.After(grace):
		logf("QEMU wedged after guest shutdown (stock WHPX trap) - cleaning up")
		if pid := qemuPid.Load(); pid != 0 {
			if p, err := os.FindProcess(int(pid)); err == nil {
				p.Kill()
			}
		}
		<-exited
		return true
	}
}
