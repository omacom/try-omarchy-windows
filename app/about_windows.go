//go:build windows

package main

import (
	"fmt"
	"net/http"
	"os/exec"
	"syscall"
	"time"
)

func runAbout() {
	message := fmt.Sprintf("Try Omarchy %s\n\nRun Omarchy on Windows. Your files persist between sessions.\n\nLauncher updates and Linux updates are separate. For Linux packages and Omarchy, use Update > Omarchy inside the desktop.\n\nCheck for launcher updates now?", currentVersion)
	if msgBox(message, mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return
	}
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
	openWindowsURL("https://github.com/omacom/try-omarchy-windows/releases/tag/" + manifest.Version)
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
