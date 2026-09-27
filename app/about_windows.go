//go:build windows

package main

import (
	"net/http"
	"os/exec"
	"syscall"
	"time"
)

const projectURL = "https://github.com/omacom/try-omarchy-windows"
const websiteURL = "https://tryomarchy.com"

func runAbout() {
	message := uiTextWith("about.body", map[string]string{
		"version": currentVersion, "website": websiteURL, "source": projectURL,
	})
	action, err := chooseAction(uiText("about.title"), message,
		uiText("about.check_updates"), uiText("about.open_website"), uiText("about.open_support"),
		uiText("about.third_party"), uiText("about.close"))
	if err != nil {
		errorBox(uiText("about.open_error") + err.Error())
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
	getUI().setStatus("%s", uiText("about.checking"))
	key, err := updatePublicKey()
	var manifest *updateManifest
	if err == nil {
		manifest, err = fetchUpdateManifest(&http.Client{Timeout: 10 * time.Second}, defaultUpdateURL, key)
	}
	uiDone()
	if err != nil {
		errorBox(uiText("about.check_error") + err.Error())
		return
	}
	if !updateIsNewer(manifest.Version, currentVersion) {
		infoBox(uiTextWith("about.no_update", map[string]string{"installed": currentVersion, "latest": manifest.Version}))
		return
	}
	if msgBox(uiTextWith("about.update_available", map[string]string{"installed": currentVersion, "latest": manifest.Version}), mbYesNo|mbIconQuestion) != idYes {
		return
	}
	openWindowsURL(projectURL + "/releases/tag/" + manifest.Version)
}

func openWindowsURL(url string) {
	cmd := exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", url)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		errorBox(uiText("about.open_page_error") + err.Error())
		return
	}
	_ = cmd.Process.Release()
}
