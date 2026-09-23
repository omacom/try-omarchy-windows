//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

func validateApprovedExecutable(path string) error {
	if !filepath.IsAbs(path) || strings.HasPrefix(path, `\\`) || strings.HasPrefix(path, `//`) || filepath.Clean(path) != path || !strings.EqualFold(filepath.Ext(path), ".exe") {
		return fmt.Errorf("choose a local Windows .exe file")
	}
	info, err := os.Lstat(path)
	if err != nil {
		return fmt.Errorf("Windows app is unavailable: %w", err)
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("Windows app must be an ordinary .exe file")
	}
	return nil
}

func approveWindowsExecutable(prefs *approvedAppPreferences, path string) error {
	if err := validateApprovedExecutable(path); err != nil {
		return err
	}
	if len(prefs.Apps) >= maximumApprovedApps {
		return fmt.Errorf("you can approve up to %d Windows apps", maximumApprovedApps)
	}
	for _, app := range prefs.Apps {
		if strings.EqualFold(app.Path, path) {
			return fmt.Errorf("this Windows app is already approved")
		}
	}
	id, err := newApprovedAppID()
	if err != nil {
		return err
	}
	name := strings.TrimSuffix(filepath.Base(path), filepath.Ext(path))
	name = strings.TrimSpace(name)
	if name == "" || len([]rune(name)) > 80 {
		return fmt.Errorf("Windows app filename needs a name of up to 80 characters")
	}
	updated := *prefs
	updated.Apps = append(append([]approvedWindowsApp{}, prefs.Apps...), approvedWindowsApp{ID: id, Name: name, Path: path})
	if err := updated.validate(); err != nil {
		return err
	}
	*prefs = updated
	return nil
}

func launchApprovedWindowsApp(dir, id string) error {
	if !validApprovedAppID(id) {
		return fmt.Errorf("invalid Windows app ID")
	}
	prefs, err := loadApprovedWindowsApps(dir)
	if err != nil {
		return err
	}
	for _, app := range prefs.Apps {
		if app.ID != id {
			continue
		}
		if err := validateApprovedExecutable(app.Path); err != nil {
			return err
		}
		cmd := exec.Command(app.Path)
		cmd.Dir = filepath.Dir(app.Path)
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: createNoWindow}
		if err := cmd.Start(); err != nil {
			return fmt.Errorf("start approved Windows app: %w", err)
		}
		if hwnd := qemuHwnd.Load(); hwnd != 0 {
			procShowWindow.Call(hwnd, swShowMinimized)
		}
		_ = cmd.Process.Release()
		return nil
	}
	return fmt.Errorf("Windows app is not approved")
}
