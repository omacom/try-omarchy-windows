//go:build windows

package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

// hostFacts collects what a bug report usually has to ask for twice: the
// Windows build, CPU, memory, and whether the machine is ARM64 in disguise.
func hostFacts() map[string]string {
	facts := map[string]string{
		"host.os":       os.Getenv("OS"),
		"host.cpu":      os.Getenv("PROCESSOR_IDENTIFIER"),
		"host.cpuArch":  os.Getenv("PROCESSOR_ARCHITECTURE"),
		"host.cpuCount": os.Getenv("NUMBER_OF_PROCESSORS"),
		"host.emulated": fmt.Sprint(os.Getenv("PROCESSOR_ARCHITEW6432") != ""),
	}
	var v struct {
		size, major, minor, build, platform uint32
		csd                                 [128]uint16
	}
	v.size = uint32(unsafe.Sizeof(v))
	if r, _, _ := syscall.NewLazyDLL("ntdll.dll").NewProc("RtlGetVersion").Call(uintptr(unsafe.Pointer(&v))); r == 0 {
		facts["host.windows"] = fmt.Sprintf("%d.%d.%d", v.major, v.minor, v.build)
	}
	total, avail := availMemMiB()
	facts["host.memoryTotalMiB"] = fmt.Sprint(total)
	facts["host.memoryAvailableMiB"] = fmt.Sprint(avail)
	return facts
}

const mbIconInformation = 0x40

func infoBox(text string) {
	t, _ := syscall.UTF16PtrFromString(text)
	c, _ := syscall.UTF16PtrFromString(appTitle)
	procMessageBoxW.Call(0, uintptr(unsafe.Pointer(t)), uintptr(unsafe.Pointer(c)), mbIconInformation|mbFront)
}
