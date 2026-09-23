//go:build windows

package main

import (
	"fmt"
	"net/http"
	"os/exec"
	"syscall"
	"time"
)

const projectURL = "https://github.com/omacom/try-omarchy-windows"
const websiteURL = "https://tryomarchy.com"

func runAbout() {
	message := fmt.Sprintf("Try Omarchy %s\n\n"+
		"Run the Omarchy desktop on Windows. Your files persist between sessions.\n\n"+
		"Originally created by @martiano. Maintained under Omacom.\n\n"+
		"Website: "+websiteURL+"\n"+
		"Source, help and issue reporting: "+projectURL+"\n\n"+
		"Open source under the MIT License. Built with Omarchy, Arch Linux and QEMU.\n\n"+
		"Launcher updates and Linux updates are separate. For Linux packages and Omarchy, use Update > Omarchy inside the desktop.",
		currentVersion)
	action, err := chooseAction("About Try Omarchy", message, "Check for launcher updates", "Open Try Omarchy website", "Open source and support", "Third-party notices", "Close")
	if err != nil {
		errorBox("Could not open About.\n\n" + err.Error())
		return
	}
	switch action {
	case 1:
		checkForLauncherUpdates()
	case 2:
		openWindowsURL(websiteURL)
	case 3:
		openWindowsURL(projectURL)
	case 4:
		openWindowsURL(projectURL + "/blob/master/THIRD_PARTY_NOTICES.md")
	}
}

func checkForLauncherUpdates() {
	getUI().setStatus("Checking for updates...")
	key, err := updatePublicKey()
	var manifest *updateManifest
	if err == nil {
		manifest, err = fetchUpdateManifest(&http.Client{Timeout: 10 * time.Second}, defaultUpdateURL, key)
	}
	uiDone()
	if err != nil {
		errorBox("Could not check for updates. Your installation has not changed.\n\n" + err.Error())
		return
	}
	if !updateIsNewer(manifest.Version, currentVersion) {
		infoBox("No newer compatible launcher is available.\n\nInstalled: " + currentVersion + "\nLatest published release: " + manifest.Version)
		return
	}
	if msgBox("Update available: "+manifest.Version+"\nInstalled: "+currentVersion+"\n\nOpen the release notes and download page? Close Omarchy before opening the new launcher.", mbYesNo|mbIconQuestion) != idYes {
		return
	}
	openWindowsURL(projectURL + "/releases/tag/" + manifest.Version)
}

func openWindowsURL(url string) {
	cmd := exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", url)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		errorBox("Windows could not open the page.\n\n" + err.Error())
		return
	}
	_ = cmd.Process.Release()
}
