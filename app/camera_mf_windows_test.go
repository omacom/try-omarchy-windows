//go:build windows

package main

import (
	"syscall"
	"testing"
)

// MFEnumDeviceSources is an mf.dll export, not the mfplat.dll one the capture
// code first assumed, and Windows builds have moved these entry points between
// mfplat.dll, mf.dll and mfcore.dll. Every one must resolve on a Windows with
// Media Foundation, and a missing export must surface as an error instead of
// the panic LazyProc.Call raises.
func TestMediaFoundationExportsResolve(t *testing.T) {
	api, err := mediaFoundation()
	if err != nil {
		t.Fatalf("resolving Media Foundation entry points: %v", err)
	}
	for name, proc := range map[string]*syscall.LazyProc{
		"MFStartup":                           api.startup,
		"MFCreateAttributes":                  api.createAttributes,
		"MFCreateMediaType":                   api.createMediaType,
		"MFEnumDeviceSources":                 api.enumDeviceSources,
		"MFCreateSourceReaderFromMediaSource": api.createSourceReader,
	} {
		if proc == nil {
			t.Fatalf("%s was not resolved", name)
		}
	}
}

func TestFindMFProcReportsMissingExports(t *testing.T) {
	if _, err := findMFProc("MFThisExportDoesNotExist"); err == nil {
		t.Fatal("a missing Media Foundation export resolved without an error")
	}
}
