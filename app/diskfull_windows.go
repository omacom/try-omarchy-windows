//go:build windows

package main

import (
	"errors"
	"syscall"
)

// Classifies an error that already happened. Free space is checked up front in
// disk_space*.go; this is the backstop for a volume that fills mid-run.
//
// A full volume surfaces as ERROR_DISK_FULL, or as ERROR_HANDLE_DISK_FULL when
// the handle's volume filled mid-write. Go maps neither onto ENOSPC, so the
// codes have to be matched directly.
const (
	diskFullErrno       = syscall.Errno(112) // ERROR_DISK_FULL
	handleDiskFullErrno = syscall.Errno(39)  // ERROR_HANDLE_DISK_FULL
)

func isDiskFull(err error) bool {
	var errno syscall.Errno
	return errors.As(err, &errno) && (errno == diskFullErrno || errno == handleDiskFullErrno)
}
