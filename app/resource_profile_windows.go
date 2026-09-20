//go:build windows

package main

import (
	"runtime"
	"time"
	"unsafe"
)

var procGetSystemTimes = kernel32.NewProc("GetSystemTimes")

func systemCPUTimes() (idle, kernel, user uint64, ok bool) {
	r, _, _ := procGetSystemTimes.Call(uintptr(unsafe.Pointer(&idle)), uintptr(unsafe.Pointer(&kernel)), uintptr(unsafe.Pointer(&user)))
	return idle, kernel, user, r != 0
}

func measureHostResources(sampleCPU bool) hostResources {
	h := hostResources{LogicalCPUs: runtime.NumCPU()}
	// GetSystemTimes is per processor group on hosts with more than 64 CPUs;
	// do not extrapolate one group's load to the whole machine.
	if sampleCPU && h.LogicalCPUs <= 64 {
		i0, k0, u0, ok := systemCPUTimes()
		if ok {
			time.Sleep(750 * time.Millisecond)
			i1, k1, u1, ok := systemCPUTimes()
			if ok {
				h.CPUBusy, h.CPUKnown = cpuBusyFraction(i0, k0, u0, i1, k1, u1)
			}
		}
	}
	h.TotalMiB, h.AvailableMiB = availMemMiB()
	return h
}
