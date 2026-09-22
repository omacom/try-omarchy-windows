//go:build windows

package main

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
)

func platformQMPControlDirectory() (string, error) {
	// QEMU's Windows AF_UNIX listener can be created directly below
	// %LOCALAPPDATA%, yet every connect to it fails with WSAEINVAL on affected
	// hosts. The per-user Windows temporary directory does not have that
	// limitation and keeps the control sockets outside guest-accessible TCP.
	base := os.TempDir()
	if len([]byte(filepath.Join(base, "TryOmarchyIPC", "supervisor.sock"))) > 103 {
		name, err := syscall.UTF16PtrFromString(base)
		if err != nil {
			return "", err
		}
		buffer := make([]uint16, 32768)
		if n, err := syscall.GetShortPathName(name, &buffer[0], uint32(len(buffer))); err == nil && n > 0 && n < uint32(len(buffer)) {
			base = syscall.UTF16ToString(buffer[:n])
		}
	}
	return filepath.Join(base, "TryOmarchyIPC"), nil
}

func isQMPControlSocket(path string) bool {
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return false
	}
	var data syscall.Win32finddata
	handle, err := syscall.FindFirstFile(name, &data)
	if err != nil {
		return false
	}
	syscall.FindClose(handle)
	return data.FileAttributes&fileAttributeReparsePoint != 0 && data.Reserved0 == 0x80000023
}

func qmpConnectionRefused(err error) bool { return errors.Is(err, syscall.Errno(10061)) }
