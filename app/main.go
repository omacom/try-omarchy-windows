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
	"runtime"
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
	supportsSharing          bool
	audio                    string
	memMiB                   int
	// kernel-irqchip=off keeps WHPX from requesting nested virtualization,
	// which some hosts advertise and then refuse (issue #19). Set by the
	// startup retry, never by a flag.
	forwards []portForward
	sshKey   string
	// Guest RAM chosen by the user (settings.json or -memory); 0 = automatic.
	memOverrideMiB int
	diskGiB        int
	irqchipOff     bool
	// Guest vCPUs chosen by the user (settings.json or -cpus); 0 = automatic.
	cpuOverride  int
	cpus         int
	hostTotalMiB int
	// Rendering decision inputs, see render_probe.go.
	renderMode    string
	runtimeID     string
	displayDriver string
}

// pickGuestMem sizes the guest RAM to this machine; see resources.go.
func pickGuestMem(gpu bool) int {
	total, avail := availMemMiB()
	return pickGuestMemMiB(gpu, total, avail)
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

// earlyLog holds lines written before shell.log is opened (update recovery,
// settings, the restored-payload decision) so they land at the top of the
// session's log instead of vanishing.
var earlyLog []string

func logf(format string, a ...any) {
	line := fmt.Sprintf("%s %s", time.Now().Format("15:04:05"), fmt.Sprintf(format, a...))
	if logFile != nil {
		fmt.Fprintln(logFile, line)
	} else if len(earlyLog) < 200 {
		earlyLog = append(earlyLog, line)
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
		errorBox(fmt.Sprintf("Setup was cancelled, but some temporary files could not be removed:\n\n%v\n\nOmarchy data folder: %s\n\nKeep this folder. Close Try Omarchy and try again.", cleanupErr, cfg.dir))
	}
	uiDone()
	return true
}

func main() {
	cfg := &config{}
	removeDataOnCancel := false
	defaultDir := filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName)
	flag.StringVar(&cfg.dir, "dir", defaultDir, "Try Omarchy data directory (virtual machine, runtime, and settings)")
	flag.StringVar(&cfg.winqEmu, "winq", `C:\WINQ-EMU`, "WINQ-EMU install path (GPU mode)")
	flag.StringVar(&cfg.share, "share", "", "Windows folder shared into Omarchy at /mnt/host and as ~/<folder name>")
	flag.BoolVar(&cfg.fresh, "fresh", false, "start over and retain the previous writable disk for recovery")
	flag.BoolVar(&cfg.fullscreen, "fullscreen", false, "start fullscreen (Immersive)")
	flag.IntVar(&cfg.memOverrideMiB, "memory", 0, "guest RAM in MiB (default: sized to this PC)")
	flag.IntVar(&cfg.cpuOverride, "cpus", 0, "guest CPUs (default: sized to this PC)")
	flag.IntVar(&cfg.diskGiB, "disk-size", 0, "guest disk capacity in GiB (0: default; grows existing standard disks, never shrinks)")
	flag.BoolVar(&cfg.noGpu, "nogpu", false, "force CPU rendering even if WINQ-EMU is installed (same as -render cpu)")
	renderFlag := flag.String("render", "", "rendering path: auto (default), gpu, or cpu")
	timeZoneFlag := flag.String("timezone", "", "guest time zone: blank follows Windows, keep leaves the guest alone, or an IANA name such as Europe/Berlin")
	keyboardFlag := flag.String("keyboard", "", "guest keyboard layout: blank follows Windows, keep leaves the guest alone, or an XKB layout such as de or us:intl")
	flag.BoolVar(&cfg.hostCursor, "host-cursor", false, "force the legacy Windows cursor over the guest")
	flag.BoolVar(&cfg.instant, "instant", false, "skip first-boot questions and use the trial account")
	flag.BoolVar(&cfg.portable, "portable", false, "run entirely from data and payload folders beside the executable")
	var forwards forwardList
	flag.Var(&forwards, "forward", "forward a Windows loopback port into Omarchy, as tcp:2222:22 or 8080:80 (repeatable)")
	sshPort := flag.Int("ssh", 0, "forward this Windows loopback port to Omarchy's sshd and start sshd for the session")
	recoveryAction := flag.String("recovery", "", "open backup, restore, reset, or uninstall controls for a stopped standard install")
	uninstall := flag.Bool("uninstall", false, "remove this Try Omarchy installation: shortcuts, the Apps & features entry, and the data folder")
	uninstallFinish := flag.Bool("uninstall-finish", false, "internal: delete the data folder after the launcher inside it exits")
	backupPath := flag.String("backup", "", "back up a stopped standard VM to a new ZIP file, then exit")
	restorePath := flag.String("restore", "", "restore a trusted backup into a new folder selected with -dir, then exit")
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
	if *uninstall {
		if *recoveryAction != "" && *recoveryAction != "uninstall" {
			fatal("Choose one recovery action: backup, restore, reset, or uninstall.")
		}
		*recoveryAction = "uninstall"
	}
	maintenance := *backupPath != "" || *restorePath != "" || *recoveryAction != ""
	if *recoveryAction != "" && (*recoveryAction != "backup" && *recoveryAction != "restore" && *recoveryAction != "reset" && *recoveryAction != "uninstall" || *backupPath != "" || *restorePath != "") {
		fatal("Choose one recovery action: backup, restore, or reset.")
	}
	if maintenance && (*backupPath != "" && *restorePath != "" || cfg.portable || cfg.fresh || *openSettings || *diagnostics || *enableWhp || *applyLauncherUpdateFlag || *applyLauncherRollbackFlag) {
		fatal("Use one recovery action on a stopped standard install, without other maintenance options.")
	}
	explicitFlags := map[string]bool{}
	flag.Visit(func(f *flag.Flag) { explicitFlags[f.Name] = true })
	if explicitFlags["recovery"] && *recoveryAction == "" {
		fatal("Choose a recovery action: backup, restore, or reset.")
	}
	if explicitFlags["backup"] && strings.TrimSpace(*backupPath) == "" || explicitFlags["restore"] && strings.TrimSpace(*restorePath) == "" {
		fatal("Provide a backup filename with -backup or -restore.")
	}
	if *restorePath != "" && !explicitFlags["dir"] {
		fatal("Use -dir with a new data folder when restoring. Existing installations are never replaced.")
	}
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
	if *uninstallFinish {
		if !explicitFlags["dir"] {
			os.Exit(2)
		}
		os.Exit(finishUninstall(cfg.dir, *updateWaitPID))
	}
	// Bind before the first-run location prompt. Two quick launches must not
	// race each other through the folder choice or write the same pointer and
	// payload files. Settings, diagnostics, and update helpers remain usable
	// while the VM owns the lifecycle port.
	if !*openSettings && !*diagnostics && !*applyLauncherUpdateFlag && !*applyLauncherRollbackFlag {
		runLifecycleListener()
	}
	if cfg.portable {
		self, err := os.Executable()
		if err != nil {
			fatal("Cannot find the portable launcher: %v", err)
		}
		root := filepath.Dir(self)
		cfg.dir = filepath.Join(root, "data")
		cfg.payloadDir = filepath.Join(root, "payload")
		removeDataOnCancel, err = dataDirectoryEmpty(cfg.dir)
		if err != nil {
			fatal("Try Omarchy cannot inspect its portable data location: %v", err)
		}
		// WHP is a property of this Windows host, so its restart marker must
		// not travel to another PC with the USB.
		cfg.hostDir = filepath.Join(os.Getenv("LOCALAPPDATA"), "TryOmarchy", "portable-host")
	} else {
		promptForLocation := !maintenance && !*diagnostics && !*applyLauncherUpdateFlag && !*applyLauncherRollbackFlag
		selected, proceed, err := resolveStandardDataDirectory(
			defaultDir, cfg.dir, explicitFlags["dir"], promptForLocation, chooseFirstRunDataDirectory,
		)
		if err != nil {
			fatal("Try Omarchy cannot resolve its data location: %v\n\nIf a saved location is damaged, fix or delete %s, then open Try Omarchy again.", err, dataLocationPointerPath(defaultDir))
		}
		if !proceed {
			return
		}
		if !explicitFlags["dir"] && !pathsEqual(selected, defaultDir) {
			if err := validateStandardDataDrive(selected); err != nil {
				fatal("The saved Try Omarchy data location is unavailable or incompatible:\n\n%s\n\n%v\n\nReconnect the drive or delete %s to choose another location.", selected, err, dataLocationPointerPath(defaultDir))
			}
		}
		cfg.dir = selected
		cfg.hostDir = cfg.dir
		removeDataOnCancel, err = dataDirectoryEmpty(cfg.dir)
		if err != nil {
			fatal("Try Omarchy cannot inspect its data location: %v", err)
		}
		if *applyLauncherUpdateFlag || *applyLauncherRollbackFlag {
			if err := applyLauncherUpdate(cfg.dir, *updateWaitPID, *updateRestartArgs, *applyLauncherRollbackFlag); err != nil {
				errorBox("Try Omarchy could not finish applying its update.\n\n" + err.Error())
				os.Exit(1)
			}
			return
		}
		// Settings and diagnostics may be opened from the running app's tray.
		// They must not inspect or roll back an update owned by that parent.
		if !maintenance && !*openSettings && !*diagnostics {
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
	}

	if *recoveryAction != "" {
		err := runRecoveryUI(cfg.dir, *recoveryAction)
		reportRecoveryResult(err)
		if err != nil && !errors.Is(err, errSetupCancelled) {
			os.Exit(1)
		}
		return
	}
	if maintenance {
		var err error
		if *backupPath != "" {
			beginRecoveryProgress("Creating VM backup. This may take a while...")
			err = writeVMBackupProgress(cfg.dir, *backupPath, recoveryProgress("Backing up"))
		} else {
			beginRecoveryProgress("Verifying and restoring VM backup...")
			err = restoreVMBackupProgress(*restorePath, cfg.dir, recoveryProgress("Restoring"))
		}
		uiDone()
		if errors.Is(err, errSetupCancelled) {
			return
		}
		if err != nil {
			errorBox("Try Omarchy could not finish the backup or restore.\n\n" + err.Error())
			os.Exit(1)
		}
		if *backupPath != "" {
			infoBox("Backup saved to:\n\n" + *backupPath + "\n\nIt contains your guest files and settings. Keep it private. Shared Windows folders are not included.")
		} else {
			infoBox("Backup restored to:\n\n" + cfg.dir + "\n\nStart Try Omarchy with -dir pointing to this folder. Your original installation was not changed.")
		}
		return
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
		if runSettingsDialog(settingsPath(cfg.dir), cfg.dir, cfg.portable) {
			logf("settings saved to %s", settingsPath(cfg.dir))
		}
		return
	}

	// settings.json holds the rows the settings window edits; explicit flags
	// win for this launch only.
	settingsFile := settingsPath(cfg.dir)
	userSettings, err := loadSettingsWithRepair(settingsFile)
	if err != nil {
		if errors.Is(err, errSetupCancelled) {
			uiDone()
			return
		}
		fatal("Try Omarchy cannot read its settings: %v\n\nFix or delete the file and open Try Omarchy again.", err)
	}
	if err := applySettings(cfg, userSettings, explicitFlags, &forwards, sshKeyPath); err != nil {
		fatal("Try Omarchy cannot use its settings: %v", err)
	}
	if explicitFlags["render"] {
		mode, err := parseRenderMode(*renderFlag)
		if err != nil {
			fatal("%v", err)
		}
		cfg.renderMode = mode
	}
	if cfg.noGpu {
		cfg.renderMode = renderCPU
	}
	cfg.noGpu = cfg.renderMode == renderCPU
	if cfg.memOverrideMiB != 0 && (cfg.memOverrideMiB < minimumGuestMemoryMiB || cfg.memOverrideMiB > maximumGuestMemoryMiB) {
		fatal("-memory must be between %d and %d MiB.", minimumGuestMemoryMiB, maximumGuestMemoryMiB)
	}
	if !explicitFlags["disk-size"] {
		storage, err := loadStorageWithRepair(cfg.dir)
		if err != nil {
			if errors.Is(err, errSetupCancelled) {
				uiDone()
				return
			}
			fatal("Cannot read storage preferences: %v", err)
		}
		cfg.diskGiB = storage.DiskGiB
	}
	if _, err := requestedDiskMiB(24*1024, cfg.diskGiB, cfg.portable); err != nil {
		fatal("%v", err)
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
	if cfg.fresh && !cfg.portable {
		if _, err := os.Lstat(cfg.disk); err == nil {
			proceed, err := confirmResetBackup(cfg.dir)
			if err != nil || !proceed {
				reportRecoveryResult(err)
				return
			}
		}
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
	configureSetupCancellation(!completeAtStart && removeDataOnCancel)
	if err := os.MkdirAll(cfg.vmDir, 0o755); err != nil {
		fatal("Could not create the Omarchy data directory: %v", err)
	}
	if err := os.MkdirAll(cfg.hostDir, 0o755); err != nil {
		fatal("Could not create the Windows host-state directory: %v", err)
	}
	logFile, _ = os.OpenFile(filepath.Join(cfg.vmDir, "shell.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if logFile != nil {
		for _, line := range earlyLog {
			fmt.Fprintln(logFile, line)
		}
		earlyLog = nil
		// A windowsgui process has no console: an unhandled panic (any
		// goroutine) writes its trace to stderr and vanishes. It happened - the
		// shell died silently mid-session leaving QEMU orphaned. Route stderr
		// into the log so the next death has a trace.
		os.Stderr = logFile
	}
	logf("---- %s starting ----", appTitle)

	// The splash IS the launch experience: it appears here and stays on screen
	// through every phase until the Omarchy window itself is visible (the
	// title enforcer closes it). Setup must never look like nothing happened.
	getUI().setStatus("Starting Try Omarchy...")
	if err := configureRecommendedSharedFolder(cfg, &userSettings, settingsFile, home, explicitFlags["share"]); err != nil {
		if finishSetupCancellation(cfg, err) {
			return
		}
		fatal("Could not set up the recommended shared folder: %v", err)
	}
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}
	if cfg.share != "" {
		validated, shareErr := validateWindowsSharedFolder(cfg.share, cfg.dir, home)
		if shareErr != nil {
			if explicitFlags["share"] {
				fatal("Cannot share %s: %v", cfg.share, shareErr)
			}
			logf("shared folder disabled for this launch: %v", shareErr)
			infoBox("The saved shared folder is unavailable and will not be shared this time. Omarchy will still start.\n\n" + shareErr.Error() + "\n\nChoose another folder from Settings.")
			cfg.share = ""
		} else {
			cfg.share = validated
		}
	}
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
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}

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
	if gpuRoot == "" && !(cfg.noGpu && haveStock && cfg.share == "") {
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
		cfg.supportsSharing = true
		cfg.runtimeID = runtimeIdentity(gpuRoot)
		cfg.displayDriver = displayDriverIdentity()
		probe, err := loadRenderProbe(cfg.dir)
		if err != nil {
			logf("ignoring %s: %v", renderProbeFilename, err)
		}
		var reason string
		cfg.useGpu, reason = startWithGPU(cfg.renderMode, probe, cfg.runtimeID, cfg.displayDriver, time.Now())
		if reason != "" {
			logf("rendering: %s", reason)
		}
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
	if cfg.share != "" && !cfg.supportsSharing {
		logf("shared folder disabled for this launch: selected QEMU has no virtio-9p")
		infoBox("The shared folder cannot be attached with the available graphics engine. Omarchy will start without it this time.\n\nTry again when the WINQ-EMU runtime is available.")
		cfg.share = ""
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
	zone, layout, variant := hostLocale(*timeZoneFlag, *keyboardFlag)
	if words := hostLocaleCmdline(zone, layout, variant); words != "" {
		cmdline += words
		logf("guest follows Windows locale:%s", words)
	}

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
	cfg.hostTotalMiB, _ = availMemMiB()
	cfg.cpus = pickGuestCPUs(runtime.NumCPU())
	if cfg.cpuOverride != 0 {
		cfg.cpus = cfg.cpuOverride
	}
	logf("resources: %d of %d logical processors, %d MiB guest RAM", cfg.cpus, runtime.NumCPU(), cfg.memMiB)
	getUI().setStatus("Starting Omarchy...")
	stopTray := startTray(cfg)
	defer stopTray()

	// SDL's keyboard grab installs a system-wide Win-key hook that leaks past
	// window focus; our hook does it right (focus-scoped).
	os.Setenv("SDL_GRAB_KEYBOARD", "0")
	// Launch-UX contract (NOTES.md): guest console sized to the window it will
	// actually get, so the picture fills it from the first frame.
	conW, conH := screenSize(cfg.fullscreen)
	if !cfg.fullscreen {
		if p := rememberedWindow(cfg.dir); p != nil && !p.Maximized {
			conW, conH = p.consoleSize()
		}
	}
	cmdline += fmt.Sprintf(" video=%dx%d", conW, conH)

	go runWinKeyHook()
	go runWinKeyQmp()
	go runTitleEnforcer(cfg.dir, cfg.fullscreen)
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
					if probe, _ := loadRenderProbe(cfg.dir); keepUpdatedRuntimeOnCPU(probe) {
						// GPU mode never ran here, so the failure is the
						// machine's graphics stack, not the new runtime: keep
						// the update and let the CPU boot commit it.
						logf("QEMU exited at startup - GPU rendering also failed before the runtime update, keeping the updated runtime and falling back to CPU rendering")
						cfg.useGpu = false
						break probe
					}
					if rolledBack, rollbackErr := rollbackPendingRuntimeUpdate(cfg.dir); rollbackErr != nil {
						logf("runtime update rollback failed: %v", rollbackErr)
					} else if rolledBack {
						logf("updated runtime failed to start - restored previous runtime")
						// The probe record must describe the runtime that
						// actually boots, which is the restored one now.
						cfg.runtimeID = runtimeIdentity(filepath.Dir(filepath.Dir(cfg.qemu)))
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
			recordRenderResult(cfg)
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

// recordRenderResult remembers which rendering path reached userspace with
// the current runtime and drivers, so the next launch can skip attempts that
// this machine cannot pass. A CPU result written while GPU was never tried
// (settings say CPU) must not later be mistaken for a probe failure, so only
// automatic and forced-GPU launches record CPU.
func recordRenderResult(cfg *config) {
	if cfg.runtimeID == "" || (cfg.renderMode == renderCPU && !cfg.useGpu) {
		return
	}
	result := renderCPU
	if cfg.useGpu {
		result = renderGPU
	}
	probe := renderProbe{Result: result, RuntimeID: cfg.runtimeID, DisplayDriver: cfg.displayDriver, RecordedAt: time.Now()}
	if err := saveRenderProbe(cfg.dir, probe); err != nil {
		logf("could not record the rendering result: %v", err)
	}
}
