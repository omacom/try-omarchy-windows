//go:build windows

package main

import (
	"strings"
	"testing"
	"unsafe"
)

func TestDXGIAdapterDescriptionLayout(t *testing.T) {
	var desc dxgiAdapterDesc
	if unsafe.Sizeof(uintptr(0)) != 8 {
		t.Skip("launcher ships amd64")
	}
	if unsafe.Sizeof(desc) != 312 || unsafe.Offsetof(desc.VideoMemory) != 272 || unsafe.Offsetof(desc.LUIDLow) != 296 || unsafe.Offsetof(desc.Flags) != 304 {
		t.Fatalf("DXGI_ADAPTER_DESC1 ABI mismatch: size=%d memory=%d LUID=%d flags=%d", unsafe.Sizeof(desc), unsafe.Offsetof(desc.VideoMemory), unsafe.Offsetof(desc.LUIDLow), unsafe.Offsetof(desc.Flags))
	}
}

func TestDXGIEnumerationOnWindows(t *testing.T) {
	facts := dxgiAdapterFacts()
	t.Logf("DXGI: %+v", facts)
	if strings.HasPrefix(facts["gpu.adapters"], "unavailable") {
		t.Skip("DXGI unavailable on this Windows host")
	}
	if !strings.Contains(facts["gpu.adapters"], "vendor=0x") || !strings.Contains(facts["gpu.adapters"], "LUID=") {
		t.Fatalf("no adapter identities: %+v", facts)
	}
	for _, key := range []string{"gpu.order.highPerformance", "gpu.order.minimumPower"} {
		if !strings.HasPrefix(facts[key], "unavailable") && !strings.Contains(facts[key], "LUID=") {
			t.Fatalf("missing preference order: %+v", facts)
		}
	}
	if value := qemuGPUPreference(`C:\try-omarchy-test-no-such-runtime\qemu-system-x86_64w.exe`); value != "unset (Windows default)" {
		t.Fatalf("unexpected preference for nonexistent runtime: %s", value)
	}
}
