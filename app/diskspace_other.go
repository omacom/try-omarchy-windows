//go:build !windows

package main

import (
	"errors"
	"syscall"
)

// Only the Windows build ships; this keeps setupFailureHelp testable on the
// Linux runners CI actually uses.
const diskFullErrno = syscall.ENOSPC

func isDiskFull(err error) bool {
	var errno syscall.Errno
	return errors.As(err, &errno) && errno == diskFullErrno
}
