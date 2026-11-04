//go:build windows

package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"slices"
	"strconv"
	"strings"
	"syscall"
	"unsafe"
)

// The settings window (-settings): the rows of settings.json as plain Win32
// controls. Preferences retain their existing persistence and live watchers;
// branded native controls preserve Windows keyboard and accessibility roles.

var (
	procRedrawWindow       = user32.NewProc("RedrawWindow")
	procGetKeyState        = user32.NewProc("GetKeyState")
	procGetNextDlgTabItem  = user32.NewProc("GetNextDlgTabItem")
	procIsDialogMessageW   = user32.NewProc("IsDialogMessageW")
	procEnableWindow       = user32.NewProc("EnableWindow")
	procLoadCursorW        = user32.NewProc("LoadCursorW")
	procGetStockObject     = syscall.NewLazyDLL("gdi32.dll").NewProc("GetStockObject")
	procSetFocus           = user32.NewProc("SetFocus")
	procAdjustWindowRectEx = user32.NewProc("AdjustWindowRectEx")
)

const (
	wsCaption                    = 0x00C00000
	wsSysmenu                    = 0x00080000
	wsBorder                     = 0x00800000
	wsTabstop                    = 0x00010000
	wsVscroll                    = 0x00200000
	esAutohscroll                = 0x0080
	esMultiline                  = 0x0004
	esAutovscroll                = 0x0040
	bsAutocheckbox               = 0x0003
	bsDefpushbutton              = 0x0001
	bmGetcheck                   = 0x00F0
	bmSetcheck                   = 0x00F1
	bstChecked                   = 1
	idcArrow                     = 32512
	colorBtnface                 = 15
	defaultGuiFont               = 17
	wmGettextlength              = 0x000E
	wmGettext                    = 0x000D
	settingsPageBase             = 2100
	settingsCameraOnID           = 2110
	settingsMicrophoneOnID       = 2111
	settingsCameraID             = 2112
	settingsUpdateOnID           = 2113
	settingsAboutID              = 2114
	settingsPrivacyID            = 2115
	settingsMicrophonePrivacyID  = 2116
	settingsSoundID              = 2117
	settingsAudioOutputID        = 2118
	settingsAudioInputID         = 2119
	settingsResourceProfileID    = 2120
	settingsStartAutomaticallyID = 2121
	settingsLaunchAtSignInID     = 2122
	settingsAppAddID             = 2123
	settingsAppRemoveID          = 2124
	settingsAppListID            = 2125
	settingsAltTabID             = 2126
	settingsUSBSelectionID       = 2127
	settingsVolumeSyncID         = 2128
	settingsInstallOmarchyID     = 2130
	settingsLinkListID           = 2131
	settingsLinkAllowID          = 2132
	settingsLinkRevokeID         = 2133
	settingsFileListID           = 2134
	settingsFileTypeID           = 2135
	settingsFileOpenerID         = 2136
	settingsFileAddID            = 2137
	settingsFileRemoveID         = 2138
	settingsSaveID               = 2001
	settingsCancelID             = 2002
	settingsBrowseID             = 2003
	settingsFullID               = 2010
	settingsMemID                = 2011
	settingsShareID              = 2012
	settingsFwdID                = 2013
	settingsKeyID                = 2014
	settingsShareOnID            = 2015
	settingsDiskID               = 2016
	settingsBackupID             = 2020
	settingsRestoreID            = 2021
	settingsResetID              = 2022
	settingsRenderAutoID         = 2023
	settingsRenderGPUID          = 2024
	settingsRenderCPUID          = 2025
	settingsCPUsID               = 2026
	settingsUninstallID          = 2027
	settingsMoveID               = 2028
	settingsMoveCleanupID        = 2029
	settingsHelpID               = 2030
	settingsSnapshotsID          = 2031
	settingsPortableID           = 2032
	settingsDisplaysID           = 2033
	settingsLANPublicID          = 2034
	settingsLANAddID             = 2035
	settingsFullscreenDisplayID  = 2036
	bsAutoradiobutton            = 0x0009
	wsGroup                      = 0x00020000
	settingsRecoveryDone         = 0x8010
)

// runSettingsDialog shows the window and returns once it closes. saved is
// true when the file was written.
func runSettingsDialog(path, dataDir string, portable bool) (saved bool) {
	return runLauncherSettings(path, dataDir, portable, false, nil)
}

func runLauncherSettings(path, dataDir string, portable, launcher bool, beforeRelaunch func()) (saved bool) {
	modeFlag := "-settings"
	if launcher {
		modeFlag = "-launcher"
	}
	if !portable {
		if self, err := os.Executable(); err == nil {
			if resolved, err := prepareMovedLocation(filepath.Dir(self), false); err == nil && !pathsEqual(resolved, filepath.Dir(self)) {
				// Transfer launcher ownership before the moved process starts;
				// otherwise it can race this process's deferred mutex close.
				if beforeRelaunch != nil {
					beforeRelaunch()
				}
				cmd := exec.Command(filepath.Join(resolved, stableLauncherName), "-dir", dataDir, modeFlag)
				if err := cmd.Start(); err != nil {
					errorBox(uiTextWith("settings.error.reopen_moved", map[string]string{"error": err.Error()}))
				}
				return false
			}
		}
	}
	runtime.LockOSThread()
	brand := newWindowBrand()
	defer brand.close()
	brand.contentTop = 136
	guard, err := lockMoveStore(hostMoveStore())
	if err != nil {
		errorBox(uiTextWith("settings.error.open", map[string]string{"error": err.Error()}))
		return false
	}
	defer guard.Close()
	if !portable {
		if err := checkMovedSettings(dataDir); err != nil {
			errorBox(uiTextWith("settings.error.open", map[string]string{"error": err.Error()}))
			return false
		}
	}
	current, err := loadSettingsWithRepair(path)
	if err != nil {
		if errors.Is(err, errSetupCancelled) {
			return false
		}
		errorBox(uiTextWith("settings.error.read_settings", map[string]string{"error": err.Error()}))
		return false
	}

	prefs, err := loadDesktopPreferences(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_devices", map[string]string{"error": err.Error()}))
		return false
	}
	launchPrefs, err := loadLaunchPreferences(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_launch", map[string]string{"error": err.Error()}))
		return false
	}
	keyboardPrefs, err := loadKeyboardPreferences(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_keyboard", map[string]string{"error": err.Error()}))
		return false
	}
	audioPrefs, err := loadAudioPreferences(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_audio", map[string]string{"error": err.Error()}))
		return false
	}
	endpointPrefs, err := loadAudioEndpoints(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_endpoints", map[string]string{"error": err.Error()}))
		return false
	}
	audioEndpointDevices, endpointErr := listAudioEndpoints()
	audioPrefs.Output, endpointPrefs.OutputID = resolveAudioSelection(
		audioPrefs.Output, endpointPrefs.OutputID, audioEndpointDevices.Output)
	audioPrefs.Input, endpointPrefs.InputID = resolveAudioSelection(
		audioPrefs.Input, endpointPrefs.InputID, audioEndpointDevices.Input)
	audioQEMU := filepath.Join(dataDir, "runtime", "bin", "qemu-system-x86_64w.exe")
	if f := flag.Lookup("winq"); f != nil && guestDisplayCount(current.Displays) == 1 && !portable {
		candidate := filepath.Join(f.Value.String(), "bin", "qemu-system-x86_64w.exe")
		if info, e := os.Stat(candidate); e == nil && info.Mode().IsRegular() {
			audioQEMU = candidate
		}
	}
	audioSupported := audioRuntimeSupportsSelection(audioQEMU)
	audioLive := audioRuntimeSupportsLiveRouting(audioQEMU)
	audioDevices, audioErr := listAudioDevices(audioQEMU)
	// Keep a disconnected selection intact until the user chooses another.
	retainAudio := func(devices []string, selected string) []string {
		if selected == "" {
			return devices
		}
		for _, name := range devices {
			if name == selected {
				return devices
			}
		}
		return append(devices, selected)
	}
	audioDevices.Output = retainAudio(audioDevices.Output, audioPrefs.Output)
	audioDevices.Input = retainAudio(audioDevices.Input, audioPrefs.Input)
	resourcePrefs, err := loadResourcePreferences(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_resources", map[string]string{"error": err.Error()}))
		return false
	}
	approvedApps, err := loadApprovedWindowsApps(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_apps", map[string]string{"error": err.Error()}))
		return false
	}
	approvedFiles, err := loadApprovedFileTypes(dataDir)
	if err != nil {
		errorBox(uiTextWith("settings.error.read_file_types", map[string]string{"error": err.Error()}))
		// Leave the unreadable file untouched until the user saves Settings.
		approvedFiles = approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{}}
	}
	approvedFiles.Types = slices.Clone(approvedFiles.Types)
	// Portable copies never register link types: the drive can move.
	approvedLinks := approvedURLSchemePreferences{SchemaVersion: 1}
	var linkRows []string
	linkConflicted := map[string]bool{}
	linkDir, linkTarget := dataDir, ""
	if !portable {
		if approvedLinks, err = loadApprovedURLSchemes(dataDir); err == nil {
			linkDir, err = filepath.Abs(dataDir)
		}
		if err != nil {
			errorBox(uiTextWith("settings.error.read_links", map[string]string{"error": err.Error()}))
			return false
		}
		linkTarget = filepath.Join(linkDir, stableLauncherName)
		reported, reportErr := loadGuestURLSchemes(dataDir)
		if reportErr != nil {
			logf("links: ignoring the guest's link types: %v", reportErr)
		}
		// Hide what Windows already handles, but keep an allowed one listed
		// so it can still be revoked.
		for _, scheme := range slices.Sorted(slices.Values(append(slices.Clone(reported), approvedLinks.Schemes...))) {
			if len(linkRows) > 0 && linkRows[len(linkRows)-1] == scheme {
				continue
			}
			linkConflicted[scheme] = urlSchemeHandledByWindows(scheme, linkTarget, linkDir)
			if !linkConflicted[scheme] || slices.Contains(approvedLinks.Schemes, scheme) {
				linkRows = append(linkRows, scheme)
			}
		}
	}
	savedLinks := slices.Clone(approvedLinks.Schemes)
	hostSnapshot := measureHostResources(true)
	runningMiB := runningGuestMiB()
	hostSnapshot.AvailableMiB = min(hostSnapshot.TotalMiB, hostSnapshot.AvailableMiB+runningMiB)
	cameras, cameraErr := listCameraDevices()
	if prefs.CameraID != "" {
		found := false
		for _, d := range cameras {
			if d.ID == prefs.CameraID {
				found = true
			}
		}
		if !found {
			cameras = append(cameras, cameraDevice{prefs.CameraID, uiText("settings.camera.disconnected")})
		}
	}

	storage, err := loadStorageWithRepair(dataDir)
	if err != nil {
		if errors.Is(err, errSetupCancelled) {
			return false
		}
		errorBox(uiTextWith("settings.error.read_storage", map[string]string{"error": err.Error()}))
		return false
	}
	guard.Close()
	hInst, _, _ := procGetModuleHandleW.Call(0)
	className, _ := syscall.UTF16PtrFromString("TryOmarchySettings")
	var hwnd uintptr
	var scroll settingsScroll
	var hFull, hFullscreenDisplay, hStartAutomatically, hLaunchAtSignIn, hAltTab, hMem, hCPUs, hDisk, hShare, hShareOn, hFwd, hKey uintptr
	fullscreenMonitors := hostMonitors()
	fullscreenChoices := []string{""}
	var hRenderAuto, hRenderGPU, hRenderCPU, hDisplays, hLANPublic uintptr
	var hCameraOn, hMicrophoneOn, hCamera, hUpdateOn uintptr
	var hAudioOutput, hAudioInput, hVolumeSync uintptr
	var hResourceProfile, hResourceHelp uintptr
	var hApprovedApps, hLinks, hFiles, hFileType, hFileOpener uintptr
	var availableFileTypes []string
	var updateResourceControls func()
	var refreshApprovedApps, refreshLinks, refreshFiles, refreshFileOpeners func()
	profileValues := []string{resourceBalanced, resourceMaximum, resourceManual}
	selectedProfile := func() string {
		index, _, _ := procSendMessageW.Call(hResourceProfile, 0x147, 0, 0) // CB_GETCURSEL
		if index >= uintptr(len(profileValues)) {
			return ""
		}
		return profileValues[index]
	}
	var selectPage func(int)
	var pages [5][]settingsScrollControl
	var pageHeights [5]int32
	var common []settingsScrollControl
	var footer []settingsScrollControl
	var layout func()
	var currentPage int
	var manualControls []uintptr
	var manualStart, manualEnd int32

	text := func(handle uintptr) string {
		n, _, _ := procSendMessageW.Call(handle, wmGettextlength, 0, 0)
		buf := make([]uint16, n+1)
		procSendMessageW.Call(handle, wmGettext, uintptr(len(buf)), uintptr(unsafe.Pointer(&buf[0])))
		return syscall.UTF16ToString(buf)
	}
	setText := func(handle uintptr, value string) {
		t, _ := syscall.UTF16PtrFromString(value)
		procSendMessageW.Call(handle, wmSettext, 0, uintptr(unsafe.Pointer(t)))
	}
	collect := func() (settings, error) {
		checked, _, _ := procSendMessageW.Call(hFull, bmGetcheck, 0, 0)
		shareChecked, _, _ := procSendMessageW.Call(hShareOn, bmGetcheck, 0, 0)
		render := renderAuto
		if r, _, _ := procSendMessageW.Call(hRenderGPU, bmGetcheck, 0, 0); r == bstChecked {
			render = renderGPU
		} else if r, _, _ := procSendMessageW.Call(hRenderCPU, bmGetcheck, 0, 0); r == bstChecked {
			render = renderCPU
		}
		memory, cpus := strconv.Itoa(current.MemoryMiB), strconv.Itoa(current.CPUs)
		if selectedProfile() == resourceManual {
			var err error
			memory, err = memoryMiBFromGiB(text(hMem))
			if err != nil {
				return settings{}, err
			}
			cpus = text(hCPUs)
		}
		s, err := settingsFromForm(checked == bstChecked, shareChecked == bstChecked,
			memory, cpus, text(hShare), text(hFwd), text(hKey), render)
		if err != nil {
			return s, err
		}
		selectedDisplay, _, _ := procSendMessageW.Call(hFullscreenDisplay, 0x147, 0, 0) // CB_GETCURSEL
		if selectedDisplay >= uintptr(len(fullscreenChoices)) {
			return s, errors.New(uiText("settings.error.fullscreen_display"))
		}
		s.FullscreenDisplay = fullscreenChoices[selectedDisplay]
		// Presets are launch-time intent: current memory pressure (including
		// a running guest) must not prevent saving them for the next boot.
		if selectedProfile() == resourceManual {
			if _, err := planGuestResources(resourceManual, hostSnapshot, render != renderCPU, s.CPUs, s.MemoryMiB, false, false); err != nil {
				return s, err
			}
		}
		s.Displays, err = strconv.Atoi(strings.TrimSpace(text(hDisplays)))
		if err != nil || s.Displays < 1 || s.Displays > maximumGuestDisplays {
			return s, errors.New(uiTextWith("settings.error.displays", map[string]string{"max": strconv.Itoa(maximumGuestDisplays)}))
		}
		checkedLAN, _, _ := procSendMessageW.Call(hLANPublic, bmGetcheck, 0, 0)
		s.LANPublic = checkedLAN == bstChecked
		s.ForwardAdapters = map[string]string{}
		for _, forward := range s.Forwards {
			if adapter := current.ForwardAdapters[forward]; adapter != "" {
				s.ForwardAdapters[forward] = adapter
			}
		}
		return s, s.validate()
	}
	browseFolder := func() {
		if selected, ok := browseForFolder(hwnd, uiText("settings.storage.browse_prompt")); ok {
			setText(hShare, selected)
			procSendMessageW.Call(hShareOn, bmSetcheck, bstChecked, 0)
		}
	}

	launchRecovery := func(action string) {
		self, err := os.Executable()
		if err != nil {
			errorBox(uiTextWith("tray.error.open_window", map[string]string{"error": err.Error()}))
			return
		}
		args := []string{"-dir", dataDir, "-recovery", action}
		if portable {
			args = append(args, "-portable")
		}
		cmd := exec.Command(self, args...)
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
		if err = cmd.Start(); err != nil {
			errorBox(uiTextWith("settings.error.recovery", map[string]string{"error": err.Error()}))
			return
		}
		procAllowSetForeground.Call(uintptr(cmd.Process.Pid))
		procEnableWindow.Call(hwnd, 0)
		go func() { _ = cmd.Wait(); procPostMessageW.Call(hwnd, settingsRecoveryDone, 0, 0) }()
	}

	wndProc := syscall.NewCallback(func(h, msg, wParam, lParam uintptr) uintptr {
		if result, handled := brand.handle(h, msg, wParam, lParam); handled {
			return result
		}
		if scroll.handle(msg, wParam) {
			return 0
		}
		switch msg {
		case 0x0005: // WM_SIZE
			if layout != nil {
				layout()
			}
			return 0
		case 0x0024: // WM_GETMINMAXINFO, dimensions include non-client frame.
			if lParam != 0 {
				limits := (*[10]int32)(unsafe.Pointer(lParam))
				limits[6], limits[7] = 500, 340
			}
			return 0
		case wmCommand:
			if id := wParam & 0xffff; id >= settingsPageBase && id < settingsPageBase+uintptr(len(pages)) {
				if selectPage != nil {
					selectPage(int(id - settingsPageBase))
				}
				return 0
			}
			switch wParam & 0xffff {
			case settingsAppAddID:
				path, ok, err := chooseExecutablePath(hwnd)
				if err != nil {
					errorBox(uiTextWith("settings.error.choose_app", map[string]string{"error": err.Error()}))
				} else if ok {
					if err := approveWindowsExecutable(&approvedApps, path); err != nil {
						errorBox(uiTextWith("settings.error.approve_app", map[string]string{"error": err.Error()}))
					} else {
						refreshApprovedApps()
					}
				}
			case settingsAppRemoveID:
				index, _, _ := procSendMessageW.Call(hApprovedApps, 0x188, 0, 0) // LB_GETCURSEL
				if index < uintptr(len(approvedApps.Apps)) {
					removedID := approvedApps.Apps[index].ID
					approvedFiles.Types = slices.DeleteFunc(approvedFiles.Types, func(entry approvedFileType) bool {
						return entry.App == removedID
					})
					approvedApps.Apps = append(approvedApps.Apps[:index], approvedApps.Apps[index+1:]...)
					refreshApprovedApps()
				}
			case settingsFileAddID:
				typeIndex, _, _ := procSendMessageW.Call(hFileType, 0x147, 0, 0) // CB_GETCURSEL
				appIndex, _, _ := procSendMessageW.Call(hFileOpener, 0x147, 0, 0)
				if typeIndex >= uintptr(len(availableFileTypes)) || appIndex > uintptr(len(approvedApps.Apps)) {
					errorBox(uiText("settings.error.file_choose"))
					return 0
				}
				entry := approvedFileType{Ext: availableFileTypes[typeIndex]}
				if appIndex > 0 {
					app := approvedApps.Apps[appIndex-1]
					if isFileOpenerInterpreter(app.Path) {
						errorBox(uiText("settings.error.file_interpreter"))
						return 0
					}
					// Opening refuses an opener inside the share, so say so now. Settings
					// runs in its own process and only knows the folder chosen here;
					// the open-time check still covers a different running share.
					shareOn, _, _ := procSendMessageW.Call(hShareOn, bmGetcheck, 0, 0)
					if share := strings.TrimSpace(text(hShare)); shareOn == bstChecked && share != "" && pathsOverlapWindows(app.Path, share) {
						errorBox(uiText("settings.error.file_opener_in_share"))
						return 0
					}
					entry.App = app.ID
				}
				approvedFiles.Types = append(approvedFiles.Types, entry)
				refreshFiles()
			case settingsFileRemoveID:
				index, _, _ := procSendMessageW.Call(hFiles, 0x188, 0, 0) // LB_GETCURSEL
				if index < uintptr(len(approvedFiles.Types)) {
					approvedFiles.Types = slices.Delete(approvedFiles.Types, int(index), int(index)+1)
					refreshFiles()
				}
			case settingsLinkAllowID, settingsLinkRevokeID:
				index, _, _ := procSendMessageW.Call(hLinks, 0x188, 0, 0) // LB_GETCURSEL
				if index >= uintptr(len(linkRows)) {
					return 0
				}
				scheme := linkRows[index]
				allowed := slices.Index(approvedLinks.Schemes, scheme)
				if wParam&0xffff == settingsLinkRevokeID && allowed >= 0 {
					approvedLinks.Schemes = slices.Delete(slices.Clone(approvedLinks.Schemes), allowed, allowed+1)
				} else if wParam&0xffff == settingsLinkAllowID && allowed < 0 {
					if len(approvedLinks.Schemes) >= maximumURLSchemes {
						errorBox(uiTextWith("settings.error.allow_link", map[string]string{"error": uiTextWith("error.links.limit", map[string]string{"count": fmt.Sprint(maximumURLSchemes)})}))
						return 0
					}
					approvedLinks.Schemes = append(slices.Clone(approvedLinks.Schemes), scheme)
				}
				refreshLinks()
				procSendMessageW.Call(hLinks, 0x186, index, 0) // LB_SETCURSEL
			case settingsRenderAutoID, settingsRenderGPUID, settingsRenderCPUID:
				if wParam>>16 == 0 && updateResourceControls != nil { // BN_CLICKED
					updateResourceControls()
				}
			case settingsResourceProfileID:
				if wParam>>16 == 1 && updateResourceControls != nil { // CBN_SELCHANGE
					updateResourceControls()
				}
			case settingsAboutID:
				self, err := os.Executable()
				if err == nil {
					cmd := exec.Command(self, "-about")
					cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
					err = cmd.Start()
					if err == nil {
						_ = cmd.Process.Release()
					}
				}
				if err != nil {
					errorBox(uiTextWith("tray.error.open_window", map[string]string{"error": err.Error()}))
				}
			case settingsUSBSelectionID:
				self, e := os.Executable()
				if e == nil {
					cmd := exec.Command(self, "-dir", dataDir, "-usb-selection", "-winq", filepath.Dir(filepath.Dir(audioQEMU)))
					configureDiskTool(cmd)
					e = cmd.Start()
					if e == nil {
						cmd.Process.Release()
					}
				}
				if e != nil {
					errorBox(uiTextWith("settings.error.usb", map[string]string{"error": e.Error()}))
				}
			case settingsMicrophonePrivacyID:
				openWindowsURL("ms-settings:privacy-microphone")
			case settingsSoundID:
				openWindowsURL("ms-settings:sound")
			case settingsPrivacyID:
				openWindowsURL("ms-settings:privacy-webcam")
			case settingsHelpID:
				infoBox(uiText("help.everyday"))
			case settingsSaveID:
				guard, err := lockMoveStore(hostMoveStore())
				if err != nil {
					errorBox(uiTextWith("settings.error.save", map[string]string{"error": err.Error()}))
					return 0
				}
				defer guard.Close()
				if !portable {
					if err := checkMovedSettings(dataDir); err != nil {
						errorBox(uiTextWith("settings.error.save", map[string]string{"error": err.Error()}))
						return 0
					}
				}
				s, err := collect()
				diskGiB := storage.DiskGiB
				if err == nil {
					diskGiB, err = parseDiskGiB(text(hDisk))
				}
				if err == nil && s.activeShare() != "" {
					home, homeErr := os.UserHomeDir()
					if homeErr != nil {
						err = fmt.Errorf("finding the Windows home folder: %w", homeErr)
					} else {
						s.Share, err = validateWindowsSharedFolder(s.Share, dataDir, home)
					}
				}
				if err == nil {
					err = saveSettings(path, s)
				}
				if err == nil && diskGiB != storage.DiskGiB {
					if storageErr := saveStorageSettings(dataDir, diskGiB); storageErr != nil {
						errorBox(uiTextWith("settings.error.save_disk", map[string]string{"error": storageErr.Error()}))
						return 0
					}
				}
				if err != nil {
					errorBox(uiTextWith("settings.error.cannot_save", map[string]string{"error": err.Error()}))
					return 0
				}
				if err == nil {
					updated := prefs
					v, _, _ := procSendMessageW.Call(hCameraOn, bmGetcheck, 0, 0)
					updated.CameraDisabled = v != bstChecked
					v, _, _ = procSendMessageW.Call(hMicrophoneOn, bmGetcheck, 0, 0)
					updated.MicrophoneDisabled = v != bstChecked
					v, _, _ = procSendMessageW.Call(hVolumeSync, bmGetcheck, 0, 0)
					updated.VolumeSyncDisabled = v != bstChecked
					v, _, _ = procSendMessageW.Call(hUpdateOn, bmGetcheck, 0, 0)
					updated.AutomaticUpdatesDisabled = v != bstChecked
					index, _, _ := procSendMessageW.Call(hCamera, 0x147, 0, 0)
					if index == 0 {
						updated.CameraID = ""
					} else if index <= uintptr(len(cameras)) {
						updated.CameraID = cameras[index-1].ID
					} else {
						errorBox(uiText("settings.error.choose_camera"))
						return 0
					}
					if err = saveDesktopPreferences(dataDir, updated); err != nil {
						errorBox(uiTextWith("settings.error.save_devices", map[string]string{"error": err.Error()}))
						return 0
					}
					v, _, _ = procSendMessageW.Call(hStartAutomatically, bmGetcheck, 0, 0)
					updatedLaunch := launchPrefs
					updatedLaunch.StartAutomatically = v == bstChecked
					v, _, _ = procSendMessageW.Call(hLaunchAtSignIn, bmGetcheck, 0, 0)
					updatedLaunch.LaunchAtSignIn = v == bstChecked
					if !portable {
						target := filepath.Join(dataDir, stableLauncherName)
						if err = syncSignInShortcut(target, dataDir, updatedLaunch.LaunchAtSignIn); err != nil {
							errorBox(uiTextWith("settings.error.save_sign_in", map[string]string{"error": err.Error()}))
							return 0
						}
					}
					if err = saveLaunchPreferences(dataDir, updatedLaunch); err != nil {
						if !portable {
							if rollbackErr := syncSignInShortcut(filepath.Join(dataDir, stableLauncherName), dataDir, launchPrefs.LaunchAtSignIn); rollbackErr != nil {
								logf("restoring sign-in shortcut after settings save failed: %v", rollbackErr)
							}
						}
						errorBox(uiTextWith("settings.error.save_startup", map[string]string{"error": err.Error()}))
						return 0
					}
					if !portable {
						target := filepath.Join(dataDir, stableLauncherName)
						if err = updateLaunchShortcuts(target, dataDir, updatedLaunch.StartAutomatically); err != nil {
							errorBox(uiTextWith("settings.error.save_shortcuts", map[string]string{"error": err.Error()}))
							return 0
						}
					}
				}
				if audioSupported {
					microphoneCheck, _, _ := procSendMessageW.Call(hMicrophoneOn, bmGetcheck, 0, 0)
					microphoneDisabled := microphoneCheck != bstChecked
					updated := audioPrefs
					updatedEndpoints := endpointPrefs
					for _, row := range []struct {
						control        uintptr
						names          []string
						value          *string
						rememberedName string
						endpointValue  *string
						rememberedID   string
						endpoints      []audioEndpointInfo
					}{
						{hAudioOutput, audioDevices.Output, &updated.Output, audioPrefs.Output, &updatedEndpoints.OutputID, endpointPrefs.OutputID, audioEndpointDevices.Output},
						{hAudioInput, audioDevices.Input, &updated.Input, audioPrefs.Input, &updatedEndpoints.InputID, endpointPrefs.InputID, audioEndpointDevices.Input},
					} {
						index, _, _ := procSendMessageW.Call(row.control, 0x147, 0, 0)
						if index > uintptr(len(row.names)) {
							errorBox(uiText("settings.error.choose_audio"))
							return 0
						}
						*row.value = ""
						if index > 0 {
							*row.value = row.names[index-1]
						}
						*row.endpointValue = endpointIDForSelection(
							*row.value, row.rememberedName, row.rememberedID, row.endpoints)
					}
					if err := saveAudioSelection(dataDir, updated, updatedEndpoints); err != nil {
						errorBox(uiTextWith("settings.error.save_audio", map[string]string{"error": err.Error()}))
						return 0
					}
					if audioLive {
						if err := publishSavedAudioRoutes(dataDir, updated, microphoneDisabled); err != nil {
							errorBox(uiTextWith("settings.error.send_audio", map[string]string{"error": err.Error()}))
							return 0
						}
					}
				}
				if err := saveResourcePreferences(dataDir, selectedProfile()); err != nil {
					errorBox(uiTextWith("settings.error.save_profile", map[string]string{"error": err.Error()}))
					return 0
				}
				if err := saveApprovedWindowsApps(dataDir, approvedApps); err != nil {
					errorBox(uiTextWith("settings.error.save_apps", map[string]string{"error": err.Error()}))
					return 0
				}
				if err := saveApprovedFileTypes(dataDir, approvedFiles); err != nil {
					errorBox(uiTextWith("settings.error.save_file_types", map[string]string{"error": err.Error()}))
					return 0
				}
				if !portable {
					if !slices.Equal(approvedLinks.Schemes, savedLinks) {
						if err := saveApprovedURLSchemes(dataDir, approvedLinks); err != nil {
							errorBox(uiTextWith("settings.error.save_links", map[string]string{"error": err.Error()}))
							return 0
						}
						savedLinks = slices.Clone(approvedLinks.Schemes)
					}
					// The file is saved; the next normal start registers again.
					if err := syncURLSchemeRegistrations(linkDir, linkTarget, approvedLinks.Schemes); err != nil {
						errorBox(uiTextWith("settings.error.register_links", map[string]string{"error": err.Error()}))
					}
				}
				// Only write the file for a real change. Older launchers reject
				// backups that contain files they do not know.
				altTabCheck, _, _ := procSendMessageW.Call(hAltTab, bmGetcheck, 0, 0)
				if toWindows := altTabCheck != bstChecked; toWindows != keyboardPrefs.AltTabToWindows {
					if err := saveKeyboardPreferences(dataDir, keyboardPreferences{AltTabToWindows: toWindows}); err != nil {
						errorBox(uiTextWith("settings.error.save_alt_tab", map[string]string{"error": err.Error()}))
						return 0
					}
				}
				saved = true
				procDestroyWindow.Call(h)
			case settingsCancelID, idCancel:
				procDestroyWindow.Call(h)
			case settingsBrowseID:
				browseFolder()
			case settingsMoveID:
				launchRecovery("move")
			case settingsMoveCleanupID:
				launchRecovery("move-cleanup")
			case settingsLANAddID:
				value, err := chooseLANForward(hwnd)
				if err != nil {
					errorBox(uiTextWith("tray.error.open_window", map[string]string{"error": err.Error()}))
					break
				}
				if value.Forward != "" {
					lines := append(strings.Fields(text(hFwd)), value.Forward)
					var forwards forwardList
					for _, line := range lines {
						if err = forwards.Set(line); err != nil {
							break
						}
					}
					if err != nil {
						errorBox(uiTextWith("lan.error.invalid", map[string]string{"error": err.Error()}))
					} else {
						setText(hFwd, strings.Join(lines, "\r\n"))
						if value.Adapter != "" {
							if current.ForwardAdapters == nil {
								current.ForwardAdapters = map[string]string{}
							}
							current.ForwardAdapters[value.Forward] = value.Adapter
						}
					}
				}
			case settingsPortableID:
				launchRecovery("portable-create")
			case settingsSnapshotsID:
				launchRecovery("snapshots")
			case settingsBackupID:
				launchRecovery("backup")
			case settingsRestoreID:
				launchRecovery("restore")
			case settingsResetID:
				launchRecovery("reset")
			case settingsUninstallID:
				launchRecovery("uninstall")
			case settingsInstallOmarchyID:
				launchRecovery("install-omarchy")
			}
			return 0
		case settingsRecoveryDone:
			if !portable {
				if resolved, err := prepareMovedLocation(dataDir, false); err == nil && !pathsEqual(resolved, dataDir) {
					if beforeRelaunch != nil {
						beforeRelaunch()
					}
					cmd := exec.Command(filepath.Join(resolved, stableLauncherName), "-dir", resolved, modeFlag)
					if err := cmd.Start(); err != nil {
						errorBox(uiTextWith("settings.error.open_moved", map[string]string{"path": resolved, "error": err.Error()}))
					}
					procDestroyWindow.Call(h)
					return 0
				}
			}
			procEnableWindow.Call(h, 1)
			procSetForegroundWindow.Call(h)
			return 0
		case wmClose:
			procDestroyWindow.Call(h)
			return 0
		case wmDestroy:
			procPostQuitMessage.Call(0)
			return 0
		}
		r, _, _ := procDefWindowProcW.Call(h, msg, wParam, lParam)
		return r
	})

	type wndclassex struct {
		size, style         uint32
		wndProc             uintptr
		clsExtra, wndExtra  int32
		inst                uintptr
		icon, cursor, brush uintptr
		menuName, className *uint16
		iconSm              uintptr
	}
	cursor, _, _ := procLoadCursorW.Call(0, idcArrow)
	icon, _, _ := procLoadIconW.Call(hInst, 1)
	wc := wndclassex{size: uint32(unsafe.Sizeof(wndclassex{})), wndProc: wndProc, inst: hInst,
		icon: icon, cursor: cursor, brush: colorBtnface + 1, className: className, iconSm: icon}
	if atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc))); atom == 0 {
		logf("settings: RegisterClassExW failed: %v", err)
		return false
	}

	const clientW, clientH = 560, 710
	rect := [4]int32{0, 0, clientW, clientH}
	style := uintptr(wsCaption | wsSysmenu | wsVscroll | 0x02000000 | 0x00040000) // WS_THICKFRAME
	procAdjustWindowRectEx.Call(uintptr(unsafe.Pointer(&rect[0])), style, 0, 0)
	// AdjustWindowRectEx excludes the vertical scrollbar from its calculation.
	scrollbarWidth, _, _ := procGetSystemMetrics.Call(2) // SM_CXVSCROLL
	w, hgt := rect[2]-rect[0]+int32(scrollbarWidth), rect[3]-rect[1]
	sx, _, _ := procGetSystemMetrics.Call(smCxscreen)
	sy, _, _ := procGetSystemMetrics.Call(smCyscreen)
	work := [4]int32{0, 0, int32(sx), int32(sy)}
	procSystemParametersInfoW.Call(0x30, 0, uintptr(unsafe.Pointer(&work[0])), 0)
	if available := work[3] - work[1] - 16; hgt > available {
		hgt = available
	}
	scroll.top = brand.contentTop
	scroll.height = hgt - (rect[3] - rect[1] - clientH) - scroll.top - 104
	scroll.content = clientH
	x := work[0] + (work[2]-work[0]-w)/2
	yWindow := work[1] + (work[3]-work[1]-hgt)/2
	windowTitle := uiText("settings.window_title")
	if launcher {
		windowTitle = uiText("brand.name")
	}
	title, _ := syscall.UTF16PtrFromString(windowTitle)
	var err2 error
	hwnd, _, err2 = procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(className)), uintptr(unsafe.Pointer(title)),
		style|wsVisible, uintptr(x), uintptr(yWindow), uintptr(w), uintptr(hgt), 0, 0, hInst, 0)
	if hwnd == 0 {
		logf("settings: CreateWindowExW failed: %v", err2)
		return false
	}

	scroll.window = hwnd
	scroll.viewport = createSettingsViewport(hwnd, hInst)
	if scroll.viewport == 0 {
		procDestroyWindow.Call(hwnd)
		return false
	}
	brand.window(hwnd)
	brand.panelControls[scroll.viewport] = true
	font := brand.font
	var headerIcon uintptr
	addHeader := func(label string, x, y, w, h int32, style uintptr) uintptr {
		class, _ := syscall.UTF16PtrFromString("STATIC")
		text, _ := syscall.UTF16PtrFromString(label)
		control, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(class)), uintptr(unsafe.Pointer(text)), wsChild|wsVisible|style, uintptr(x), uintptr(y), uintptr(w), uintptr(h), hwnd, 0, hInst, 0)
		procSendMessageW.Call(control, wmSetfont, font, 1)
		return control
	}
	headerIcon, _, _ = procLoadImageW.Call(hInst, 1, imageIcon, 44, 44, 0)
	defer procDestroyIcon.Call(headerIcon)
	headerImage := addHeader("", 24, 22, 44, 44, 3)
	procSendMessageW.Call(headerImage, 0x0172, imageIcon, headerIcon)
	heading := addHeader(uiText("brand.name"), 84, 20, 340, 34, ssNoprefix)
	procSendMessageW.Call(heading, wmSetfont, brand.heading, 1)
	addHeader("WINDOWS  ·  "+currentVersion, 86, 57, 380, 24, ssNoprefix)
	bodyControls := false
	mk := func(class, label string, x, y, cx, cy int32, style, id uintptr) uintptr {
		c, _ := syscall.UTF16PtrFromString(class)
		t, _ := syscall.UTF16PtrFromString(label)
		parent, positionY := hwnd, y
		if bodyControls {
			parent, positionY = scroll.viewport, y-scroll.top
		}
		h, _, _ := procCreateWindowExW.Call(0, uintptr(unsafe.Pointer(c)), uintptr(unsafe.Pointer(t)),
			wsChild|wsVisible|style, uintptr(x), uintptr(positionY), uintptr(cx), uintptr(cy), parent, id, hInst, 0)
		brand.control(h, class, style)
		guardWheel(h, class, style)
		if bodyControls {
			brand.panelControls[h] = true
		}
		scroll.controls = append(scroll.controls, settingsScrollControl{h, x, y, cx, cy})
		return h
	}
	// The sizes below are what English needs. A longer translation makes its
	// control taller or wider and moves the rows under it down, instead of
	// being cut off.
	const left, checkboxTextInset = 24, 20
	measure := func(text string, width int32) (int32, int32) { return measureText(hwnd, font, text, width) }
	// The label column is as wide as its longest label, within reason.
	labelW := int32(150)
	for _, name := range []string{
		uiText("settings.display.fullscreen_display"), uiText("settings.resources.profile"), uiText("settings.resources.memory"),
		uiText("settings.resources.cpus"), uiText("settings.storage.capacity"), uiText("settings.storage.location"),
		uiText("settings.storage.shared_folder"), uiText("settings.camera.camera"), uiText("settings.sound.output"),
		uiText("settings.sound.microphone"), uiText("settings.graphics.displays"), uiText("settings.graphics.rendering"),
		uiText("settings.network.forwards"), uiText("settings.network.ssh_key"),
	} {
		width, _ := measure(name, 0)
		labelW = max(labelW, min(width+8, 230))
	}
	fieldX := left + labelW + 10
	fieldW := clientW - fieldX - 36
	// fieldLabel names a field in the label column and returns how much
	// taller than cy its text needs to be.
	fieldLabel := func(text string, y, cy int32) int32 {
		_, height := measure(text, labelW)
		height = max(cy, height)
		mk("STATIC", text, left, y+3, labelW, height, ssNoprefix, 0)
		return height - cy
	}
	// note is a wrapped paragraph at least cy tall. It returns the y below
	// it: y+advance for English, more for a longer translation.
	note := func(text string, x, y, w, cy, advance int32) int32 {
		_, height := measure(text, w)
		height = max(cy, height)
		mk("STATIC", text, x, y, w, height, ssNoprefix, 0)
		return y + advance + height - cy
	}
	// check is a checkbox across the rest of the row whose label wraps when
	// it does not fit on one line. It returns how much taller than cy it is.
	check := func(text string, x, y, cy int32, id uintptr) (uintptr, int32) {
		w := clientW - left - x
		_, height := measure(text, w-checkboxTextInset)
		style := uintptr(bsAutocheckbox | wsTabstop)
		if height > cy {
			style |= bsMultiline
		}
		height = max(cy, height)
		return mk("BUTTON", text, x, y, w, height, style, id), height - cy
	}
	// buttons lays out a row of buttons, each at least its English width,
	// and moves the rest to a new line when a translation does not fit. It
	// returns the buttons and how much lower the row ends.
	type rowButton struct {
		text  string
		width int32
		id    uintptr
	}
	buttons := func(x, y, h, gap int32, row ...rowButton) ([]uintptr, int32) {
		handles := make([]uintptr, 0, len(row))
		start, grow := x, int32(0)
		for _, b := range row {
			width, _ := measure(b.text, 0)
			width = max(b.width, width+24)
			if x > start && x+width > clientW-left {
				x, grow = start, grow+h+8
			}
			handles = append(handles, mk("BUTTON", b.text, x, y+grow, width, h, wsTabstop, b.id))
			x += width + gap
		}
		return handles, grow
	}
	// grid lays out a set of equal buttons width wide, as many to a row as
	// fit and the rows balanced, so four become two rows of two rather
	// than three and one. It returns the buttons and how much lower the
	// grid ends than one row.
	grid := func(x, y, h, gap, width int32, cells ...rowButton) ([]uintptr, int32) {
		columns := max(1, min(int32(len(cells)), (clientW-left-x+gap)/(width+gap)))
		rows := (int32(len(cells)) + columns - 1) / columns
		columns = (int32(len(cells)) + rows - 1) / rows
		handles := make([]uintptr, 0, len(cells))
		for i, cell := range cells {
			column, row := int32(i)%columns, int32(i)/columns
			handles = append(handles, mk("BUTTON", cell.text, x+column*(width+gap), y+row*(h+8), width, h, wsTabstop, cell.id))
		}
		return handles, (rows - 1) * (h + 8)
	}
	for i, label := range []string{uiText("settings.tab.general"), uiText("settings.tab.devices"), uiText("settings.tab.advanced"),
		uiText("settings.tab.recovery"), uiText("settings.tab.apps")} {
		mk("BUTTON", label, 16+int32(i)*106, 94, 102, 30, wsTabstop, settingsPageBase+uintptr(i))
	}
	common = append(common, scroll.controls...)
	scroll.controls = nil
	bodyControls = true
	y := scroll.top
	section := func(label string) {
		if y > scroll.top {
			y += 16
		}
		_, height := measure(label, clientW-32)
		height = max(24, height)
		h := mk("STATIC", label, left, y, clientW-32, height, ssNoprefix, 0)
		brand.sectionControls[h] = true
		y += 32 + height - 24
	}
	var grow int32
	section(uiText("settings.section.display"))
	hFull, grow = check(uiText("settings.display.fullscreen"), left, y, 22, settingsFullID)
	if current.Fullscreen {
		procSendMessageW.Call(hFull, bmSetcheck, bstChecked, 0)
	}
	y += 30 + grow
	grow = fieldLabel(uiText("settings.display.fullscreen_display"), y, 20)
	hFullscreenDisplay = mk("COMBOBOX", "", fieldX, y, fieldW, 180, 0x0003|wsVscroll|wsTabstop, settingsFullscreenDisplayID)
	addDisplay := func(label, name string) {
		value, _ := syscall.UTF16PtrFromString(label)
		procSendMessageW.Call(hFullscreenDisplay, 0x143, 0, uintptr(unsafe.Pointer(value))) // CB_ADDSTRING
		fullscreenChoices = append(fullscreenChoices, name)
	}
	value, _ := syscall.UTF16PtrFromString(uiText("settings.display.primary_automatic"))
	procSendMessageW.Call(hFullscreenDisplay, 0x143, 0, uintptr(unsafe.Pointer(value)))
	selectedDisplay := 0
	for _, monitor := range fullscreenMonitors {
		values := map[string]string{"name": monitor.Name, "width": strconv.Itoa(int(monitor.Bounds.width())), "height": strconv.Itoa(int(monitor.Bounds.height()))}
		label := uiTextWith("settings.display.monitor", values)
		if monitor.Primary {
			label = uiTextWith("settings.display.monitor_primary", values)
		}
		addDisplay(label, monitor.Name)
		if monitor.Name == current.FullscreenDisplay {
			selectedDisplay = len(fullscreenChoices) - 1
		}
	}
	if current.FullscreenDisplay != "" && selectedDisplay == 0 {
		addDisplay(uiTextWith("settings.display.disconnected", map[string]string{"name": current.FullscreenDisplay}), current.FullscreenDisplay)
		selectedDisplay = len(fullscreenChoices) - 1
	}
	procSendMessageW.Call(hFullscreenDisplay, 0x14e, uintptr(selectedDisplay), 0) // CB_SETCURSEL
	y += 34 + grow
	hStartAutomatically, grow = check(uiText("settings.display.start_automatically"), left, y, 22, settingsStartAutomaticallyID)
	if launchPrefs.StartAutomatically {
		procSendMessageW.Call(hStartAutomatically, bmSetcheck, bstChecked, 0)
	}
	if portable {
		procEnableWindow.Call(hStartAutomatically, 0)
	}
	y += 30 + grow
	hLaunchAtSignIn, grow = check(uiText("settings.display.launch_at_sign_in"), left, y, 22, settingsLaunchAtSignInID)
	if launchPrefs.LaunchAtSignIn {
		procSendMessageW.Call(hLaunchAtSignIn, bmSetcheck, bstChecked, 0)
	}
	if portable {
		procEnableWindow.Call(hLaunchAtSignIn, 0)
	}
	y += 30 + grow
	hAltTab, grow = check(uiText("settings.display.alt_tab"), left, y, 22, settingsAltTabID)
	if !keyboardPrefs.AltTabToWindows {
		procSendMessageW.Call(hAltTab, bmSetcheck, bstChecked, 0)
	}
	y += 30 + grow
	section(uiText("settings.section.resources"))
	grow = fieldLabel(uiText("settings.resources.profile"), y, 20)
	hResourceProfile = mk("COMBOBOX", "", fieldX, y, fieldW, 130, 0x0003|wsVscroll|wsTabstop, settingsResourceProfileID)
	for i, label := range []string{uiText("settings.resources.balanced"), uiText("settings.resources.maximum"), uiText("settings.resources.manual")} {
		t, _ := syscall.UTF16PtrFromString(label)
		procSendMessageW.Call(hResourceProfile, 0x143, 0, uintptr(unsafe.Pointer(t))) // CB_ADDSTRING
		if profileValues[i] == effectiveResourceProfile(resourcePrefs.Profile, current.CPUs, current.MemoryMiB) {
			procSendMessageW.Call(hResourceProfile, 0x14e, uintptr(i), 0) // CB_SETCURSEL
		}
	}
	y += 34 + grow
	manualStart = y
	manualIndex := len(scroll.controls)
	// A hint beside a short field; returns how much taller than a line it is.
	hint := func(text string, y int32) int32 {
		_, height := measure(text, fieldW-112)
		height = max(20, height)
		mk("STATIC", text, fieldX+112, y+3, fieldW-112, height, ssNoprefix, 0)
		return height - 20
	}
	grow = fieldLabel(uiText("settings.resources.memory"), y, 20)
	hMem = mk("EDIT", memoryGiBText(current.MemoryMiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsMemID)
	grow = max(grow, hint(uiText("settings.resources.memory_hint"), y))
	y += 34 + grow
	grow = fieldLabel(uiText("settings.resources.cpus"), y, 20)
	hCPUs = mk("EDIT", strconv.Itoa(current.CPUs), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsCPUsID)
	grow = max(grow, hint(uiTextWith("settings.resources.cpus_hint", map[string]string{"count": strconv.Itoa(min(maximumGuestCPUs, hostSnapshot.LogicalCPUs))}), y))
	y += 34 + grow
	manualEnd = y
	for _, c := range scroll.controls[manualIndex:] {
		manualControls = append(manualControls, c.handle)
	}
	resourceHelp := func(profile string) string {
		gib := func(mib int) string { return strconv.FormatFloat(float64(mib)/1024, 'f', 1, 64) }
		help := uiText("settings.resources.manual_help")
		if profile != resourceManual {
			checked, _, _ := procSendMessageW.Call(hRenderCPU, bmGetcheck, 0, 0)
			gpu := settingsEstimateGPU(current.Render, hRenderCPU, checked)
			plan, err := planGuestResources(profile, hostSnapshot, gpu, 0, 0, false, false)
			estimate := map[string]string{"cpus": strconv.Itoa(plan.CPUs), "memory": gib(plan.MemoryMiB)}
			switch {
			case err != nil:
				help = err.Error()
			case profile == resourceMaximum:
				help = uiTextWith("settings.resources.estimate_maximum", estimate)
			default:
				help = uiTextWith("settings.resources.estimate", estimate)
			}
		}
		pc := map[string]string{"cpus": strconv.Itoa(hostSnapshot.LogicalCPUs), "total": gib(hostSnapshot.TotalMiB), "available": gib(hostSnapshot.AvailableMiB)}
		if runningMiB > 0 {
			return help + "\n" + uiTextWith("settings.resources.pc_running", pc)
		}
		return help + "\n" + uiTextWith("settings.resources.pc", pc)
	}
	// The help changes with the profile; make room for the longest.
	resourceHelpHeight := int32(76)
	for _, profile := range profileValues {
		_, height := measure(resourceHelp(profile), clientW-2*left)
		resourceHelpHeight = max(resourceHelpHeight, height)
	}
	hResourceHelp = mk("STATIC", "", left, y, clientW-2*left, resourceHelpHeight, ssNoprefix, 0)
	updateResourceControls = func() {
		profile := selectedProfile()
		enabled := uintptr(0)
		if profile == resourceManual {
			enabled = 1
		}
		procEnableWindow.Call(hMem, enabled)
		procEnableWindow.Call(hCPUs, enabled)
		setText(hResourceHelp, resourceHelp(profile))
		if layout != nil {
			layout()
		}
	}
	updateResourceControls()
	y += 8 + resourceHelpHeight
	section(uiText("settings.section.storage"))
	grow = fieldLabel(uiText("settings.storage.capacity"), y, 20)
	hDisk = mk("EDIT", strconv.Itoa(storage.DiskGiB), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsDiskID)
	y += 28 + grow
	y = note(uiText("settings.storage.capacity_help"), left, y, clientW-2*left, 36, 38)
	var status []string
	if disk, err := inspectInstallationDisk(dataDir); err == nil {
		status = append(status, uiTextWith("settings.storage.current_capacity", map[string]string{"size": formatGiB(disk.VirtualBytes)}))
	}
	if available, err := diskFreeBytes(dataDir); err == nil {
		status = append(status, uiTextWith("settings.storage.free_space", map[string]string{"size": formatGiB(available)}))
	}
	y = note(strings.Join(status, " "), left, y, clientW-2*left, 20, 26)
	grow = fieldLabel(uiText("settings.storage.location"), y, 20)
	mk("EDIT", dataDir, fieldX, y, fieldW, 22, wsBorder|wsTabstop|esAutohscroll|0x0800, 0) // ES_READONLY; long paths remain selectable.
	y += 24 + grow
	// The two-line label spans the folder field and the checkbox under it.
	shareLabelBottom := y + 3 + 32 + fieldLabel(uiText("settings.storage.shared_folder"), y, 32)
	browseText := uiText("settings.storage.browse")
	browseW, _ := measure(browseText, 0)
	browseW = max(72, browseW+24)
	hShare = mk("EDIT", current.Share, fieldX, y, fieldW-browseW-8, 24, wsBorder|wsTabstop|esAutohscroll, settingsShareID)
	mk("BUTTON", browseText, fieldX+fieldW-browseW, y, browseW, 24, wsTabstop, settingsBrowseID)
	y += 28
	hShareOn, grow = check(uiText("settings.storage.share_allow"), fieldX, y, 22, settingsShareOnID)
	if current.Share != "" && !current.ShareDisabled {
		procSendMessageW.Call(hShareOn, bmSetcheck, bstChecked, 0)
	}
	y = max(y+34+grow, shareLabelBottom+8)
	pages[0] = append(pages[0], scroll.controls...)
	pageHeights[0] = y
	scroll.controls = nil
	y = scroll.top
	section(uiText("settings.section.camera"))
	hCameraOn, grow = check(uiText("settings.camera.allow"), left, y, 24, settingsCameraOnID)
	if !prefs.CameraDisabled {
		procSendMessageW.Call(hCameraOn, bmSetcheck, bstChecked, 0)
	}
	y += 34 + grow
	grow = fieldLabel(uiText("settings.camera.camera"), y, 24)
	hCamera = mk("COMBOBOX", "", fieldX, y, fieldW, 180, 0x0003|wsVscroll|wsTabstop, settingsCameraID)
	addCamera := func(label string) {
		t, _ := syscall.UTF16PtrFromString(label)
		procSendMessageW.Call(hCamera, 0x143, 0, uintptr(unsafe.Pointer(t)))
	}
	addCamera(uiText("settings.camera.automatic"))
	selected := 0
	for i, d := range cameras {
		addCamera(d.Name)
		if d.ID == prefs.CameraID {
			selected = i + 1
		}
	}
	procSendMessageW.Call(hCamera, 0x14E, uintptr(selected), 0)
	y += 38 + grow
	cameraHelp := uiText("settings.camera.help")
	if cameraErr != nil {
		cameraHelp = uiText("settings.camera.list_error")
	} else if len(cameras) == 0 {
		cameraHelp = uiText("settings.camera.none")
	}
	y = note(cameraHelp, left, y, 450, 42, 50)
	hMicrophoneOn, grow = check(uiText("settings.microphone.allow"), left, y, 24, settingsMicrophoneOnID)
	if !prefs.MicrophoneDisabled {
		procSendMessageW.Call(hMicrophoneOn, bmSetcheck, bstChecked, 0)
	}
	y += 34 + grow
	y = note(uiText("settings.microphone.help"), left, y, 450, 42, 50)
	addAudioCombo := func(label string, id uintptr, names []string, value string) uintptr {
		grow := fieldLabel(label, y, 24)
		h := mk("COMBOBOX", "", fieldX, y, fieldW, 180, 0x0003|wsVscroll|wsTabstop, id)
		selected := 0
		for i, name := range append([]string{uiText("settings.sound.windows_default")}, names...) {
			t, _ := syscall.UTF16PtrFromString(name)
			procSendMessageW.Call(h, 0x143, 0, uintptr(unsafe.Pointer(t)))
			if i > 0 && name == value {
				selected = i
			}
		}
		procSendMessageW.Call(h, 0x14E, uintptr(selected), 0)
		if !audioSupported {
			procEnableWindow.Call(h, 0)
		}
		y += 38 + grow
		return h
	}
	section(uiText("settings.section.sound"))
	hAudioOutput = addAudioCombo(uiText("settings.sound.output"), settingsAudioOutputID, audioDevices.Output, audioPrefs.Output)
	hAudioInput = addAudioCombo(uiText("settings.sound.microphone"), settingsAudioInputID, audioDevices.Input, audioPrefs.Input)
	hVolumeSync, grow = check(uiText("settings.sound.sync_volume"), left, y, 24, settingsVolumeSyncID)
	if !prefs.VolumeSyncDisabled {
		procSendMessageW.Call(hVolumeSync, bmSetcheck, bstChecked, 0)
	}
	y += 28 + grow
	audioHelp := uiText("settings.sound.help")
	if !audioSupported {
		audioHelp = uiText("settings.sound.unsupported")
	} else if audioErr != nil {
		audioHelp = uiText("settings.sound.list_error")
	} else if endpointErr != nil {
		audioHelp = uiText("settings.sound.no_endpoint_ids")
	} else if audioLive {
		audioHelp = uiText("settings.sound.live")
	}
	y = note(audioHelp, left, y, 450, 42, 50)
	_, grow = buttons(left, y, 28, 0, rowButton{uiText("settings.sound.windows_devices"), 260, settingsSoundID})
	y += 36 + grow
	y = note(uiText("settings.sound.defaults_help"), left, y, 450, 42, 50)
	_, grow = buttons(left, y, 28, 14,
		rowButton{uiText("settings.sound.camera_privacy"), 210, settingsPrivacyID},
		rowButton{uiText("settings.sound.microphone_privacy"), 224, settingsMicrophonePrivacyID})
	y += 40 + grow
	section(uiText("settings.section.usb"))
	_, grow = buttons(left, y, 28, 0, rowButton{uiText("settings.usb.choose"), 260, settingsUSBSelectionID})
	y += 36 + grow
	y = note(uiText("settings.usb.help"), left, y, clientW-2*left, 42, 50)
	pages[1] = append(pages[1], scroll.controls...)
	pageHeights[1] = y
	scroll.controls = nil
	y = scroll.top
	section(uiText("settings.section.graphics"))
	grow = fieldLabel(uiText("settings.graphics.displays"), y, 20)
	hDisplays = mk("EDIT", strconv.Itoa(guestDisplayCount(current.Displays)), fieldX, y, 100, 24, wsBorder|wsTabstop|esAutohscroll, settingsDisplaysID)
	grow = max(grow, hint(uiTextWith("settings.graphics.displays_hint", map[string]string{"max": strconv.Itoa(maximumGuestDisplays)}), y))
	y += 34 + grow
	grow = fieldLabel(uiText("settings.graphics.rendering"), y, 20)
	// Radio buttons side by side, each as wide as its label.
	renderX := fieldX
	for _, radio := range []struct {
		text  string
		width int32
		style uintptr
		id    uintptr
		out   *uintptr
	}{
		{uiText("settings.graphics.automatic"), 90, bsAutoradiobutton | wsGroup | wsTabstop, settingsRenderAutoID, &hRenderAuto},
		{uiText("settings.graphics.gpu"), 60, bsAutoradiobutton, settingsRenderGPUID, &hRenderGPU},
		{uiText("settings.graphics.cpu"), 60, bsAutoradiobutton, settingsRenderCPUID, &hRenderCPU},
	} {
		width, _ := measure(radio.text, 0)
		width = max(radio.width, width+checkboxTextInset+4)
		*radio.out = mk("BUTTON", radio.text, renderX, y, width, 22, radio.style, radio.id)
		renderX += width + 6
	}
	switch current.Render {
	case renderGPU:
		procSendMessageW.Call(hRenderGPU, bmSetcheck, bstChecked, 0)
	case renderCPU:
		procSendMessageW.Call(hRenderCPU, bmSetcheck, bstChecked, 0)
	default:
		procSendMessageW.Call(hRenderAuto, bmSetcheck, bstChecked, 0)
	}
	updateResourceControls()
	y += 24 + grow
	y = note(uiText("settings.graphics.rendering_help"), left, y, clientW-2*left, 36, 44)
	graphics := uiText("settings.graphics.gpu_note")
	if probe, err := loadRenderProbe(dataDir); err == nil && probe != nil {
		graphics += "\n" + uiTextWith("settings.graphics.last_boot", map[string]string{
			"mode": strings.ToUpper(probe.Result), "time": probe.RecordedAt.Local().Format("2006-01-02 15:04")})
	}
	y = note(graphics, left, y, clientW-2*left, 76, 84)
	section(uiText("settings.section.network"))
	// The three-line label sits beside the taller field.
	forwardsLabelBottom := y + 3 + 60 + fieldLabel(uiText("settings.network.forwards"), y, 60)
	hFwd = mk("EDIT", strings.Join(current.Forwards, "\r\n"), fieldX, y, fieldW, 72,
		wsBorder|wsTabstop|wsVscroll|esMultiline|esAutovscroll, settingsFwdID)
	y = max(y+82, forwardsLabelBottom+8)
	lanText := uiText("settings.network.add_lan")
	lanW, _ := measure(lanText, 0)
	lanW = max(120, lanW+24)
	mk("BUTTON", lanText, left, y, lanW, 26, wsTabstop, settingsLANAddID)
	hLANPublic, grow = check(uiText("settings.network.lan_public"), left+lanW+10, y, 22, settingsLANPublicID)
	if current.LANPublic {
		procSendMessageW.Call(hLANPublic, bmSetcheck, bstChecked, 0)
	}
	y += 32 + grow
	grow = fieldLabel(uiText("settings.network.ssh_key"), y, 24)
	hKey = mk("EDIT", current.SSHKey, fieldX, y, fieldW, 24, wsBorder|wsTabstop|esAutohscroll, settingsKeyID)
	y = note(uiText("settings.network.ssh_key_help"), left, y+28+grow, clientW-2*left, 22, 30)
	hUpdateOn, grow = check(uiText("settings.network.automatic_updates"), left, y, 24, settingsUpdateOnID)
	if !prefs.AutomaticUpdatesDisabled {
		procSendMessageW.Call(hUpdateOn, bmSetcheck, bstChecked, 0)
	}
	y += 34 + grow
	y = note(uiText("settings.network.linux_updates"), left, y, 450, 36, 42)
	_, grow = buttons(left, y, 28, 0, rowButton{uiText("settings.network.about"), 210, settingsAboutID})
	y += 40 + grow
	pages[2] = append(pages[2], scroll.controls...)
	pageHeights[2] = y
	scroll.controls = nil
	y = scroll.top
	section(uiText("settings.section.recovery"))
	recoveryRow := []rowButton{
		{uiText("settings.recovery.backup"), 0, settingsBackupID},
		{uiText("settings.recovery.restore"), 0, settingsRestoreID},
		{uiText("settings.recovery.snapshots"), 0, settingsSnapshotsID},
		{uiText("settings.recovery.reset"), 0, settingsResetID},
	}
	installRow := []rowButton{
		{uiText("settings.recovery.uninstall"), 0, settingsUninstallID},
		{uiText("settings.recovery.move"), 0, settingsMoveID},
		{uiText("settings.recovery.cleanup"), 0, settingsMoveCleanupID},
		{uiText("settings.recovery.portable"), 0, settingsPortableID},
	}
	// Both rows share one width so their buttons line up; English fits four
	// across.
	recoveryW := int32(104)
	for _, cell := range append(append([]rowButton{}, recoveryRow...), installRow...) {
		width, _ := measure(cell.text, 0)
		recoveryW = max(recoveryW, width+16)
	}
	recoveryButtons, grow := grid(left, y, 26, 8, recoveryW, recoveryRow...)
	if portable {
		procEnableWindow.Call(recoveryButtons[3], 0)
	}
	y += 30 + grow
	help := uiText("settings.recovery.help")
	if portable {
		help = uiText("settings.recovery.help_portable")
	}
	y = note(help, left, y, clientW-2*left, 36, 42)
	installButtons, grow := grid(left, y, 26, 8, recoveryW, installRow...)
	uninstallButton, moveButton, cleanupButton := installButtons[0], installButtons[1], installButtons[2]
	state, stateErr := hostMoveStore().load()
	if portable {
		procEnableWindow.Call(uninstallButton, 0)
		procEnableWindow.Call(moveButton, 0)
	}
	if portable || stateErr != nil || state.Retained == nil || !state.Retained.Booted || !pathsEqual(state.Retained.Destination, dataDir) {
		procEnableWindow.Call(cleanupButton, 0)
	}
	y += 40 + grow
	y = note(uiText("settings.recovery.install_help"), left, y, clientW-2*left, 20, 26)
	_, grow = buttons(left, y, 26, 0, rowButton{uiText("settings.recovery.install"), 160, settingsInstallOmarchyID})
	pages[3] = append(pages[3], scroll.controls...)
	pageHeights[3] = y + 40 + grow
	scroll.controls = nil
	y = scroll.top
	section(uiText("settings.section.apps"))
	y = note(uiText("settings.apps.help"), left, y, clientW-2*left, 48, 52)
	hApprovedApps = mk("LISTBOX", "", left, y, clientW-2*left, 240, wsBorder|wsVscroll|wsTabstop|0x0001, settingsAppListID) // LBS_NOTIFY
	refreshApprovedApps = func() {
		procSendMessageW.Call(hApprovedApps, 0x184, 0, 0) // LB_RESETCONTENT
		for _, app := range approvedApps.Apps {
			label, _ := syscall.UTF16PtrFromString(app.Name + " - " + app.Path)
			procSendMessageW.Call(hApprovedApps, 0x180, 0, uintptr(unsafe.Pointer(label))) // LB_ADDSTRING
		}
		if refreshFileOpeners != nil {
			refreshFileOpeners()
			refreshFiles()
		}
	}
	refreshApprovedApps()
	y += 250
	_, grow = buttons(left, y, 28, 10,
		rowButton{uiText("settings.apps.add"), 190, settingsAppAddID},
		rowButton{uiText("settings.apps.remove"), 160, settingsAppRemoveID})
	y += 40 + grow
	y = note(uiText("settings.apps.save_help"), left, y, clientW-2*left, 70, 76)
	section(uiText("settings.section.files"))
	y = note(uiText("settings.files.help"), left, y, clientW-2*left, 70, 76)
	hFiles = mk("LISTBOX", "", left, y, clientW-2*left, 160, wsBorder|wsVscroll|wsTabstop|0x0001, settingsFileListID) // LBS_NOTIFY
	y += 170
	grow = fieldLabel(uiText("settings.files.type"), y, 20)
	hFileType = mk("COMBOBOX", "", fieldX, y, fieldW, 220, 0x0003|wsVscroll|wsTabstop, settingsFileTypeID)
	y += 34 + grow
	grow = fieldLabel(uiText("settings.files.opener"), y, 20)
	hFileOpener = mk("COMBOBOX", "", fieldX, y, fieldW, 220, 0x0003|wsVscroll|wsTabstop, settingsFileOpenerID)
	y += 34 + grow
	refreshFileOpeners = func() {
		// A removed selection must not silently become the default opener.
		procSendMessageW.Call(hFileOpener, 0x14B, 0, 0) // CB_RESETCONTENT
		label, _ := syscall.UTF16PtrFromString(uiText("settings.files.default_app"))
		procSendMessageW.Call(hFileOpener, 0x143, 0, uintptr(unsafe.Pointer(label))) // CB_ADDSTRING
		for _, app := range approvedApps.Apps {
			label, _ := syscall.UTF16PtrFromString(app.Name)
			procSendMessageW.Call(hFileOpener, 0x143, 0, uintptr(unsafe.Pointer(label)))
		}
	}
	refreshFiles = func() {
		procSendMessageW.Call(hFiles, 0x184, 0, 0) // LB_RESETCONTENT
		for _, entry := range approvedFiles.Types {
			opener := uiText("settings.files.default_app")
			if entry.App != "" {
				opener = uiText("settings.files.unavailable_app")
				for _, app := range approvedApps.Apps {
					if app.ID == entry.App {
						opener = app.Name
						break
					}
				}
			}
			label, _ := syscall.UTF16PtrFromString(entry.Ext + " - " + opener)
			procSendMessageW.Call(hFiles, 0x180, 0, uintptr(unsafe.Pointer(label))) // LB_ADDSTRING
		}
		procSendMessageW.Call(hFileType, 0x14B, 0, 0) // CB_RESETCONTENT
		availableFileTypes = nil
		for _, ext := range ReviewedFileTypes() {
			if slices.ContainsFunc(approvedFiles.Types, func(entry approvedFileType) bool { return entry.Ext == ext }) {
				continue
			}
			availableFileTypes = append(availableFileTypes, ext)
			label, _ := syscall.UTF16PtrFromString(ext)
			procSendMessageW.Call(hFileType, 0x143, 0, uintptr(unsafe.Pointer(label))) // CB_ADDSTRING
		}
		if len(availableFileTypes) > 0 {
			procSendMessageW.Call(hFileType, 0x14E, 0, 0) // CB_SETCURSEL
		}
	}
	refreshFileOpeners()
	procSendMessageW.Call(hFileOpener, 0x14E, 0, 0) // CB_SETCURSEL, default on initial load only
	refreshFiles()
	_, grow = buttons(left, y, 28, 10,
		rowButton{uiText("settings.files.add"), 150, settingsFileAddID},
		rowButton{uiText("settings.files.remove"), 160, settingsFileRemoveID})
	y += 40 + grow
	y = note(uiText("settings.files.save_help"), left, y, clientW-2*left, 48, 54)
	section(uiText("settings.section.links"))
	if portable {
		y = note(uiText("settings.links.portable"), left, y, clientW-2*left, 20, 26)
	} else {
		y = note(uiText("settings.links.help"), left, y, clientW-2*left, 48, 52)
	}
	hLinks = mk("LISTBOX", "", left, y, clientW-2*left, 160, wsBorder|wsVscroll|wsTabstop|0x0001, settingsLinkListID) // LBS_NOTIFY
	refreshLinks = func() {
		procSendMessageW.Call(hLinks, 0x184, 0, 0) // LB_RESETCONTENT
		for _, scheme := range linkRows {
			label := scheme
			if slices.Contains(approvedLinks.Schemes, scheme) {
				label = uiTextWith("settings.links.allowed", map[string]string{"scheme": scheme})
				if linkConflicted[scheme] {
					label = uiTextWith("settings.links.conflicted", map[string]string{"scheme": scheme})
				}
			}
			value, _ := syscall.UTF16PtrFromString(label)
			procSendMessageW.Call(hLinks, 0x180, 0, uintptr(unsafe.Pointer(value))) // LB_ADDSTRING
		}
	}
	refreshLinks()
	y += 170
	linkButtons, grow := buttons(left, y, 28, 10,
		rowButton{uiText("settings.links.allow"), 150, settingsLinkAllowID},
		rowButton{uiText("settings.links.revoke"), 150, settingsLinkRevokeID})
	if portable {
		for _, h := range append(linkButtons, hLinks) {
			procEnableWindow.Call(h, 0)
		}
	}
	y += 40 + grow
	pages[4] = append(pages[4], scroll.controls...)
	pageHeights[4] = y
	scroll.controls = nil
	bodyControls = false
	footerText, saveText, cancelText := uiText("settings.save_hint"), uiText("settings.save"), uiText("settings.cancel")
	if launcher {
		footerText, saveText, cancelText = uiText("launcher.launch_hint"), uiText("launcher.launch"), uiText("launcher.close")
	}
	mk("STATIC", footerText, left, 0, clientW-48, 40, ssNoprefix, 0)
	// The footer's buttons keep their English widths unless a translation
	// needs more; layout() places Save and Cancel from the right edge.
	footerWidth := func(text string, minimum int32) int32 {
		width, _ := measure(text, 0)
		return max(minimum, width+24)
	}
	helpW, saveW, cancelW := footerWidth(uiText("settings.help"), 100), footerWidth(saveText, 154), footerWidth(cancelText, 100)
	mk("BUTTON", uiText("settings.help"), left, 46, helpW, 36, wsTabstop, settingsHelpID)
	mk("BUTTON", saveText, clientW-24-cancelW-8-saveW, 46, saveW, 36, bsDefpushbutton|wsTabstop, settingsSaveID)
	mk("BUTTON", cancelText, clientW-24-cancelW, 46, cancelW, 36, wsTabstop, settingsCancelID)
	brand.primary, _, _ = user32.NewProc("GetDlgItem").Call(hwnd, settingsSaveID)
	footer = append([]settingsScrollControl{}, scroll.controls...)
	scroll.controls = nil
	layout = func() {
		var client [4]int32
		procGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&client)))
		width, height := client[2], client[3]
		scroll.height = max(int32(1), height-scroll.top-104)
		procSetWindowPos.Call(scroll.viewport, 0, 0, uintptr(scroll.top), uintptr(width), uintptr(scroll.height), 0x0004|0x0010)
		scroll.controls = append(scroll.controls[:0], pages[currentPage]...)
		scroll.content = pageHeights[currentPage] + 12
		manual := selectedProfile() == resourceManual
		if currentPage == 0 {
			for _, handle := range manualControls {
				show := uintptr(0)
				if manual {
					show = swShow
				}
				procShowWindow.Call(handle, show)
			}
			if !manual {
				scroll.content -= manualEnd - manualStart
			}
		}
		for i := range scroll.controls {
			c := &scroll.controls[i]
			if currentPage == 0 && !manual && c.y >= manualEnd {
				c.y -= manualEnd - manualStart
			}
		}
		for i, c := range common {
			x := int32(16) + int32(i)*(width-32)/5
			procSetWindowPos.Call(c.handle, 0, uintptr(x), uintptr(c.y), uintptr((width-32)/5-4), uintptr(c.h), 0x0004|0x0010)
		}
		for i, c := range footer {
			x, w := c.x, c.w
			if i == 0 {
				w = width - 32
			}
			if i == 2 {
				x = width - 24 - footer[3].w - 8 - c.w
			}
			if i == 3 {
				x = width - 24 - c.w
			}
			procSetWindowPos.Call(c.handle, 0, uintptr(x), uintptr(height-100+c.y), uintptr(w), uintptr(c.h), 0x0004|0x0010)
		}
		for i := range scroll.controls {
			c := &scroll.controls[i]
			if c.x+c.w >= clientW-48 {
				c.w = max(int32(20), width-48-c.x)
			}
		}
		scroll.move(scroll.offset)
	}
	selectPage = func(index int) {
		currentPage = index
		brand.activeTab = common[index].handle
		for _, page := range pages {
			for _, c := range page {
				procShowWindow.Call(c.handle, 0)
			}
		}
		scroll.controls = nil
		for _, c := range pages[index] {
			procShowWindow.Call(c.handle, swShow)
			scroll.controls = append(scroll.controls, c)
		}
		scroll.content = pageHeights[index] + 12
		scroll.offset = 0
		layout()
		procRedrawWindow.Call(hwnd, 0, 0, 0x185)
	}
	selectPage(0)
	// Settings is often opened from the tray while the maximized QEMU window
	// owns the foreground. Raise it once, then immediately return it to the
	// normal z-order so it is visible without staying above unrelated apps.
	procSetWindowPos.Call(hwnd, hwndTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procSetForegroundWindow.Call(hwnd)
	procSetWindowPos.Call(hwnd, hwndNotTopmost, 0, 0, 0, 0, swpNoSize|swpNoMove|swpShowWindow)
	procSetFocus.Call(hFull)

	var m msgStruct
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if r == 0 || int32(r) == -1 {
			break
		}
		// This is a registered window, not a dialog resource. DefWindowProc
		// does not supply dialog default-button handling: Enter otherwise sends
		// IDOK instead of our Save/Launch ID, or ignores the focused button.
		if m.message == wmKeydown && m.wParam == 13 && m.hwnd != hFwd {
			var class [32]uint16
			procGetClassNameW.Call(m.hwnd, uintptr(unsafe.Pointer(&class[0])), uintptr(len(class)))
			switch strings.ToLower(syscall.UTF16ToString(class[:])) {
			case "button":
				style, _, _ := user32.NewProc("GetWindowLongW").Call(m.hwnd, ^uintptr(15)) // GWL_STYLE
				if style&0xf <= bsDefpushbutton || style&0xf == 0xb {
					procSendMessageW.Call(m.hwnd, 0x00f5, 0, 0) // BM_CLICK
				} else {
					procSendMessageW.Call(hwnd, wmCommand, settingsSaveID, 0)
				}
				continue
			case "edit":
				procSendMessageW.Call(hwnd, wmCommand, settingsSaveID, 0)
				continue
			}
		}
		// The multiline port-forward editor consumes Tab by default. Port
		// entries use newlines, so keep Tab and Shift+Tab for form navigation.
		if m.hwnd == hFwd && m.message == wmKeydown && m.wParam == 9 {
			shift, _, _ := procGetKeyState.Call(0x10)
			next, _, _ := procGetNextDlgTabItem.Call(hwnd, hFwd, (shift>>15)&1)
			if next != 0 {
				procSetFocus.Call(next)
			}
			scroll.revealFocus()
			continue
		}
		if ok, _, _ := procIsDialogMessageW.Call(hwnd, uintptr(unsafe.Pointer(&m))); ok != 0 {
			// IsDialogMessage also dispatches scrolling and paint messages.
			// Only keyboard navigation should bring the focused control back.
			if m.message == wmKeydown || m.message == wmSyskeydown || m.message == 0x106 {
				scroll.revealFocus()
			}
			continue
		}
		procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
	return saved
}

func settingsEstimateGPU(saved string, cpuControl, checked uintptr) bool {
	if cpuControl == 0 {
		return saved != renderCPU
	}
	return checked != bstChecked
}
