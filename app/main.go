//go:build windows

package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

// Try Omarchy for Windows - the native app shell. One exe replacing
// launch-omarchy.ps1 + winkey-forwarder.ps1 + clipboard-bridge.ps1:
// launches QEMU (WINQ-EMU GPU stack when installed, stock CPU fallback),
// supervises it through WHPX's rough edges, scopes the Windows key to the VM
// window, keeps the window branded, and bridges the clipboard. The native
// launcher edits settings before handing off to the SDL guest window.

const appTitle = "Try Omarchy"

type config struct {
	desktop                     desktopPreferences
	audioDevices                audioPreferences
	audioRates                  audioSampleRates
	dir, hostDir, payloadDir    string
	winqEmu, share              string
	fresh, fullscreen, noGpu    bool
	resetPayloadPrepared        bool
	factoryVerifiedThisRun      bool
	fullscreenDisplay           string
	hostCursor                  bool
	experimentalPinch           bool
	disablePinch, guestPinch    bool
	lanPublic                   bool
	instant, portable           bool
	localPayload                bool
	localPayloadSHA256          string
	localRuntimePayloadSHA256   string
	guestDir, vmDir, disk       string
	qmpDir                      string
	followHostTimeZone          bool
	diskFormat                  string
	qemu                        string
	runtimeLoader               runtimeLoaderPreflight
	useGpu                      bool
	supportsSharing             bool
	audio                       string
	memMiB                      int
	displays                    int
	displayWidth, displayHeight int
	// kernel-irqchip=off keeps WHPX from requesting nested virtualization,
	// which some hosts advertise and then refuse (issue #19). Set by the
	// startup retry, never by a flag.
	forwards []portForward
	// launchForwards is the list from this launch; forwards follows live
	// changes from Settings between boots (forward_live.go).
	launchForwards []portForward
	sshKey         string
	// Guest RAM chosen by the user (settings.json or -memory); 0 = automatic.
	memOverrideMiB int
	diskGiB        int
	irqchipOff     bool
	// Guest vCPUs chosen by the user (settings.json or -cpus); 0 = automatic.
	cpuOverride  int
	cpus         int
	hostTotalMiB int
	// Rendering decision inputs, see render_probe.go.
	renderMode     string
	runtimeID      string
	displayDriver  string
	temporaryCPU   bool
	recoveryChoice *gpuRecoveryDecision
	// venus offers guest Vulkan on the host GPU in GPU mode (venus.go).
	venus bool
}

// memoryStarved reports whether the current attempt's QEMU died because the
// guest RAM couldn't be allocated (stderr is truncated per attempt).
func memoryStarved(cfg *config) bool {
	data, err := os.ReadFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"))
	return err == nil && bytes.Contains(data, []byte("cannot set up guest memory"))
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

// fatal shows a finished message, already in the launcher's language, and
// exits. Messages come from the catalog: uiText or uiTextWith.
func fatal(msg string) {
	logf("FATAL %s", msg)
	uiDone()
	errorBox(msg)
	os.Exit(1)
}

func finishSetupCancellation(cfg *config, err error) bool {
	if !setupCancelled() && !errors.Is(err, errSetupCancelled) {
		return false
	}
	getUI().setStatus("%s", uiText("status.cancelling"))
	logf("setup cancelled by user")
	if logFile != nil {
		logFile.Close()
		logFile = nil
	}
	executable, _ := os.Executable()
	if cleanupErr := cleanupCancelledSetup(cfg.dir, executable, cancelRemovesAll.Load()); cleanupErr != nil {
		errorBox(uiTextWith("setup.cancel.cleanup_failed", map[string]string{"error": cleanupErr.Error(), "path": cfg.dir}))
	}
	uiDone()
	return true
}

func main() {
	// Windows runs this for an allowed link. The link is untrusted text, so
	// it is never mixed with the ordinary flags; see parseURLActivation.
	if dir, url, activation, ok := parseURLActivation(os.Args[1:]); activation {
		if !ok {
			errorBox(uiText("control.open_url.usage"))
			os.Exit(2)
		}
		os.Exit(sendOpenURL(dir, url))
	}
	cfg := &config{}
	removeDataOnCancel := false
	defaultDir := filepath.Join(os.Getenv("LOCALAPPDATA"), defaultDataDirectoryName)
	flag.StringVar(&cfg.dir, "dir", defaultDir, "Try Omarchy data directory (virtual machine, runtime, and settings)")
	flag.StringVar(&cfg.winqEmu, "winq", `C:\WINQ-EMU`, "WINQ-EMU install path (GPU mode)")
	flag.StringVar(&cfg.share, "share", "", "Windows folder shared into Omarchy at /mnt/host and as ~/<folder name>")
	flag.BoolVar(&cfg.fresh, "fresh", false, "start over and retain the previous writable disk for recovery")
	flag.IntVar(&cfg.displays, "displays", 1, "number of guest displays (1 to 16)")
	flag.BoolVar(&cfg.fullscreen, "fullscreen", false, "start fullscreen (Immersive)")
	flag.StringVar(&cfg.fullscreenDisplay, "fullscreen-display", "", "Windows display device for the first fullscreen output (empty: primary)")
	flag.IntVar(&cfg.memOverrideMiB, "memory", 0, "guest RAM in MiB (default: sized to this PC)")
	flag.IntVar(&cfg.cpuOverride, "cpus", 0, "guest CPUs (default: sized to this PC)")
	resourceProfileFlag := flag.String("resource-profile", "", "resource preset: balanced, maximum-performance, or manual; -cpus and -memory override individual resources")
	flag.IntVar(&cfg.diskGiB, "disk-size", 0, "guest disk capacity in GiB (0: default; grows existing disks, never shrinks)")
	flag.BoolVar(&cfg.noGpu, "nogpu", false, "force CPU rendering even if WINQ-EMU is installed (same as -render cpu)")
	renderFlag := flag.String("render", "", "rendering path: auto (default), gpu, or cpu")
	timeZoneFlag := flag.String("timezone", "", "guest time zone: blank follows Windows, keep leaves the guest alone, or an IANA name such as Europe/Berlin")
	keyboardFlag := flag.String("keyboard", "", "guest keyboard layout: blank follows Windows, keep leaves the guest alone, or an XKB layout such as de or us:intl")
	localeFlag := flag.String("locale", "", "guest language: blank follows Windows, keep leaves the guest alone, or a locale such as de_DE")
	flag.BoolVar(&cfg.hostCursor, "host-cursor", false, "force the legacy Windows cursor over the guest")
	flag.BoolVar(&cfg.experimentalPinch, "experimental-pinch", false, "force Precision Touchpad pinch on for a guest configured by hand")
	flag.BoolVar(&cfg.disablePinch, "disable-pinch", false, "keep ordinary Windows two-finger input instead of forwarding touchpad pinch")
	flag.BoolVar(&cfg.instant, "instant", false, "skip first-boot questions and use the quick-start account (omarchy / omarchy)")
	flag.BoolVar(&cfg.portable, "portable", false, "run entirely from data and payload folders beside the executable")
	var forwards forwardList
	flag.Var(&forwards, "forward", "forward a Windows port into Omarchy: tcp:2222:22 (local), tcp:192.168.1.5:8080:80 (LAN); repeatable")
	firewallPlan := flag.String("firewall-plan", "", "internal: apply owned LAN firewall rules")
	flag.BoolVar(&cfg.lanPublic, "lan-public", false, "allow explicitly selected LAN forwards on public networks")
	sshPort := flag.Int("ssh", 0, "forward this Windows loopback port to Omarchy's sshd and start sshd for the session")
	openAbout := flag.Bool("about", false, "show version information and check for updates")
	usbSelection := flag.Bool("usb-selection", false, "internal: edit startup USB selection for an explicit data folder")
	openDevices := flag.Bool("devices", false, "manage USB devices in the running VM")
	recoveryAction := flag.String("recovery", "", "open backup, restore, snapshots, portable-create, reset, move, uninstall, or install-omarchy controls")
	uninstall := flag.Bool("uninstall", false, "remove this Try Omarchy installation: shortcuts, the Apps & features entry, and the data folder")
	uninstallFinish := flag.Bool("uninstall-finish", false, "internal: delete the data folder after the launcher inside it exits")
	reclaim := flag.Bool("reclaim", false, "ask the running Omarchy to zero its free space so the disk file shrinks after shutdown, then exit")
	backupPath := flag.String("backup", "", "back up a stopped standard VM to a new ZIP file, then exit")
	restorePath := flag.String("restore", "", "restore a trusted backup into a new folder selected with -dir, then exit")
	openSettings := flag.Bool("settings", false, "open the settings window, then exit")
	openLauncher := flag.Bool("launcher", false, "open the launcher before starting Omarchy")
	startImmediately := flag.Bool("start", false, "start Omarchy immediately without the launcher window")
	diagnostics := flag.Bool("diagnostics", false, "write a zip of logs, settings, and machine facts for a bug report, then exit")
	sshKeyPath := flag.String("ssh-key", "", "public key to authorize for the Omarchy account (default: your ~/.ssh/id_*.pub when -ssh is used)")
	noUpdate := flag.Bool("no-update", false, "do not check the feed for newer releases")
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
	disableFastStartupFlag := flag.Bool("disable-fast-startup", false, "internal: elevated helper that turns off Windows Fast Startup before installing Omarchy next to Windows")
	applyLauncherUpdateFlag := flag.Bool("apply-launcher-update", false, "internal: apply a staged launcher update")
	applyLauncherRollbackFlag := flag.Bool("apply-launcher-rollback", false, "internal: restore the previous launcher")
	restartWaitPID := flag.Int("restart-update-wait-pid", 0, "internal: wait for the stopped guest launcher before restarting")
	updateWaitPID := flag.Int("update-wait-pid", 0, "internal: process to wait for before replacing the launcher")
	updateRestartArgs := flag.String("update-restart-args", "", "internal: encoded launcher restart arguments")
	flag.Parse()
	if *restartWaitPID > 0 {
		waitForProcess(*restartWaitPID)
	}
	if *openAbout {
		runAbout()
		return
	}

	if *openDevices {
		if err := runUSBDeviceUI(); err != nil {
			fatal(uiTextWith("fatal.usb.open", map[string]string{"error": err.Error()}))
		}
		return
	}
	if *uninstall {
		if *recoveryAction != "" && *recoveryAction != "uninstall" {
			fatal(uiTextWith("fatal.cli.recovery_action", map[string]string{"actions": "backup, restore, reset, uninstall"}))
		}
		*recoveryAction = "uninstall"
	}
	maintenance := *backupPath != "" || *restorePath != "" || *recoveryAction != ""
	installWalkthrough := *recoveryAction == "install-omarchy"
	if *recoveryAction != "" && (*recoveryAction != "backup" && *recoveryAction != "restore" && *recoveryAction != "reset" && *recoveryAction != "uninstall" && *recoveryAction != "move" && *recoveryAction != "move-cleanup" && *recoveryAction != "snapshots" && *recoveryAction != "portable-create" && *recoveryAction != "install-omarchy" || *backupPath != "" || *restorePath != "") {
		fatal(uiTextWith("fatal.cli.recovery_action", map[string]string{"actions": "backup, restore, snapshots, portable-create, reset, move, uninstall, install-omarchy"}))
	}
	if maintenance && (*backupPath != "" && *restorePath != "" || cfg.portable && !portableRecoveryAllowed(*recoveryAction, *backupPath, *restorePath) || cfg.fresh || *openSettings || *diagnostics || *enableWhp || *disableFastStartupFlag || *applyLauncherUpdateFlag || *applyLauncherRollbackFlag) {
		fatal(uiText("fatal.cli.one_action"))
	}
	explicitFlags := map[string]bool{}
	flag.Visit(func(f *flag.Flag) { explicitFlags[f.Name] = true })
	if *usbSelection {
		if !explicitFlags["dir"] || cfg.portable || *openDevices || maintenance || *openSettings || *uninstall {
			fatal(uiText("fatal.cli.usb_selection"))
		}
		qemu := filepath.Join(cfg.dir, "runtime", "bin", "qemu-system-x86_64w.exe")
		external := filepath.Join(cfg.winqEmu, "bin", "qemu-system-x86_64w.exe")
		if info, e := os.Stat(external); e == nil && info.Mode().IsRegular() {
			qemu = external
		}
		if err := runUSBSelectionUI(cfg.dir, qemu); err != nil {
			fatal(uiTextWith("fatal.usb.choose", map[string]string{"error": err.Error()}))
		}
		return
	}
	if explicitFlags["recovery"] && *recoveryAction == "" {
		fatal(uiTextWith("fatal.cli.recovery_action_maintenance", map[string]string{"actions": "backup, restore, reset"}))
	}
	if explicitFlags["backup"] && strings.TrimSpace(*backupPath) == "" || explicitFlags["restore"] && strings.TrimSpace(*restorePath) == "" {
		fatal(uiText("fatal.cli.backup_filename"))
	}
	if *restorePath != "" && !explicitFlags["dir"] {
		fatal(uiText("fatal.cli.restore_dir"))
	}
	if strings.TrimSpace(*runtimeRelease) == "" {
		*runtimeRelease = *release
	}
	if strings.TrimSpace(*runtimeSumsSHA256) == "" {
		*runtimeSumsSHA256 = *sumsSHA256
	}
	launcherPins := pinnedPayloadUpdate{*release, *sumsSHA256, *runtimeRelease, *runtimeSumsSHA256}

	// The elevated relaunch does exactly one thing and reports back via exit
	// code (see setup.go); it must not touch the single-instance port.
	if *enableWhp {
		os.Exit(runDismEnable())
	}
	if *disableFastStartupFlag {
		runDisableFastStartupHelper()
	}
	if *firewallPlan != "" {
		if err := applyEncodedLANFirewall(*firewallPlan); err != nil {
			errorBox(uiTextWith("lan.error.firewall", map[string]string{"error": err.Error()}))
			os.Exit(1)
		}
		return
	}
	if *reclaim {
		os.Exit(sendLifecycleCommand("reclaim"))
	}
	if *uninstallFinish {
		if !explicitFlags["dir"] {
			os.Exit(2)
		}
		os.Exit(finishUninstall(cfg.dir, *updateWaitPID))
	}
	if *recoveryAction == "move-cleanup" && *updateWaitPID > 0 {
		waitForProcess(*updateWaitPID)
	}
	showLauncher := shouldOpenLauncher(explicitFlags, *openLauncher, *startImmediately) &&
		!maintenance && !*openSettings && !*diagnostics && !*applyLauncherUpdateFlag && !*applyLauncherRollbackFlag
	var releaseMenu func()
	if showLauncher {
		// The launcher edits preferences like Settings. It does not own the VM
		// lifecycle port: recovery tools must remain usable before boot.
		guard, err := acquireLauncherMenu(defaultDataDirectoryName)
		if err != nil {
			fatal(uiTextWith("fatal.launcher.open", map[string]string{"error": err.Error()}))
		}
		if guard == 0 {
			infoBox(uiText("setup.already_open"))
			return
		}
		releaseMenu = func() {
			if guard != 0 {
				procCloseHandle.Call(guard)
				guard = 0
			}
		}
		defer releaseMenu()
		*openSettings = true
	}
	// Direct starts bind before the first-run location prompt. The menu has its
	// own guard above. Settings, diagnostics, and update helpers remain usable
	// while the VM owns the lifecycle port.
	if !*openSettings && !*diagnostics && !*applyLauncherUpdateFlag && !*applyLauncherRollbackFlag && !installWalkthrough {
		runLifecycleListener()
	}
	if !cfg.portable {
		resolved, moveErr := prepareMovedLocation(cfg.dir, !*openSettings && !*diagnostics && !*applyLauncherUpdateFlag && !*applyLauncherRollbackFlag && !installWalkthrough, *uninstall || *recoveryAction == "uninstall")
		if moveErr != nil {
			fatal(uiTextWith("fatal.move.resolve", map[string]string{"error": moveErr.Error()}))
		}
		if !pathsEqual(resolved, cfg.dir) {
			cfg.dir = resolved
			explicitFlags["dir"] = true
		}
	}
	if cfg.portable {
		self, err := os.Executable()
		if err != nil {
			fatal(uiTextWith("fatal.launcher.portable_missing", map[string]string{"error": err.Error()}))
		}
		root := filepath.Dir(self)
		cfg.dir = filepath.Join(root, "data")
		cfg.payloadDir = filepath.Join(root, "payload")
		if err := requirePortableStartupFilesystem(root); err != nil {
			fatal(err.Error())
		}
		removeDataOnCancel, err = dataDirectoryEmpty(cfg.dir)
		if err != nil {
			fatal(uiTextWith("fatal.location.portable_inspect", map[string]string{"error": err.Error()}))
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
			fatal(uiTextWith("fatal.location.resolve", map[string]string{"error": err.Error(), "file": dataLocationPointerPath(defaultDir)}))
		}
		if !proceed {
			return
		}
		if !explicitFlags["dir"] && !pathsEqual(selected, defaultDir) {
			if err := validateStandardDataDrive(selected); err != nil {
				fatal(uiTextWith("fatal.location.unavailable", map[string]string{"path": selected, "error": err.Error(), "file": dataLocationPointerPath(defaultDir)}))
			}
		}
		cfg.dir = selected
		cfg.hostDir = cfg.dir
		removeDataOnCancel, err = dataDirectoryEmpty(cfg.dir)
		if err != nil {
			fatal(uiTextWith("fatal.location.inspect", map[string]string{"error": err.Error()}))
		}
		if *applyLauncherUpdateFlag || *applyLauncherRollbackFlag {
			if err := applyLauncherUpdate(cfg.dir, *updateWaitPID, *updateRestartArgs, *applyLauncherRollbackFlag); err != nil {
				errorBox(uiTextWith("update.error.apply", map[string]string{"error": err.Error()}))
				os.Exit(1)
			}
			return
		}
	}
	if validUninstallState(cfg.dir) && !*uninstall && *recoveryAction != "uninstall" {
		fatal(uiText("error.uninstall.incomplete"))
	}
	// The install walkthrough can inspect a running guest and ask its owner to
	// shut down. It must not take over lifecycle or interrupted-update recovery.
	if !*openSettings && !*diagnostics && !*applyLauncherUpdateFlag && !*applyLauncherRollbackFlag && !installWalkthrough {
		if err := recoverCheckpointRollback(cfg.dir); err != nil {
			fatal(uiTextWith("fatal.snapshot_recovery", map[string]string{"error": err.Error()}))
		}
	}
	// Settings and diagnostics may be opened from the running app's tray.
	// They must not inspect or roll back an update owned by that parent.
	if !maintenance && !*openSettings && !*diagnostics {
		restartArgs, err := encodeRestartArgs(os.Args[1:])
		if err != nil {
			fatal(uiTextWith("fatal.update_arguments", map[string]string{"error": err.Error()}))
		}
		if rollingBack, recoverErr := recoverLauncherUpdate(cfg.dir, restartArgs); recoverErr != nil {
			logf("launcher update recovery: %v", recoverErr)
		} else if rollingBack {
			return
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
			beginRecoveryProgress(uiText("recovery.backup.status"))
			err = writeVMBackupProgress(cfg.dir, *backupPath, recoveryProgress(recoveryBackingUp))
		} else {
			beginRecoveryProgress(uiText("recovery.restore.status"))
			err = restoreVMBackupProgress(*restorePath, cfg.dir, recoveryProgress(recoveryRestoring))
		}
		uiDone()
		if errors.Is(err, errSetupCancelled) {
			return
		}
		if err != nil {
			errorBox(uiTextWith("recovery.error", map[string]string{"error": err.Error()}))
			os.Exit(1)
		}
		if *backupPath != "" {
			infoBox(uiTextWith("recovery.backup.done", map[string]string{"path": *backupPath}))
		} else {
			if err := createRestoredLaunchers(cfg.dir); err != nil {
				infoBox(uiTextWith("recovery.restore.done_no_shortcuts", map[string]string{"path": cfg.dir, "error": err.Error()}))
			} else {
				infoBox(uiTextWith("recovery.restore.done", map[string]string{"path": cfg.dir}))
			}
		}
		return
	}

	// Runs before settings load on purpose: a damaged settings.json is one of
	// the things a bug report needs to carry.
	if *diagnostics {
		bundle, err := writeDiagnostics(cfg.dir, launcherFacts(cfg))
		if err != nil {
			errorBox(uiTextWith("diagnostics.error", map[string]string{"error": err.Error()}))
			os.Exit(1)
		}
		infoBox(uiTextWith("diagnostics.done", map[string]string{"path": bundle}))
		return
	}

	if *openSettings {
		if runLauncherSettings(settingsPath(cfg.dir), cfg.dir, cfg.portable, showLauncher, releaseMenu) {
			logf("settings saved to %s", settingsPath(cfg.dir))
			if showLauncher {
				self, err := os.Executable()
				if err != nil {
					fatal(uiTextWith("fatal.launcher.missing", map[string]string{"error": err.Error()}))
				}
				args := append(append([]string{}, os.Args[1:]...), "-dir", cfg.dir, "-start")
				cmd := exec.Command(self, args...)
				if err := cmd.Start(); err != nil {
					fatal(uiTextWith("fatal.start", map[string]string{"error": err.Error()}))
				}
				procAllowSetForeground.Call(uintptr(cmd.Process.Pid))
				_ = cmd.Process.Release()
			}
		}
		return
	}

	var desktopErr error
	cfg.desktop, desktopErr = loadDesktopPreferences(cfg.dir)
	if desktopErr != nil {
		fatal(uiTextWith("fatal.preferences.desktop", map[string]string{"error": desktopErr.Error()}))
	}

	cfg.audioDevices, desktopErr = loadLaunchAudioPreferences(cfg.dir)
	if desktopErr != nil {
		fatal(uiTextWith("fatal.preferences.audio", map[string]string{"error": desktopErr.Error()}))
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
		fatal(uiTextWith("fatal.settings.read", map[string]string{"error": err.Error()}))
	}
	if err := applySettings(cfg, userSettings, explicitFlags, &forwards, sshKeyPath); err != nil {
		fatal(uiTextWith("fatal.settings.use", map[string]string{"error": err.Error()}))
	}
	resourcePrefs, err := loadResourcePreferences(cfg.dir)
	if err != nil {
		fatal(uiTextWith("fatal.preferences.resources", map[string]string{"error": err.Error()}))
	}
	if explicitFlags["resource-profile"] {
		resourcePrefs.Profile = *resourceProfileFlag
	}
	if err := validateResourceProfile(resourcePrefs.Profile); err != nil {
		fatal(uiTextWith("fatal.invalid_setting", map[string]string{"error": err.Error()}))
	}
	if explicitFlags["render"] {
		mode, err := parseRenderMode(*renderFlag)
		if err != nil {
			fatal(uiTextWith("fatal.invalid_setting", map[string]string{"error": err.Error()}))
		}
		cfg.renderMode = mode
	}
	forceCPU := cfg.noGpu
	if cfg.noGpu {
		cfg.renderMode = renderCPU
	}
	cfg.noGpu = cfg.renderMode == renderCPU
	if cfg.memOverrideMiB != 0 && (cfg.memOverrideMiB < minimumGuestMemoryMiB || cfg.memOverrideMiB > maximumGuestMemoryMiB) {
		fatal(uiTextWith("fatal.cli.memory_range", map[string]string{"min": fmt.Sprint(minimumGuestMemoryMiB), "max": fmt.Sprint(maximumGuestMemoryMiB)}))
	}
	if !explicitFlags["disk-size"] {
		storage, err := loadStorageWithRepair(cfg.dir)
		if err != nil {
			if errors.Is(err, errSetupCancelled) {
				uiDone()
				return
			}
			fatal(uiTextWith("fatal.preferences.storage", map[string]string{"error": err.Error()}))
		}
		cfg.diskGiB = storage.DiskGiB
	}
	if _, err := requestedDiskMiB(24*1024, cfg.diskGiB, cfg.portable); err != nil {
		fatal(uiTextWith("fatal.invalid_setting", map[string]string{"error": err.Error()}))
	}
	home, _ := os.UserHomeDir()
	sshKey, err := resolveSSHPreset(&forwards, *sshPort, *sshKeyPath, home, explicitFlags["ssh-key"])
	if err != nil {
		fatal(uiTextWith("fatal.ssh", map[string]string{"error": err.Error()}))
	}
	cfg.forwards = forwards
	var pausedLANForwards []portForward
	if !explicitFlags["forward"] && !explicitFlags["ssh"] && len(userSettings.ForwardAdapters) > 0 {
		adapters, err := availableLANAdapters()
		if err != nil {
			fatal(uiTextWith("fatal.lan.adapters", map[string]string{"error": err.Error()}))
		}
		active, paused := filterUnavailableForwardAdapters(forwards, userSettings.ForwardAdapters, adapters)
		pausedLANForwards = paused
		cfg.forwards, err = resolveForwardAdapters(active, userSettings.ForwardAdapters, adapters)
		if err != nil {
			fatal(uiTextWith("fatal.lan.prepare", map[string]string{"error": err.Error()}))
		}
	}
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
		fatal(uiTextWith("fatal.update.recover", map[string]string{"error": err.Error()}))
	}
	if payloadsRolledBack {
		if err := pinRestoredPayloads(cfg.dir, release, sumsSHA256, runtimeRelease, runtimeSumsSHA256); err != nil {
			fatal(uiTextWith("fatal.update.restored", map[string]string{"error": err.Error()}))
		}
		logf("using restored guest and runtime for this recovery launch")
	}
	snapshotRecovery, err := pinCheckpointBoot(cfg.dir, explicitFlags, release, sumsSHA256, runtimeRelease, runtimeSumsSHA256)
	if err != nil {
		fatal(uiTextWith("fatal.snapshot_restored", map[string]string{"error": err.Error()}))
	}
	completeAtStart := completeInstallExists(cfg.dir, filepath.Base(cfg.disk))
	needsProvisioning := cfg.fresh || !completeAtStart
	configureSetupCancellation(!installationDataExists(cfg.dir) && removeDataOnCancel)
	if err := os.MkdirAll(cfg.vmDir, 0o755); err != nil {
		fatal(uiTextWith("fatal.data_directory", map[string]string{"error": err.Error()}))
	}
	if err := os.MkdirAll(cfg.hostDir, 0o755); err != nil {
		fatal(uiTextWith("fatal.host_state_directory", map[string]string{"error": err.Error()}))
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
	getUI().setStatus("%s", uiText("status.starting_launcher"))
	if err := configureRecommendedSharedFolder(cfg, &userSettings, settingsFile, home, explicitFlags["share"]); err != nil {
		if finishSetupCancellation(cfg, err) {
			return
		}
		fatal(uiTextWith("fatal.share.recommended", map[string]string{"error": err.Error()}))
	}
	if finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}
	if cfg.share != "" {
		validated, shareErr := validateWindowsSharedFolder(cfg.share, cfg.dir, home)
		if shareErr != nil {
			if explicitFlags["share"] {
				fatal(uiTextWith("fatal.share.folder", map[string]string{"folder": cfg.share, "error": shareErr.Error()}))
			}
			logf("shared folder disabled for this launch: %v", shareErr)
			infoBox(uiTextWith("share.unavailable", map[string]string{"error": shareErr.Error()}))
			cfg.share = ""
		} else {
			cfg.share = validated
		}
	}
	// Per-run stderr: the memory ladder sniffs this file, stale errors from a
	// previous run must not be mistaken for this one's.
	os.Remove(filepath.Join(cfg.vmDir, "qemu-stderr.log"))

	updatesEnabled := !snapshotRecovery && !cfg.desktop.AutomaticUpdatesDisabled && automaticUpdatesEnabled(cfg, *noUpdate, *release, *sumsSHA256)
	ownPayload := ownPayloadUpdate(cfg, completeAtStart, snapshotRecovery || payloadsRolledBack, explicitFlags, launcherPins)
	// A new launcher can boot the installed image without upgrading it online.
	// Explicit payload pins, reset and checkpoint recovery retain their meaning.
	if completeAtStart && !cfg.fresh && !snapshotRecovery && !payloadsRolledBack {
		if !explicitFlags["release"] && !explicitFlags["sums-sha256"] {
			if r, digest, ok := installReceiptIdentity(cfg.guestDir); ok {
				*release, *sumsSHA256 = r, digest
			}
		}
		if !explicitFlags["runtime-release"] && !explicitFlags["runtime-sums-sha256"] {
			if r, digest, ok := runtimeReceiptIdentity(filepath.Join(cfg.dir, "runtime")); ok {
				*runtimeRelease, *runtimeSumsSHA256 = r, digest
			}
		}
	}
	payloadRoot := updatePayloadRoot(cfg.dir, cfg.payloadDir, cfg.portable)
	appliedOwnPayload := false
	if ownPayload != nil {
		key, _ := updatePublicKey()
		if err := verifiedLauncherPayloadUpdate(setupContext(), cfg.dir, payloadRoot, *ownPayload, key); err == nil {
			*release, *sumsSHA256 = ownPayload.Release, ownPayload.Digest
			*runtimeRelease, *runtimeSumsSHA256 = ownPayload.RuntimeRelease, ownPayload.RuntimeDigest
			cfg.payloadDir, cfg.localPayload = payloadRoot, true
			cfg.localPayloadSHA256, cfg.localRuntimePayloadSHA256 = ownPayload.Digest, ownPayload.RuntimeDigest
			appliedOwnPayload = true
			ownPayload = nil
		} else if marker, e := os.ReadFile(filepath.Join(launcherUpdateDir(cfg.dir), stagedUpdateFilename)); e == nil && string(marker) == currentVersion {
			if setupCancelled() {
				return
			}
			if _, e := os.Stat(filepath.Join(launcherUpdateDir(cfg.dir), currentVersion, pinnedPayloadFilename)); e == nil {
				logf("discarding staged launcher payload: %v", err)
				_ = removeUpdateFile(filepath.Join(launcherUpdateDir(cfg.dir), stagedUpdateFilename))
				_ = removeUpdateFile(filepath.Join(launcherUpdateDir(cfg.dir), currentVersion, pinnedPayloadFilename))
			}
		}
	}
	state, _ := readLauncherUpdateState(cfg.dir)
	version := ""
	activeUpdate := state != nil && state.Version == currentVersion && state.ManifestSHA256 != ""
	if activeUpdate {
		version = state.Version
	} else if updatesEnabled && !payloadsRolledBack && !appliedOwnPayload {
		data, _ := os.ReadFile(filepath.Join(launcherUpdateDir(cfg.dir), stagedUpdateFilename))
		version = string(data)
		if version == currentVersion && ownPayload != nil {
			version = ""
		}
	}
	if version != "" && !snapshotRecovery {
		key, keyErr := updatePublicKey()
		var manifest *updateManifest
		var stageErr error
		if keyErr != nil {
			stageErr = keyErr
		} else {
			manifest, stageErr = verifiedStagedUpdate(setupContext(), cfg.dir, payloadRoot, version, key)
		}
		if stageErr != nil {
			if setupCancelled() {
				return
			}
			logf("discarding staged update: %v", stageErr)
			discardStagedUpdate(cfg.dir, payloadRoot, version)
			if activeUpdate {
				fatal(uiTextWith("fatal.update.recover", map[string]string{"error": stageErr.Error()}))
			}
		} else if failedUpdateVersion(cfg.dir) != version && (activeUpdate || version == currentVersion || updateIsNewer(version, currentVersion)) {
			if version != currentVersion {
				if started, err := startStagedLauncherUpdate(cfg, manifest, os.Args[1:]); err != nil {
					logf("staged update deferred: %v", err)
				} else if started {
					uiDone()
					return
				}
			} else {
				*release, *sumsSHA256 = manifest.Release, manifest.ManifestSHA256
				if !explicitFlags["runtime-release"] && !explicitFlags["runtime-sums-sha256"] {
					*runtimeRelease, *runtimeSumsSHA256 = manifest.Release, manifest.ManifestSHA256
				}
				if !cfg.portable {
					cfg.payloadDir, cfg.localPayload = payloadRoot, true
					cfg.localPayloadSHA256 = manifest.ManifestSHA256
				}
			}
		}
	}
	stopUpdates := configureBackgroundUpdates(cfg, *updateURL, updatesEnabled && !payloadsRolledBack, ownPayload)
	defer stopUpdates()
	defer func() {
		if (setupCancelled() || intentionalUpdateQuit.Load()) && !bootAnnouncedReady.Load() {
			if err := interruptPendingUpdates(cfg.dir); err != nil {
				logf("recording interrupted update: %v", err)
			}
		}
	}()

	// Explicit reset acquisition must complete before runtime/guest publication.
	if cfg.fresh {
		if err := ensureGuest(cfg, *release, *sumsSHA256); err != nil {
			if finishSetupCancellation(cfg, err) {
				return
			}
			fatal(uiTextWith("fatal.image.setup", map[string]string{"error": err.Error(), "help": setupFailureHelp(err)}))
		}
	}

	// Detachment can fail for retryable reasons such as a full USB stick. Finish
	// it before runtime publication can mark this version as a started update.
	if !cfg.fresh && !payloadsRolledBack {
		if err := preparePortablePayloadTransition(cfg, *release, *sumsSHA256); err != nil {
			if finishSetupCancellation(cfg, err) {
				return
			}
			fatal(uiTextWith("fatal.portable_disk_update", map[string]string{"error": err.Error()}))
		}
	}

	// Machine setup the old bootstrap.ps1 handled: hypervisor on (may walk the
	// user through one restart and exit), then a QEMU to run: C:\WINQ-EMU if
	// present, otherwise the bundled runtime, downloaded when missing. Stock QEMU
	// from the old bootstrap is only a fallback when the runtime cannot be set
	// up, CPU rendering included: stock WHPX misses QMP at startup and wedges
	// instead of exiting after a guest poweroff (docs/FINDINGS.md).
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
	haveStock := stockErr == nil && !cfg.portable && !snapshotRecovery && guestDisplayCount(cfg.displays) == 1
	gpuRoot := ""
	if !cfg.portable && !snapshotRecovery && guestDisplayCount(cfg.displays) == 1 {
		_, err := os.Stat(filepath.Join(cfg.winqEmu, "bin", qemuExe))
		if err == nil {
			// A user-managed WINQ-EMU install stays under the user's control. Only
			// the bundled runtime under cfg.dir participates in automatic updates.
			gpuRoot = cfg.winqEmu
		}
	}
	if gpuRoot == "" {
		if payloadsRolledBack {
			root := filepath.Join(cfg.dir, "runtime")
			info, err := os.Stat(filepath.Join(root, "bin", qemuExe))
			if err != nil || !info.Mode().IsRegular() {
				fatal(uiText("fatal.runtime.incomplete"))
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
						fatal(uiTextWith("fatal.runtime.portable", map[string]string{"error": err.Error()}))
					}
					fatal(uiTextWith("fatal.runtime.download", map[string]string{"error": err.Error(), "help": setupFailureHelp(err)}))
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
		cfg.venus, reason = venusDecision(os.Getenv(venusVariable), cfg.displayDriver)
		logf("rendering: Venus Vulkan %s - %s", map[bool]string{false: "off", true: "on"}[cfg.venus], reason)
	} else {
		cfg.qemu = stockQemu
	}

	// Prepare boot files from the authenticated selected payload. Creation and
	// explicit reset acquire a missing exact-pin factory separately.
	if payloadsRolledBack {
		ready, err := installReceiptMatches(cfg.guestDir, *release, *sumsSHA256, bootGuestArtifacts)
		if err != nil || !ready || validateInstalledDiskBacking(cfg) != nil {
			fatal(uiText("fatal.image.incomplete"))
		}
	} else if !cfg.resetPayloadPrepared {
		if err := ensureGuest(cfg, *release, *sumsSHA256); err != nil {
			if finishSetupCancellation(cfg, err) {
				return
			}
			if cfg.portable {
				fatal(uiTextWith("fatal.image.portable", map[string]string{"error": err.Error()}))
			}
			fatal(uiTextWith("fatal.image.setup", map[string]string{"error": err.Error(), "help": setupFailureHelp(err)}))
		}
	}
	if cfg.share != "" && !cfg.supportsSharing {
		logf("shared folder disabled for this launch: selected QEMU has no virtio-9p")
		infoBox(uiText("share.unsupported_runtime"))
		cfg.share = ""
	}

	specData, err := os.ReadFile(filepath.Join(cfg.guestDir, "build-spec.json"))
	if err != nil {
		fatal(uiTextWith("fatal.build_spec.read", map[string]string{"error": err.Error()}))
	}
	var spec buildSpec
	if err := json.Unmarshal(specData, &spec); err != nil {
		fatal(uiTextWith("fatal.build_spec.parse", map[string]string{"error": err.Error()}))
	}
	cfg.guestPinch = guestAcceptsPinch(spec)
	// Serial log only - no console= on the display, so no kernel text or
	// blinking cursor flashes in the window before SDDM (boot problems: read
	// vm\serial*.log).
	cmdline := strings.ReplaceAll(spec.Runtime.KernelCommandLine, "console=tty0 ", "")
	cmdline = strings.ReplaceAll(cmdline, "console=hvc0", "console=ttyS0")
	cmdline += " vt.global_cursor_default=0"
	// The kernel's setup code prints "Probing EDD" on the display while it
	// asks the BIOS about disks, which a virtio disk does not need.
	cmdline += " edd=off"
	if cfg.instant {
		cmdline += " tryomarchy.instant=1"
	}
	cmdline += sshCmdline(cfg.forwards, cfg.sshKey)
	plan := &bootPlan{
		explicit: explicitFlags, baseCmdline: cmdline, profile: resourcePrefs.Profile, forceCPU: forceCPU,
		timeZone: *timeZoneFlag, keyboard: *keyboardFlag, language: *localeFlag, home: home,
		gpuRuntime: gpuRoot != "", multiDisplayRuntime: gpuRoot != "" && gpuRoot != cfg.winqEmu,
	}

	if err := prepareDisk(cfg, spec.Runtime.Storage.ExpandedSizeMiB); err != nil {
		if finishSetupCancellation(cfg, err) {
			return
		}
		fatal(uiTextWith("fatal.disk", map[string]string{"error": err.Error()}))
	}
	if unclean, err := previousGuestExitUnclean(cfg.dir); err != nil {
		logf("could not read previous guest exit: %v", err)
	} else if unclean && msgBox(uiText("shutdown.unclean_recovery"), mbYesNo|mbIconQuestion|mbDefbutton2) == idYes {
		if err := runRecoveryUI(cfg.dir, "snapshots"); err != nil {
			errorBox(err.Error())
		}
		return
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
	getUI().setStatus("%s", uiText("status.measuring_resources"))
	if err := planBootResources(cfg, plan); err != nil {
		fatal(uiTextWith("fatal.resources", map[string]string{"error": err.Error()}))
	}
	bootLine := bootCmdline(cfg, plan)
	getUI().setStatus("%s", uiText("status.starting_omarchy"))
	stopTray := startTray(cfg)
	defer stopTray()
	if len(pausedLANForwards) > 0 {
		rules := make([]string, 0, len(pausedLANForwards))
		for _, forward := range pausedLANForwards {
			rules = append(rules, forward.String())
		}
		message := uiTextWith("tray.lan.paused", map[string]string{"rules": strings.Join(rules, ", ")})
		logf("%s", message)
		showTrayNotice(uiText("tray.lan.title"), message)
	}

	// SDL's keyboard grab installs a system-wide Win-key hook that leaks past
	// window focus; our hook does it right (focus-scoped).
	os.Setenv("SDL_GRAB_KEYBOARD", "0")

	reclaimDir.Store(&cfg.dir)
	reclaimSupported.Store(cfg.diskFormat == "raw")
	go runGuestAgent(cfg.dir)
	go watchKeyboardPreferences(cfg.dir)
	go runWinKeyHook()
	go runWinKeyQmp()
	go runTitleEnforcer(cfg.dir, cfg.fullscreen, cfg.fullscreenDisplay)
	go runCursorReleaseGuard()
	go runCloseGuard()
	runClipboardBridge()
	runCameraBridge(cfg.desktop)
	runHelloBridge()
	cfg.followHostTimeZone = *timeZoneFlag != "keep" && guestAcceptsTimeZone(spec)
	if cfg.followHostTimeZone {
		stopTimeZone := startTimeZoneBridge(func() string {
			if *timeZoneFlag != "" {
				return *timeZoneFlag
			}
			return ianaZoneForWindows(hostTimeZoneKey())
		})
		defer stopTimeZone()
	}
	if audioRuntimeSupportsLiveRouting(cfg.qemu) {
		runAudioBridge(cfg.dir, cfg.qemu, cfg.desktop.MicrophoneDisabled)
	}

	activeForwards, pausedPorts := filterForwardBindings(cfg.forwards)
	cfg.forwards = activeForwards
	pauseLaunchForwards(pausedPorts, false)
	noticePausedForwards("tray.forward.paused", pausedPorts, "")
	if err := prepareLANFirewall(cfg); err != nil {
		var paused []portForward
		cfg.forwards, paused = filterLANForwards(cfg.forwards)
		pauseLaunchForwards(paused, true)
		noticePausedForwards("tray.lan.firewall_paused", paused, err.Error())
		logf("LAN firewall unavailable: %v", err)
	}
	cfg.launchForwards = append([]portForward(nil), cfg.forwards...)
	// Command-line -forward and -ssh replace the saved list for this launch.
	if !explicitFlags["forward"] && !explicitFlags["ssh"] {
		go runLiveForwardWatcher(cfg.dir, cfg.launchForwards)
	}
	cfg.audio = "sdl"

	startBootCurtain(cfg)
	stopDiskMonitor := startHostDiskMonitor(cfg.dir)
	defer stopDiskMonitor()
	for relaunch := supervise(cfg, bootLine); relaunch; relaunch = supervise(cfg, bootLine) {
		// A guest reboot applies what Settings saved meanwhile. A failed
		// measurement keeps the previous boot's size.
		reloadBootSettings(cfg, plan)
		if err := planBootResources(cfg, plan); err != nil {
			logf("reboot: keeping %d CPUs and %d MiB: %v", cfg.cpus, cfg.memMiB, err)
		}
		bootLine = bootCmdline(cfg, plan)
	}
	stopDiskMonitor()
	if (setupCancelled() || intentionalUpdateQuit.Load()) && !bootAnnouncedReady.Load() {
		if err := interruptPendingUpdates(cfg.dir); err != nil {
			logf("recording interrupted update: %v", err)
		}
	}
	if !restartForUpdate.Load() && finishSetupCancellation(cfg, checkSetupCancelled()) {
		return
	}
	stopUpdates()
	if windowsSessionEnding.Load() {
		return
	}

	compactAfterShutdown(cfg)
	if restartForUpdate.Load() {
		self, err := os.Executable()
		if err == nil {
			cmd := exec.Command(self, append(os.Args[1:], "-restart-update-wait-pid", strconv.Itoa(os.Getpid()))...)
			if err := cmd.Start(); err != nil {
				logf("restart to update: %v", err)
			} else {
				_ = cmd.Process.Release()
			}
		}
	}
	logf("---- exiting ----")
}

func loadLaunchAudioPreferences(dir string) (audioPreferences, error) {
	preferences, err := loadAudioPreferences(dir)
	if err != nil {
		return preferences, err
	}
	ids, err := loadAudioEndpoints(dir)
	if err != nil {
		return preferences, err
	}
	if endpoints, err := listAudioEndpoints(); err != nil {
		logf("Stable audio endpoint lookup is unavailable; using saved device names: %v", err)
	} else {
		preferences.Output, _ = resolveAudioSelection(preferences.Output, ids.OutputID, endpoints.Output)
		preferences.Input, _ = resolveAudioSelection(preferences.Input, ids.InputID, endpoints.Input)
	}
	return preferences, nil
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
		if setupCancelled() || windowsSessionEnding.Load() {
			return false
		}
		ran, preflightErr := cfg.runtimeLoader.run(setupContext(), attempt, cfg.qemu, runtimeLoaderTimeout, loadRuntime)
		if setupCancelled() || windowsSessionEnding.Load() {
			return false
		}
		if ran && preflightErr != nil {
			logf("runtime loader preflight did not complete: %v; continuing with normal QEMU startup", preflightErr)
		}
		mode := "CPU rendering (llvmpipe)"
		if cfg.useGpu && cfg.venus {
			mode = "GPU accelerated (virgl + Venus Vulkan)"
		} else if cfg.useGpu {
			mode = "GPU accelerated (virgl)"
		}
		logf("booting - %s (attempt %d)", mode, attempt)
		pendingReboot.Store(false)
		guestCompositorHealth.boot(time.Now())
		guestReady.Store(false)
		guestBootStarted()
		controlDir, err := prepareQMPControl()
		if err != nil {
			fatal(uiTextWith("fatal.vm_controls", map[string]string{"error": err.Error()}))
		}
		cfg.qmpDir = controlDir
		// Guest picker and a separate Settings process can change these files
		// while QEMU runs. A guest reboot must use the latest saved choices.
		cfg.audioDevices, err = loadLaunchAudioPreferences(cfg.dir)
		if err != nil {
			fatal(uiTextWith("fatal.preferences.audio_reload", map[string]string{"error": err.Error()}))
		}
		// Local forwards changed while running (forward_live.go) carry into a
		// reboot instead of reverting to the launch list.
		cfg.forwards = filterPausedLaunchForwards(forwardsForBoot(cfg.launchForwards))
		audioSelection := cfg.audio == "sdl" && audioRuntimeSupportsSelection(cfg.qemu)
		if cfg.audio == "sdl" {
			cfg.audioRates = launchAudioSampleRates(cfg.audioDevices, audioSelection, cfg.desktop.MicrophoneDisabled)
			logf("SDL audio sample rates: output=%d Hz input=%d Hz", cfg.audioRates.Output, cfg.audioRates.Input)
		}
		proc = exec.Command(cfg.qemu, buildQemuArgs(cfg, cmdline)...)
		if !audioSelection && (cfg.audioDevices.Output != "" || (!cfg.desktop.MicrophoneDisabled && cfg.audioDevices.Input != "")) {
			logf("Selected audio devices require the updated SDL runtime; this attempt uses Windows defaults")
		}
		proc.Env = audioEnvironment(os.Environ(), cfg.audioDevices, audioSelection, cfg.desktop.MicrophoneDisabled)
		if cfg.audio == "sdl" && audioRuntimeSupportsLiveRouting(cfg.qemu) {
			routeDir := audioRouteDirectory(cfg.dir)
			if err := publishAudioRoutes(routeDir, cfg.audioDevices, cfg.desktop.MicrophoneDisabled); err != nil {
				logf("live audio controls are unavailable for this boot: %v", err)
			} else {
				proc.Env = append(proc.Env, "OMARCHY_SDL_AUDIO_CONTROL_DIRECTORY="+routeDir)
			}
		}
		pinch := pinchEnabled(cfg)
		logf("touchpad pinch forwarding: %v (guest declares device: %v)", pinch, cfg.guestPinch)
		proc.Env = pinchEnvironment(proc.Env, pinch)
		externalHook := keyboardRuntimeSupportsExternalHook(cfg.qemu)
		qemuExternalKeyboardHook.Store(externalHook)
		proc.Env = keyboardEnvironment(proc.Env, externalHook)
		if cfg.useGpu {
			cfg.displayDriver = displayDriverIdentity()
			logGPULaunchFacts(cfg, proc.Env, dxgiAdapterFacts(), qemuGPUPreference(cfg.qemu))
		}
		// The w-binary's startup errors (bad args, SDL init) only ever reach
		// stderr; without this they vanish and a dead QEMU is undebuggable.
		ef, err := os.OpenFile(filepath.Join(cfg.vmDir, "qemu-stderr.log"),
			os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0o644) // per-attempt: the memory ladder sniffs it
		if err == nil {
			proc.Stdout = ef
			proc.Stderr = ef
		}
		guestLaunchInProgress.Store(true)
		if windowsSessionEnding.Load() {
			guestLaunchInProgress.Store(false)
			if ef != nil {
				ef.Close()
			}
			return false
		}
		guestRuntimeGeneration.Add(1)
		guestExitedCleanly.Store(false)
		if err := recordGuestExit(cfg.dir, false, "running"); err != nil {
			logf("could not record guest start: %v", err)
		}
		if windowsSessionEnding.Load() {
			guestLaunchInProgress.Store(false)
			if ef != nil {
				ef.Close()
			}
			return false
		}
		err = proc.Start()
		// QEMU inherits its own handle. Closing ours per attempt keeps retries
		// and guest reboots from leaking one handle each.
		if ef != nil {
			ef.Close()
		}
		if err != nil {
			guestLaunchInProgress.Store(false)
			fatal(uiTextWith("fatal.qemu.start", map[string]string{"error": err.Error()}))
		}
		qemuPid.Store(uint32(proc.Process.Pid))
		guestLaunchInProgress.Store(false)
		exited := make(chan error, 1)
		go func() {
			err := proc.Wait()
			if err != nil {
				logf("QEMU process exited with error: %v", err)
			}
			exited <- err
		}()

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
				cancelBackgroundUpdate()
				proc.Process.Kill()
				<-exited
				qemuPid.Store(0)
				return false
			case <-exited:
				startupDead = true
				if detail := qemuStartupFailureTail(cfg.vmDir); detail != "" {
					logf("QEMU startup failure (attempt %d, %s):\n%s", attempt, mode, detail)
				}
				if forwardStartupProblem(cfg.vmDir) {
					active, paused := failedStartupForwards(cfg.vmDir, cfg.forwards)
					if len(paused) == 0 {
						// Some QEMU versions omit the rule. Re-probe before giving up
						// forwarding for this launch; keep the VM available either way.
						active, paused = filterForwardBindings(cfg.forwards)
						if len(paused) == 0 {
							active, paused = nil, cfg.forwards
						}
					}
					if len(paused) > 0 {
						pauseLaunchForwards(paused, false)
						cfg.forwards, cfg.launchForwards = active, active
						setLiveForwards(active)
						noticePausedForwards("tray.forward.paused", paused, "")
						// Every retry removes a rule. Keep the existing watchdog budget
						// for VM failures rather than spending it on up to 64 forwards.
						attempt--
						break probe
					}
					fatal(uiText("fatal.qemu.port"))
				}
				// The host refused nested virtualization for the partition
				// (issue #19). Nothing else about the launch is wrong, so
				// retry with the irqchip in QEMU, which never asks for it.
				if nestedVirtRefused(cfg) {
					if !cfg.irqchipOff {
						logf("QEMU exited at startup - host refused nested virtualization, retrying with kernel-irqchip=off")
						cfg.irqchipOff = true
						break probe
					}
					fatal(uiTextWith("fatal.qemu.nested", map[string]string{"folder": cfg.vmDir}))
				}
				// SDL supports capture and playback. Retain playback-only
				// DirectSound, then silent operation, for unavailable devices.
				if cfg.audio != "none" && audioUnavailable(cfg) {
					if cfg.audio == "sdl" {
						cfg.audio = "dsound"
					} else {
						cfg.audio = "none"
					}
					logf("QEMU exited at startup - retrying with audio backend %s", cfg.audio)
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
					fatal(uiText("fatal.memory"))
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
				fatal(uiTextWith("fatal.qemu.exited", map[string]string{"folder": cfg.vmDir}))
			case <-time.After(wait):
				wait = 3 * time.Second
				qmp = qmpConnect(qmpSupPort, 8*time.Second)
			}
		}
		if qmp != nil {
			guestUp.Store(true)
			if pref, e := loadUSBPreferences(cfg.dir); e != nil {
				logf("USB choice not applied: %v", e)
			} else if pref.Enabled {
				ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
				client, e := dialQMPControl(ctx, qmpToolsPort)
				if e == nil {
					e = (usbBroker{client}).AttachSaved(ctx, pref)
					client.Close()
				}
				cancel()
				if e != nil {
					logf("USB choice not applied: %v", e)
				} else {
					logf("saved USB device attached")
				}
			}
			// QMP answers while the guest is kernel-panicked, and a graphics
			// runtime can start QMP before failing later in guest boot. Keep all
			// update components rollback-capable until the in-guest readiness
			// service reaches userspace and networking.
			defer qmp.close()
			return watch(cfg, qmp, exited)
		}
		qemuPid.Store(0)
		if !startupDead {
			logf("QEMU did not answer QMP within the startup timeout - killing and retrying")
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
	fatal(uiTextWith("fatal.qemu.unhealthy", map[string]string{"count": fmt.Sprint(maxLaunchAttempts)}))
	return false
}

func watch(cfg *config, qmp *qmpConn, exited <-chan error) bool {
	logf("supervisor: watching guest lifecycle and file drops")
	lines := qmp.readLines()
	reason := ""
	cleanExit := false
	silence := qmpSilence{}
	postReady := false
	qmpRunning := true
	lastTick, lastReply := time.Now(), time.Now()
	hangReported := false
	var recovery <-chan gpuRecoveryDecision
	var cancelRecovery context.CancelFunc
	defer func() {
		if cancelRecovery != nil {
			cancelRecovery()
		}
	}()
	offerRecovery := func(trigger string) {
		if recovery != nil || !postReady || setupCancelled() || pendingReboot.Load() || guestCompositorHealth.suppressRecovery() {
			return
		}
		if trigger != "blank" {
			logf("display health: %s stopped responding; preserving session while offering recovery", trigger)
		}
		suggest := false
		// A window that never showed the desktop is not a freeze.
		if cfg.useGpu && trigger != "blank" {
			var err error
			suggest, err = recordGPUFreeze(cfg.dir, cfg.renderMode, true, postReady, cfg.runtimeID, cfg.displayDriver)
			if err != nil {
				logf("could not record GPU freeze: %v", err)
			}
		}
		result := make(chan gpuRecoveryDecision, 1)
		recovery = result
		snapshotCfg := *cfg
		snapshotPID, snapshotMisses := qemuPid.Load(), silence.misses
		dialogContext, cancel := context.WithCancel(context.Background())
		cancelRecovery = cancel
		go func() {
			facts := launcherFacts(&snapshotCfg)
			facts["display.freeze.trigger"] = trigger
			facts["display.freeze.qemuPID"] = fmt.Sprint(snapshotPID)
			facts["display.freeze.qmpMisses"] = fmt.Sprint(snapshotMisses)
			facts["display.freeze.qmpHangThresholdProbes"] = fmt.Sprint(qmpHangMisses)
			if snapshotCfg.useGpu {
				recordGPURuntimeReport(snapshotCfg.dir, snapshotCfg.vmDir)
			}
			bundle, err := writeDiagnostics(snapshotCfg.dir, facts)
			if err != nil {
				logf("display freeze diagnostics failed: %v", err)
			} else {
				logf("display freeze diagnostics saved: %s", bundle)
			}
			select {
			case <-dialogContext.Done():
				result <- gpuRecoveryDecision{}
				return
			default:
			}
			result <- chooseFreezeRecovery(dialogContext.Done(), snapshotCfg.useGpu, trigger, bundle, err, suggest)
		}()
	}
	// GPU rendering is recorded as working only once Windows has shown the
	// desktop in the Omarchy window; blankOffered asks once per boot.
	gpuRecordPending, blankOffered := false, false
	tick := 0
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	procDown := false
	movedBootPending := false
	diskPauseNotified := false
	// Cancel in the setup window, which stays up while Omarchy boots, shuts
	// the guest down. Early in its boot the guest misses the power button, so
	// it is pressed again once Omarchy reports that it is up; stopping it
	// outright could interrupt a first boot's account setup. A guest that
	// still has not shut down is stopped.
	var stopDeadline <-chan time.Time
	pressAgainWhenUp := false
	for reason == "" && !procDown {
		if pressAgainWhenUp && bootAnnouncedReady.Load() {
			pressAgainWhenUp = false
			logf("guest is up - asking it again to shut down")
			qmp.writeLine(`{"execute":"system_powerdown"}`)
			stopDeadline = time.After(30 * time.Second)
		}
		if guestReady.Swap(false) {
			postReady = true
			guestCompositorHealth.userspaceReady()
			if cfg.useGpu {
				recordGPURuntimeReport(cfg.dir, cfg.vmDir)
			}
			pendingPayload, _ := readPayloadUpdateState(cfg.dir)
			commitLauncherUpdate(cfg.dir)
			commitPayloadUpdates(cfg.dir)
			commitCheckpointBoot(cfg.dir)
			if pendingPayload != nil {
				pruneCommittedUpdatePayloads(cfg)
			}
			startReadyUpdateCheck()
			gpuRecordPending, blankOffered = cfg.useGpu, false
			if !cfg.useGpu {
				recordRenderResult(cfg)
			}
			movedBootPending = true
		}
		if movedBootPending && markMovedGuestReady(cfg.dir) {
			movedBootPending = false
		}
		select {
		case <-exited:
			procDown = true
		case line, ok := <-lines:
			if !ok {
				lines = nil
				if postReady && !pendingReboot.Load() && !setupCancelled() {
					hangReported = true
					select {
					case <-exited:
						procDown = true
					default:
						offerRecovery("qmp")
					}
				} else {
					procDown = waitExit(exited, 15*time.Second, cfg)
				}
				break
			}
			silence.answered()
			hangReported = false
			lastReply = time.Now()
			var status struct {
				Return *vmRuntimeStatus `json:"return"`
				Event  string           `json:"event"`
			}
			if json.Unmarshal([]byte(line), &status) == nil {
				if status.Return != nil && status.Return.Status != "" {
					qmpRunning = status.Return.Running
				}
				if status.Event == "STOP" {
					qmpRunning = false
				}
				if status.Event == "RESUME" {
					qmpRunning = true
					guestCompositorHealth.power(time.Now(), false)
				}
			}
			if diskFullPauseEvent(line) && !diskPauseNotified {
				diskPauseNotified = true
				message := uiText("tray.disk.paused")
				logf("%s", message)
				showTrayNotice(uiText("tray.disk.paused_title"), message)
			}
			if paths, point, ok := droppedFilesEvent(line); ok {
				logf("file drop: received %d item(s)", len(paths))
				if err := sendDroppedFilesAt(paths, guestDropPoint(point), cursorPosition()); err != nil {
					reportTransferError(err)
				}
			}
			if r := shutdownReason(line); r != "" {
				reason = r
				cleanExit = cleanGuestShutdown(line)
			}
		case <-setupCancelWake:
			cancelBackgroundUpdate()
			if stopDeadline == nil {
				guestCompositorHealth.stop()
				logf("startup cancelled - shutting the guest down")
				if err := qmp.writeLine(`{"execute":"system_powerdown"}`); err != nil {
					procDown = waitExit(exited, 15*time.Second, cfg)
					break
				}
				pressAgainWhenUp = !bootAnnouncedReady.Load()
				wait := 30 * time.Second
				if pressAgainWhenUp {
					wait = 90 * time.Second
				}
				stopDeadline = time.After(wait)
			}
		case <-stopDeadline:
			logf("guest did not shut down after the cancel - stopping it")
			qmp.writeLine(`{"execute":"quit"}`)
			stopDeadline = nil
			procDown = waitExit(exited, 15*time.Second, cfg)
		case decision := <-recovery:
			recovery = nil
			if cancelRecovery != nil {
				cancelRecovery()
				cancelRecovery = nil
			}
			if !decision.restart || setupCancelled() || pendingReboot.Load() || guestCompositorHealth.suppressRecovery() {
				continue
			}
			if err := saveGPURecoveryChoice(cfg.dir, decision); err != nil {
				errorBox(uiTextWith("recovery.gpu.settings_failed", map[string]string{"error": err.Error()}))
				continue
			}
			guestCompositorHealth.stop()
			cfg.recoveryChoice = &decision
			// Only this explicit choice authorizes discarding the session.
			qmp.writeLine(`{"execute":"quit"}`)
			waitExit(exited, 15*time.Second, cfg)
			guestUp.Store(false)
			qemuPid.Store(0)
			return true
		case <-ticker.C:
			now := time.Now()
			if cancelRecovery != nil && guestCompositorHealth.suppressRecovery() {
				cancelRecovery()
			}
			if now.Sub(lastTick) > 5*time.Second {
				silence.answered()
				lastReply = now
			}
			lastTick = now
			if stopDeadline == nil && !pendingReboot.Load() && !setupCancelled() && guestCompositorHealth.stalled(now, qmpRunning && now.Sub(lastReply) < 15*time.Second) {
				offerRecovery("compositor")
			}
			if gpuRecordPending && desktopSeen.Load() {
				gpuRecordPending = false
				recordRenderResult(cfg)
			}
			if gpuRecordPending && !blankOffered && recovery == nil && stopDeadline == nil && !pendingReboot.Load() && !setupCancelled() &&
				curtainTimes().blankDesktopDue(now, desktopLooks.Load()) {
				blankOffered = true
				if probe, _ := loadRenderProbe(cfg.dir); !gpuDesktopConfirmed(probe, cfg.runtimeID, cfg.displayDriver) {
					logf("display health: GPU rendering has not shown the desktop in the Omarchy window")
					offerRecovery("blank")
				}
			}
			tick++
			if tick%5 == 0 {
				if err := qmp.writeLine(`{"execute":"query-status"}`); err != nil {
					if !postReady {
						procDown = waitExit(exited, 15*time.Second, cfg)
						break
					}
					if !hangReported && stopDeadline == nil {
						hangReported = true
						offerRecovery("qmp")
					}
				}
				if silence.probe() && !hangReported {
					hangReported = true
					if postReady && stopDeadline == nil && !setupCancelled() && !pendingReboot.Load() {
						offerRecovery("qmp")
					} else if !postReady {
						logf("QEMU stopped answering during boot")
						procDown = waitExit(exited, 15*time.Second, cfg)
					}
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
					cleanExit = cleanGuestShutdown(line)
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
	if !postReady && (setupCancelled() || intentionalUpdateQuit.Load()) {
		if err := interruptPendingUpdates(cfg.dir); err != nil {
			logf("recording interrupted first boot: %v", err)
		}
	}
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
	if err := recordGuestExit(cfg.dir, cleanExit, reason); err != nil {
		logf("could not record guest exit: %v", err)
	}
	guestExitedCleanly.Store(cleanExit)
	if windowsSessionEnding.Load() {
		return false
	}
	if reason == "reboot" {
		logf("guest rebooted - relaunching")
		return true
	}
	if reason == "" {
		logf("QEMU exited without a guest shutdown event")
	} else {
		logf("guest powered off (%s)", reason)
	}
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
		if activateRunningInstance() {
			os.Exit(0)
		}
		fatal(uiTextWith("fatal.port.lifecycle", map[string]string{"port": fmt.Sprint(lifecyclePort)}))
	}
	go func() {
		gate := make(chan struct{}, 4)
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			select {
			case gate <- struct{}{}:
				go func() {
					defer func() { <-gate }()
					serveLifecycle(c, loopbackPeerPID, lifecyclePIDFromQEMU, processFromSameUser, func(line string, c net.Conn) {
						if url, ok := parseOpenURLLine(line); ok {
							c.SetWriteDeadline(time.Now().Add(3 * time.Second))
							err := errors.New(uiText("error.links.not_ready"))
							if a := theAgent.Load(); a != nil {
								err = a.requestURLOpen(url)
							}
							if err != nil {
								fmt.Fprintln(c, "error: "+err.Error())
							} else {
								fmt.Fprintln(c, "ok: forwarded")
							}
							return
						}
						switch line {
						case "shutdown":
							guestCompositorHealth.stop()
						case "reboot":
							guestCompositorHealth.stop()
							logf("guest announced reboot")
							pendingReboot.Store(true)
						case "ready":
							logf("guest userspace announced ready")
							guestReady.Store(true)
							guestDesktopReady()
						case "reclaim":
							c.SetWriteDeadline(time.Now().Add(3 * time.Second))
							if err := requestReclaimError(); err != nil {
								fmt.Fprintln(c, "error: "+err.Error())
							} else {
								fmt.Fprintln(c, "ok: Preparing free space. Check Reclaim status in the tray before shutting down.")
							}
						}
					})
				}()
			default:
				c.Close()
			}
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
	if !shouldRecordRenderResult(cfg.runtimeID, cfg.renderMode, cfg.useGpu, cfg.temporaryCPU) {
		return
	}
	result := renderCPU
	if cfg.useGpu {
		result = renderGPU
	}
	// The supervisor records GPU only after the window showed the desktop.
	probe := renderProbe{Result: result, RuntimeID: cfg.runtimeID, DisplayDriver: cfg.displayDriver, RecordedAt: time.Now(), DesktopSeen: cfg.useGpu}
	if err := saveRenderProbe(cfg.dir, probe); err != nil {
		logf("could not record the rendering result: %v", err)
	}
}

// hostResumed is signalled by the tray window when Windows resumes from
// sleep, so the guest clock can be corrected right away.
var hostResumed = make(chan struct{}, 1)

// theAgent is the running launcher's guest agent channel; nil until it is
// listening.
var theAgent atomic.Pointer[guestAgent]

func runGuestAgent(dir string) {
	l, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", agentPort))
	if err != nil {
		logf("agent: port %d unavailable, guest clock sync disabled: %v", agentPort, err)
		return
	}
	logf("agent: listening on %d", agentPort)
	a := newGuestAgent()
	a.peerAllowed = lifecycleConnectionFromQEMU
	a.health = &guestCompositorHealth
	a.appsDir = dir
	a.launchApp = func(id string) error { return launchApprovedWindowsApp(dir, id) }
	a.openFile = func(rel string) error { return openSharedWindowsFile(dir, rel) }
	a.dropDrag = performDropDrag
	theAgent.Store(a)
	a.run(l, hostResumed)
}

// requestReclaim asks the guest to zero its free space so disk.raw can be
// compacted after shutdown. Used by the tray, and by "-reclaim" through the
// lifecycle port.
// reclaimDir is the data directory whose Windows drive bounds a reclaim pass.
var reclaimDir atomic.Pointer[string]
var reclaimSupported atomic.Bool

func requestReclaimError() error {
	if !reclaimSupported.Load() {
		return errors.New(uiText("reclaim.error.unsupported"))
	}
	dir := reclaimDir.Load()
	a := theAgent.Load()
	if dir == nil || a == nil {
		return errors.New(uiText("reclaim.error.not_ready"))
	}
	free, err := diskFreeBytes(*dir)
	if err != nil {
		return uiError(uiTextWith("reclaim.error.free_space", map[string]string{"error": err.Error()}), err)
	}
	budget := reclaimBudgetMiB(free)
	if budget == 0 {
		return errors.New(uiText("reclaim.error.low_space"))
	}
	if !a.requestZeroFill(budget) {
		return errors.New(uiTextWith("reclaim.error.not_started", map[string]string{"status": a.reclaimStatus()}))
	}
	return nil
}

// compactAfterShutdown runs once the guest has powered off, when the disk is
// closed, and only when the guest reported a zero-fill during this session.
func compactAfterShutdown(cfg *config) {
	a := theAgent.Load()
	if a == nil || !a.compactPending() || cfg.diskFormat != "raw" {
		return
	}
	getUI().setStatus("%s", uiText("status.reclaiming"))
	logf("compact: scanning %s", cfg.disk)
	before, beforeErr := platformAllocatedFileBytes(cfg.disk)
	reclaimed, err := compactDisk(cfg.disk, nil)
	if err != nil {
		logf("compact: %v", err)
		infoBox(uiTextWith("reclaim.error", map[string]string{"error": err.Error()}))
		return
	}
	logf("compact: %s of zero blocks turned back into holes", formatGiB(reclaimed))
	after, afterErr := platformAllocatedFileBytes(cfg.disk)
	if beforeErr == nil && afterErr == nil {
		saved := before - after
		if saved < 0 {
			saved = 0
		}
		infoBox(uiTextWith("reclaim.done", map[string]string{"space": formatGiB(saved)}))
	} else {
		infoBox(uiText("reclaim.done_unknown"))
	}
}

// sendOpenURL hands a link to the running launcher, which forwards it to
// Omarchy. It never starts Omarchy, and sends the link only to a listener of
// this Windows user in this session.
func sendOpenURL(dir, url string) int {
	prefs, err := loadApprovedURLSchemes(dir)
	if err != nil {
		errorBox(uiTextWith("control.open_url.failed", map[string]string{"error": uiText("error.links.read_failed")}))
		return 1
	}
	url, ok := validOpenURL(url, prefs.Schemes)
	if !ok {
		errorBox(uiTextWith("control.open_url.failed", map[string]string{"error": uiText("error.links.not_allowed")}))
		return 1
	}
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", lifecyclePort), 3*time.Second)
	if err != nil {
		errorBox(uiText("control.not_running"))
		return 1
	}
	defer c.Close()
	if !connectionFromSameUser(c) {
		errorBox(uiTextWith("control.open_url.failed", map[string]string{"error": uiText("error.links.unverified")}))
		return 1
	}
	c.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := io.WriteString(c, openURLLine(url)); err != nil {
		errorBox(uiTextWith("control.error", map[string]string{"error": err.Error()}))
		return 1
	}
	reply, err := bufio.NewReader(io.LimitReader(c, 4096)).ReadString('\n')
	if err != nil {
		errorBox(uiText("control.open_url.unconfirmed"))
		return 1
	}
	if !strings.HasPrefix(reply, "ok: ") {
		errorBox(uiTextWith("control.open_url.failed", map[string]string{"error": strings.TrimSpace(strings.TrimPrefix(reply, "error: "))}))
		return 1
	}
	return 0
}

// sendLifecycleCommand hands one line to the running launcher on loopback.
func sendLifecycleCommand(command string) int {
	c, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", lifecyclePort), 3*time.Second)
	if err != nil {
		errorBox(uiText("control.not_running"))
		return 1
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := io.WriteString(c, command+"\n"); err != nil {
		errorBox(uiTextWith("control.error", map[string]string{"error": err.Error()}))
		return 1
	}
	if command == "reclaim" {
		reply, err := bufio.NewReader(io.LimitReader(c, 4096)).ReadString('\n')
		if err != nil {
			errorBox(uiText("control.reclaim_unconfirmed"))
			return 1
		}
		if !strings.HasPrefix(reply, "ok: ") {
			errorBox(strings.TrimSpace(strings.TrimPrefix(reply, "error: ")))
			return 1
		}
		infoBox(strings.TrimSpace(strings.TrimPrefix(reply, "ok: ")))
	}
	return 0
}
