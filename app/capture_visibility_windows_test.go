//go:build windows

package main

import (
	"testing"
	"unsafe"
)

func TestSessionDisplayStatusReadsOnlyItsSetting(t *testing.T) {
	type message struct {
		head powerBroadcastSetting
		data uint32
	}
	if unsafe.Offsetof(message{}.data) != 20 {
		t.Fatalf("POWERBROADCAST_SETTING data offset = %d, want 20", unsafe.Offsetof(message{}.data))
	}
	off := message{head: powerBroadcastSetting{powerSetting: guidSessionDisplayStatus, dataLength: 4}, data: sessionDisplayOff}
	if state, ok := sessionDisplayStatus(&off.head); !ok || state != sessionDisplayOff {
		t.Fatalf("display off read as %d, %t", state, ok)
	}
	on := message{head: powerBroadcastSetting{powerSetting: guidSessionDisplayStatus, dataLength: 4}, data: sessionDisplayOn}
	if state, ok := sessionDisplayStatus(&on.head); !ok || state != sessionDisplayOn {
		t.Fatalf("display on read as %d, %t", state, ok)
	}
	other := message{head: powerBroadcastSetting{powerSetting: comGUID{d1: 1}, dataLength: 4}}
	if _, ok := sessionDisplayStatus(&other.head); ok {
		t.Fatal("another power setting was read as the display state")
	}
	short := message{head: powerBroadcastSetting{powerSetting: guidSessionDisplayStatus}}
	if _, ok := sessionDisplayStatus(&short.head); ok {
		t.Fatal("a setting without data was read")
	}
	if _, ok := sessionDisplayStatus(nil); ok {
		t.Fatal("a missing setting was read")
	}
}
