//go:build windows

package main

import "testing"

func TestDataLocationIsLocal(t *testing.T) {
	dir := t.TempDir()
	if !dataLocationIsLocal(dir) {
		t.Fatal("temporary directory was not recognized as local")
	}
	if supported, err := dataLocationSupportsSparseFiles(dir); err != nil || !supported {
		t.Fatalf("temporary directory sparse support = %v, %v", supported, err)
	}
	if err := validateStandardDataDrive(dir); err != nil {
		t.Fatalf("temporary directory was not accepted: %v", err)
	}
	if dataLocationIsLocal(`\\server\share\TryOmarchy`) {
		t.Fatal("UNC location was recognized as local")
	}
	for _, driveType := range []uintptr{0, 1, 4, 5} {
		if supportedLocalDriveType(driveType) {
			t.Fatalf("drive type %d was recognized as supported", driveType)
		}
	}
	for _, driveType := range []uintptr{driveRemovable, driveFixed, driveRAMDisk} {
		if !supportedLocalDriveType(driveType) {
			t.Fatalf("drive type %d was not recognized as supported", driveType)
		}
	}
}
