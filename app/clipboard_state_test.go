package main

import "testing"

func TestClipboardHostTextRemainsPendingUntilSent(t *testing.T) {
	var state clipboardSyncState
	const text = "copied before the guest connected"

	if !state.shouldSendHost(text) {
		t.Fatal("new host text was not eligible for delivery")
	}
	if !state.shouldSendHost(text) {
		t.Fatal("unsent host text was consumed")
	}
	state.markHostSent(text)
	if state.shouldSendHost(text) {
		t.Fatal("delivered host text was offered again")
	}
}

func TestClipboardGuestTextDoesNotEchoBack(t *testing.T) {
	var state clipboardSyncState
	if !state.shouldAcceptGuest("from guest") {
		t.Fatal("new guest text was rejected")
	}
	state.markGuestAccepted("from guest")
	if state.shouldAcceptGuest("from guest") {
		t.Fatal("duplicate guest text was accepted")
	}
	if state.shouldSendHost("from guest") {
		t.Fatal("guest text would echo back to the guest")
	}
}

func TestClipboardRejectsOversizedText(t *testing.T) {
	tooLarge := string(make([]byte, maxClipboardTextBytes+1))
	var state clipboardSyncState
	if state.shouldAcceptGuest(tooLarge) || state.shouldSendHost(tooLarge) {
		t.Fatal("oversized clipboard text was accepted")
	}
}
