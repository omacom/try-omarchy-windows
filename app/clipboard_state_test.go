package main

import "testing"

func TestClipboardHostTextRemainsPendingUntilSent(t *testing.T) {
	var state clipboardSyncState
	const text = "copied before the guest connected"

	if !state.shouldSendHost(textItem(text)) {
		t.Fatal("new host text was not eligible for delivery")
	}
	if !state.shouldSendHost(textItem(text)) {
		t.Fatal("unsent host text was consumed")
	}
	state.markHostSent(textItem(text))
	if state.shouldSendHost(textItem(text)) {
		t.Fatal("delivered host text was offered again")
	}
}

func TestClipboardGuestTextDoesNotEchoBack(t *testing.T) {
	var state clipboardSyncState
	if !state.shouldAcceptGuest(textItem("from guest")) {
		t.Fatal("new guest text was rejected")
	}
	state.markGuestAccepted(textItem("from guest"))
	if state.shouldAcceptGuest(textItem("from guest")) {
		t.Fatal("duplicate guest text was accepted")
	}
	if state.shouldSendHost(textItem("from guest")) {
		t.Fatal("guest text would echo back to the guest")
	}
}

func TestClipboardRejectsOversizedText(t *testing.T) {
	tooLarge := string(make([]byte, maxClipboardTextBytes+1))
	var state clipboardSyncState
	if state.shouldAcceptGuest(textItem(tooLarge)) || state.shouldSendHost(textItem(tooLarge)) {
		t.Fatal("oversized clipboard text was accepted")
	}
}
