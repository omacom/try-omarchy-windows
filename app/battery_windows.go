//go:build windows

package main

import (
	"fmt"
	"syscall"
	"unsafe"
)

var procGetSystemPowerStatus = syscall.NewLazyDLL("kernel32.dll").NewProc("GetSystemPowerStatus")

func hostBatteryLine() (string, error) {
	var status systemPowerStatus
	ok, _, err := procGetSystemPowerStatus.Call(uintptr(unsafe.Pointer(&status)))
	if ok == 0 {
		return "", fmt.Errorf("GetSystemPowerStatus: %w", err)
	}
	return encodeBatteryLine(status)
}
