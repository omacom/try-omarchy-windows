//go:build !windows

package main

import "runtime"

func hostFacts() map[string]string {
	return map[string]string{"host.os": runtime.GOOS, "host.cpuArch": runtime.GOARCH}
}
