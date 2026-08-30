//go:build !windows

package main

import (
	"errors"
	"syscall"
)

// Classifies an error that already happened, unlike the up-front free-space
// check. Only the Windows build ships; matching ENOSPC here keeps
// setupFailureHelp testable on the Linux runners CI actually uses.
const diskFullErrno = syscall.ENOSPC

func isDiskFull(err error) bool {
	var errno syscall.Errno
	return errors.As(err, &errno) && errno == diskFullErrno
}
