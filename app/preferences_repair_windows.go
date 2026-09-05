//go:build windows

package main

import "path/filepath"

func offerPreferencesRepair(path string, cause error) error {
	if msgBox("Try Omarchy cannot read "+filepath.Base(path)+".\n\n"+cause.Error()+"\n\nRestore these preferences to their defaults? The original file will be backed up first. Guest files and the VM disk will not be changed.", mbYesNo|mbIconQuestion|mbDefbutton2) != idYes {
		return errSetupCancelled
	}
	saved, err := repairPreferences(path)
	if err != nil {
		return err
	}
	infoBox("Default preferences restored.\n\nThe previous file is kept at:\n" + saved + "\n\nReview your settings before enabling a shared folder or port forwarding again.")
	return nil
}

func loadSettingsWithRepair(path string) (settings, error) {
	value, err := loadSettings(path)
	if err == nil {
		return value, nil
	}
	if err = offerPreferencesRepair(path, err); err != nil {
		return settings{}, err
	}
	return loadSettings(path)
}
func loadStorageWithRepair(dir string) (storageSettings, error) {
	value, err := loadStorageSettings(dir)
	if err == nil {
		return value, nil
	}
	if err = offerPreferencesRepair(filepath.Join(dir, storageSettingsFilename), err); err != nil {
		return storageSettings{}, err
	}
	return loadStorageSettings(dir)
}
