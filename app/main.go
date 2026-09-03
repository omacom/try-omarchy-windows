//go:build windows

package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
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

const appTitle = "Try Omarchy"

type config struct {
	dir, hostDir, payloadDir string
	winqEmu, share           string
	fresh, fullscreen, noGpu bool
	hostCursor               bool
	instant, portable        bool
	guestDir, vmDir, disk    string
	diskFormat               string
	qemu                     string
	useGpu                   bool
	audio                    string
	memMiB                   int
	// kernel-irqchip=off keeps WHPX from requesting nested virtualization,
	// which some hosts advertise and then refuse (issue #19). Set by the
	// startup retry, never by a flag.
	forwards []portForward
	sshKey   string
	// Guest RAM chosen by the user (settings.json or -memory); 0 = automatic.
	memOverrideMiB int
	irqchipOff     bool
}

// pickGuestMem sizes the guest to the machine instead of demanding a fixed
// 4-6 GB (which dies with "cannot set up guest memory" on a busy 8 GB PC):
// the mode's ideal, minus a ~2 GB cushion for Windows, floored at 1 GB.
// Omarchy runs lean (zram in the image), so a small guest beats no guest;
// truly starved machines get the clean message from the memory ladder.
func pickGuestMem(gpu bool) int {
	want := 4096
	if gpu {
		want = 6144
	}
	_, avail := availMemMiB()
	if avail == 0 {
		return want
	}
	m := avail - 2048
	if m > want {
		m = want
	}
	if m < 1024 {
		m = 1024
	}
	return m
}

// memoryStarved reports whether the current attempt's QEMU died because the
// guest RAM couldn't be allocated (stderr is truncated per attempt).
func memoryStarved(cfg *config) bool {
	data, err := os.ReadFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"))
	return err == nil && bytes.Contains(data, []byte("cannot set up guest memory"))
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

// Kept as a variable so isolated signed test builds can use their own data
// directory without changing production behavior.
var defaultDataDirectoryName = "TryOmarchy"

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

func finishSetupCancellation(cfg *config, err error) bool {
	if !setupCancelled() && !errors.Is(err, errSetupCancelled) {
		return false
	}
	getUI().setStatus("Cancelling and cleaning up...")
	logf("setup cancelled by user")
	if logFile != nil {
		logFile.Close()
		logFile = nil
	}
	executable, _ := os.Executable()
	if cleanupErr := cleanupCancelledSetup(cfg.dir, executable, cancelRemovesAll.Load()); cleanupErr != nil {
		errorBox(fmt.Sprintf("Setup was cancelled, but some temporary files could not be removed:\n\n%v\n\nYou can safely delete %s manually.", cleanupErr, cfg.dir))
	}
	uiDone()
	return true
}

func main() {
	cfg := &config{}
	flag.StringVar(&cfg.dir, "dir", filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName), "data directory")
	flag.StringVar(&cfg.winqEmu, "winq", `C:\WINQ-EMU`, "WINQ-EMU install path (GPU mode)")
	flag.StringVar(&cfg.share, "share", "", "Windows folder shared into Omarchy at /mnt/host and as ~/<folder name> (GPU mode)")
	flag.BoolVar(&cfg.fresh, "fresh", false, "discard the writable disk and start over")
	flag.BoolVar(&cfg.fullscreen, "fullscreen", false, "start fullscreen (Immersive)")
	flag.IntVar(&cfg.memOverrideMiB, "memory", 0, "guest RAM in MiB (default: sized to this PC)")
	flag.BoolVar(&cfg.noGpu, "nogpu", false, "force CPU rendering even if WINQ-EMU is installed")
	flag.BoolVar(&cfg.hostCursor, "host-cursor", false, "force the legacy Windows cursor over the guest")
	flag.BoolVar(&cfg.instant, "instant", false, "skip first-boot questions and use the trial account")
	flag.BoolVar(&cfg.portable, "portable", false, "run entirely from data and payload folders beside the executable")
	var forwards forwardList
	flag.Var(&forwards, "forward", "forward a Windows loopback port into Omarchy, as tcp:2222:22 or 8080:80 (repeatable)")
	sshPort := flag.Int("ssh", 0, "forward this Windows loopback port to Omarchy's sshd and start sshd for the session")
	openSettings := flag.Bool("settings", false, "open the settings window, then exit")
	diagnostics := flag.Bool("diagnostics", false, "write a zip of logs, settings, and machine facts for a bug report, then exit")
	sshKeyPath := flag.String("ssh-key", "", "public key to authorize for the Omarchy account (default: your ~/.ssh/id_*.pub when -ssh is used)")
	noUpdate := flag.Bool("no-update", false, "do not check for launcher or guest updates")
	updateURL := flag.String("update-url", defaultUpdateURL, "authenticated update manifest URL")
	release := flag.String("release", defaultReleaseURL,
		"base URL the guest image is downloaded from on first run")
	sumsSHA256 := flag.String("sums-sha256", defaultSumsSHA256,
		"trusted SHA256 digest of the release's SHA256SUMS file")
	runtimeRelease := flag.String("runtime-release", defaultRuntimeReleaseURL,
		"base URL the graphics runtime is downloaded from")
	runtimeSumsSHA256 := flag.String("runtime-sums-sha256", defaultRuntimeSumsSHA256,
		"trusted SHA256 digest of the runtime release's SHA256SUMS file")
	enableWhp := flag.Bool("enable-whp", false, "internal: elevated helper that enables the Windows Hypervisor Platform")
	applyLauncherUpdateFlag := flag.Bool("apply-launcher-update", false, "internal: apply a staged launcher update")
	applyLauncherRollbackFlag := flag.Bool("apply-launcher-rollback", false, "internal: restore the previous launcher")
	updateWaitPID := flag.Int("update-wait-pid", 0, "internal: process to wait for before replacing the launcher")
	updateRestartArgs := flag.String("update-restart-args", "", "internal: encoded launcher restart arguments")
	flag.Parse()
	if strings.TrimSpace(*runtimeRelease) == "" {
		*runtimeRelease = *release
	}
	if strings.TrimSpace(*runtimeSumsSHA256) == "" {
		*runtimeSumsSHA256 = *sumsSHA256
	}

	// The elevated relaunch does exactly one thing and reports back via exit
	// code (see setup.go); it must not touch the single-instance port.
	if *enableWhp {
		os.Exit(runDismEnable())
	}
	if cfg.portable {
		self, err := os.Executable()
		if err != nil {
			fatal("Cannot find the portable launcher: %v", err)
		}
		root := filepath.Dir(self)
		cfg.dir = filepath.Join(root, "data")
		cfg.payloadDir = filepath.Join(root, "payload")
		// WHP is a property of this Windows host, so its restart marker must
		// not travel to another PC with the USB.
		cfg.hostDir = filepath.Join(os.Getenv("LOCALAPPDATA"), "TryOmarchy", "portable-host")
	} else {
		cfg.hostDir = cfg.dir
		if *applyLauncherUpdateFlag || *applyLauncherRollbackFlag {
			if err := applyLauncherUpdate(cfg.dir, *updateWaitPID, *updateRestartArgs, *applyLauncherRollbackFlag); err != nil {
				errorBox("Try Omarchy could not finish applying its update.\n\n" + err.Error())
				os.Exit(1)
			}
			return
		}
		restartArgs, err := encodeRestartArgs(os.Args[1:])
		if err != nil {
			fatal("Could not preserve launcher arguments for updates: %v", err)
		}
		if rollingBack, recoverErr := recoverLauncherUpdate(cfg.dir, restartArgs); recoverErr != nil {
			logf("launcher update recovery: %v", recoverErr)
		} else if rollingBack {
			return
		}
	}

	// Runs before settings load on purpose: a damaged settings.json is one of
	// the things a bug report needs to carry.
	if *diagnostics {
		bundle, err := writeDiagnostics(cfg.dir, launcherFacts(cfg))
		if err != nil {
			errorBox("Try Omarchy could not write the diagnostics bundle.\n\n" + err.Error())
			os.Exit(1)
		}
		infoBox("Diagnostics written to:\n\n" + bundle + "\n\nIt contains redacted settings, recent logs, and machine facts, but no disk images or home-folder files. Review it before attaching it to an issue because logs can still contain local details.")
		return
	}

	if *openSettings {
		if runSettingsDialog(settingsPath(cfg.dir)) {
			logf("settings saved to %s", settingsPath(cfg.dir))
		}
		return
	}

	// settings.json holds the rows the settings window edits; explicit flags
	// win for this launch only.
	explicitFlags := map[string]bool{}
	flag.Visit(func(f *flag.Flag) { explicitFlags[f.Name] = true })
	userSettings, err := loadSettings(settingsPath(cfg.dir))
	if err != nil {
		fatal("Try Omarchy cannot read its settings: %v\n\nFix or delete the file and open Try Omarchy again.", err)
	}
	if err := applySettings(cfg, userSettings, explicitFlags, &forwards, sshKeyPath); err != nil {
		fatal("Try Omarchy cannot use its settings: %v", err)
	}
	if cfg.memOverrideMiB != 0 && (cfg.memOverrideMiB < minimumGuestMemoryMiB || cfg.memOverrideMiB > maximumGuestMemoryMiB) {
		fatal("-memory must be between %d and %d MiB.", minimumGuestMemoryMiB, maximumGuestMemoryMiB)
	}
	home, _ := os.UserHomeDir()
	sshKey, err := resolveSSHPreset(&forwards, *sshPort, *sshKeyPath, home, explicitFlags["ssh-key"])
	if err != nil {
		fatal("%v.", err)
	}
	cfg.forwards = forwards
	cfg.sshKey = sshKey
	if sshRequested(cfg.forwards) && cfg.sshKey == "" {
		logf("ssh requested without a public key - password login only")
	}

	cfg.guestDir = filepath.Join(cfg.dir, "guest")
	cfg.vmDir = filepath.Join(cfg.dir, "vm")
	cfg.diskFormat = "raw"
	cfg.disk = filepath.Join(cfg.vmDir, "disk.raw")
	if cfg.portable {
		cfg.diskFormat = "qcow2"
		cfg.disk = filepath.Join(cfg.vmDir, "disk.qcow2")
	}
	payloadsRolledBack, err := rollbackPendingPayloadUpdates(cfg.dir)
	if err != nil {
		fatal("Could not recover the previous Omarchy files after an interrupted update: %v", err)
	}
	if payloadsRolledBack {
		if err := pinRestoredPayloads(cfg.dir, release, sumsSHA256, runtimeRelease, runtimeSumsSHA256); err != nil {
			fatal("Could not use the restored Omarchy files: %v", err)
		}
		logf("using restored guest and runtime for this recovery launch")
	}
	completeAtStart := completeInstallExists(cfg.dir, filepath.Base(cfg.disk))
	needsProvisioning := cfg.fresh || !completeAtStart
	configureSetupCancellation(!completeAtStart)
	if err := os.MkdirAll(cfg.vmDir, 0o755); err != nil {
		fatal("Could not create the Omarchy data directory: %v", err)
	}
	if err := os.MkdirAll(cfg.hostDir, 0o755); err != nil {
		fatal("Could not create the Windows host-state directory: %v", err)
	}
	logFile, _ = os.OpenFile(filepath.Join(cfg.vmDir, "shell.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if logFile != nil {
		// A windowsgui process has no console: an unhandled panic (any
		// goroutine) writes its trace to stderr and vanishes. It happened - the
		// shell died silently mid-session leaving QEMU orphaned. Route stderr
		// into the log so the next death has a trace.
		os.Stderr = logFile
	}
	logf("---- %s starting ----", appTitle)

	// Single-instance guard FIRST: binding the lifecycle port before the image
	// fetch stops a double-click double-launch from downloading the same 1.4 GB
	// into the same files twice (it happened).
	runLifecycleListener()

	// The splash IS the launch experience: it appears here and stays on screen
	// through every phase until the Omarchy window itself is visible (the
	// title enforcer closes it). Setup must never look like nothing happened.
	getUI().setStatus("Starting Try Omarchy...")
	// Per-run stderr: the memory ladder sniffs this file, stale errors from a
	// previous run must not be mistaken for this one's.
	os.Remove(filepath.Join(cfg.vmDir, "qemu-stderr.log"))

	if automaticUpdatesEnabled(cfg, *noUpdate, *release, *sumsSHA256) {
		checkDue := *updateURL != defaultUpdateURL || updateCheckDue(cfg.dir, time.Now())
		if checkDue {
			_ = recordUpdateCheck(cfg.dir, time.Now())
			if updating, updateErr := maybeStartLauncherUpdate(cfg, *updateURL, os.Args[1:]); updateErr != nil {
				logf("update check skipped: %v", updateErr)
			} else if updating {
				logf("starting authenticated launcher update")
				uiDone()
				if logFile != nil {
					logFile.Close()
				}
				return
			}
		}
	}

	// Machine setup the old bootstrap.ps1 handled: hypervisor on (may walk the
	// user through one restart and exit), then a QEMU to run. Existing setups
	// win - C:\WINQ-EMU, then a previously downloaded runtime, then stock QEMU
	// from the bootstrap; a bare machine downloads the portable WINQ-EMU tree.
	ensureWHP(cfg)
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}
	chooseProvisionMode(cfg, needsProvisioning)

	const qemuExe = "qemu-system-x86_64w.exe"
	stockQemu := `C:\Program Files\qemu\` + qemuExe
	_, stockErr := os.Stat(stockQemu)
	haveStock := stockErr == nil && !cfg.portable
	gpuRoot := ""
	if !cfg.portable {
		_, err := os.Stat(filepath.Join(cfg.winqEmu, "bin", qemuExe))
		if err == nil {
			// A user-managed WINQ-EMU install stays under the user's control. Only
			// the bundled runtime under cfg.dir participates in automatic updates.
			gpuRoot = cfg.winqEmu
		}
	}
	if gpuRoot == "" && !(cfg.noGpu && haveStock) {
		if payloadsRolledBack {
			root := filepath.Join(cfg.dir, "runtime")
			info, err := os.Stat(filepath.Join(root, "bin", qemuExe))
			if err != nil || !info.Mode().IsRegular() {
				fatal("The restored graphics engine is incomplete. Reinstall Try Omarchy or use a working stock QEMU installation.")
			}
			gpuRoot = root
		} else {
			root, err := ensureRuntime(cfg, *runtimeRelease, *runtimeSumsSHA256)
			if err != nil {
				if finishSetupCancellation(cfg, err) {
					return
				}
				logf("runtime setup failed: %v", err)
				if !haveStock {
					if cfg.portable {
						fatal("Setting up the portable graphics engine failed: %v\n\nThe USB payload may be missing or damaged.", err)
					}
					fatal("Downloading the graphics engine failed: %v\n\n%s", err, setupFailureHelp(err))
				}
			} else {
				gpuRoot = root
			}
		}
	}
	if gpuRoot != "" {
		cfg.qemu = filepath.Join(gpuRoot, "bin", qemuExe)
		cfg.useGpu = !cfg.noGpu
	} else {
		cfg.qemu = stockQemu
	}

	// First run: fetch the guest image, or copy and unpack the authenticated
	// local payload. Portable mode never falls back to the network.
	if payloadsRolledBack {
		ready, err := installReceiptMatches(cfg.guestDir, *release, *sumsSHA256, installedGuestArtifacts)
		if err != nil || !ready {
			fatal("The restored Omarchy image is incomplete. Reinstall Try Omarchy to recover it.")
		}
	} else {
		if err := ensureGuest(cfg, *release, *sumsSHA256); err != nil {
			if finishSetupCancellation(cfg, err) {
				return
			}
			if cfg.portable {
				fatal("Setting up portable Omarchy failed: %v\n\nThe USB payload may be missing or damaged.", err)
			}
			fatal("Setting up the Omarchy image failed: %v\n\n%s", err, setupFailureHelp(err))
		}
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
	if cfg.instant {
		cmdline += " tryomarchy.instant=1"
	}
	cmdline += sshCmdline(cfg.forwards, cfg.sshKey)
	cmdline += shareCmdline(cfg.share)

	if err := prepareDisk(cfg, spec.Runtime.Storage.ExpandedSizeMiB); err != nil {
		if finishSetupCancellation(cfg, err) {
			return
		}
		fatal("Preparing the writable disk failed: %v", err)
	}
	// From here onward the installation is complete. A last-second cancel may
	// stop this launch, but must not remove the working VM it just finished.
	cancelRemovesAll.Store(false)
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}
	// A shortcut to a copied portable executable would lose its sibling
	// payload and defeat portability. The USB launcher remains the entrypoint.
	if !cfg.portable {
		offerLauncherShortcuts(cfg.dir)
	}
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}
	cfg.memMiB = pickGuestMem(cfg.useGpu)
	if cfg.memOverrideMiB != 0 {
		// The user's choice stands; the startup memory ladder still halves it
		// if Windows cannot actually provide that much.
		cfg.memMiB = cfg.memOverrideMiB
	}
	getUI().setStatus("Starting Omarchy...")

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
	go runCursorReleaseGuard()
	go runCloseGuard()
	runClipboardBridge()

	cfg.audio = "dsound"

	for relaunch := true; relaunch; {
		relaunch = supervise(cfg, cmdline)
	}
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
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
func supervise(cfg *config, cmdline string) bool {
	var proc *exec.Cmd
	var qmp *qmpConn
	// The worst case can consume one attempt each for nested virtualization,
	// audio, runtime rollback, and GPU fallback before walking 64 GiB down to a
	// final 1 GiB memory attempt. Keep a small margin without allowing a loop.
	const maxLaunchAttempts = 12
	for attempt := 1; attempt <= maxLaunchAttempts; attempt++ {
		if setupCancelled() {
			return false
		}
		mode := "CPU rendering (llvmpipe)"
		if cfg.useGpu {
			mode = "GPU accelerated (virgl + Venus Vulkan)"
		}
		logf("booting - %s (attempt %d)", mode, attempt)
		pendingReboot.Store(false)
		guestReady.Store(false)
		proc = exec.Command(cfg.qemu, buildQemuArgs(cfg, cmdline)...)
		// The w-binary's startup errors (bad args, SDL init) only ever reach
		// stderr; without this they vanish and a dead QEMU is undebuggable.
		if ef, err := os.OpenFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"),
			os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644); err == nil { // per-attempt: the memory ladder sniffs it
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
			case <-setupCancelWake:
				proc.Process.Kill()
				<-exited
				qemuPid.Store(0)
				return false
			case <-exited:
				startupDead = true
				// The host refused nested virtualization for the partition
				// (issue #19). Nothing else about the launch is wrong, so
				// retry with the irqchip in QEMU, which never asks for it.
				if nestedVirtRefused(cfg) {
					if !cfg.irqchipOff {
						logf("QEMU exited at startup - host refused nested virtualization, retrying with kernel-irqchip=off")
						cfg.irqchipOff = true
						break probe
					}
					fatal("Windows refused to start the virtual machine: the hypervisor does not allow nested virtualization on this PC.\n\nThis is a known problem with some Intel Core Ultra laptops and machines running the full Hyper-V feature set. Details are in %s\\qemu-stderr.log.", cfg.vmDir)
				}
				// No DirectSound device (VMs, some remote sessions) kills
				// QEMU at startup; retry silent rather than dying.
				if cfg.audio == "dsound" && audioUnavailable(cfg) {
					logf("QEMU exited at startup - retrying without audio")
					cfg.audio = "none"
					break probe
				}
				// Not enough free memory: step the guest down before giving
				// up - it should launch with whatever the machine can spare.
				if memoryStarved(cfg) {
					if cfg.memMiB > 1024 {
						cfg.memMiB = cfg.memMiB / 2
						if cfg.memMiB < 1024 {
							cfg.memMiB = 1024
						}
						logf("QEMU exited at startup - low memory, retrying with %d MiB", cfg.memMiB)
						break probe
					}
					fatal("There isn't enough free memory to start Omarchy right now.\n\nClose some apps and open Try Omarchy again.")
				}
				// Broken host GL (remote sessions, ancient drivers) kills the
				// gl=on display the same way; same binary, CPU args, still up.
				if cfg.useGpu {
					if rolledBack, rollbackErr := rollbackPendingRuntimeUpdate(cfg.dir); rollbackErr != nil {
						logf("runtime update rollback failed: %v", rollbackErr)
					} else if rolledBack {
						logf("updated runtime failed to start - restored previous runtime")
						break probe
					}
					logf("QEMU exited at startup - falling back to CPU rendering")
					cfg.useGpu = false
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
			// QMP answers while the guest is kernel-panicked, and a graphics
			// runtime can start QMP before failing later in guest boot. Keep all
			// update components rollback-capable until the in-guest readiness
			// service reaches userspace and networking.
			defer qmp.close()
			return watch(cfg, qmp, exited)
		}
		qemuPid.Store(0)
		if !startupDead {
			logf("QEMU is not answering (known WHPX launch wedge) - killing and retrying")
			proc.Process.Kill()
			<-exited
		}
		if sleepDuringSetup(2*time.Second) != nil {
			return false
		}
	}
	if setupCancelled() {
		return false
	}
	fatal("QEMU failed to come up healthy after %d attempts.", maxLaunchAttempts)
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
		if guestReady.Swap(false) {
			commitLauncherUpdate(cfg.dir)
			commitPayloadUpdates(cfg.dir)
		}
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

var (
	pendingReboot atomic.Bool
	guestReady    atomic.Bool
)

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
				line, err := bufio.NewReader(io.LimitReader(c, 64)).ReadString('\n')
				if err != nil {
					return
				}
				switch strings.TrimSpace(line) {
				case "reboot":
					logf("guest announced reboot")
					pendingReboot.Store(true)
				case "ready":
					logf("guest userspace announced ready")
					guestReady.Store(true)
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
