//go:build windows

package main

import "testing"

func TestDataLocationIsLocal(t *testing.T) {
	if !dataLocationIsLocal(t.TempDir()) {
		t.Fatal("temporary directory was not recognized as local")
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
