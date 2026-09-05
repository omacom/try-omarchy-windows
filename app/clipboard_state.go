package main

import (
	"strings"
	"unicode/utf8"
)

const maxClipboardTextBytes = 8 << 20

func clipboardTextAllowed(text string) bool {
	return len(text) > 0 && len(text) <= maxClipboardTextBytes && utf8.ValidString(text) && !strings.ContainsRune(text, 0)
}

// clipboardSyncState tracks content that already crossed the bridge. Host
// content is marked only after a successful write so a disconnected or
// reconnecting guest cannot make a clipboard change disappear.
type clipboardSyncState struct {
	lastSeen string
}

func (s *clipboardSyncState) shouldAcceptGuest(text string) bool {
	return clipboardTextAllowed(text) && text != s.lastSeen
}

func (s *clipboardSyncState) markGuestAccepted(text string) {
	s.lastSeen = text
}

func (s *clipboardSyncState) shouldSendHost(text string) bool {
	return clipboardTextAllowed(text) && text != s.lastSeen
}

func (s *clipboardSyncState) markHostSent(text string) {
	s.lastSeen = text
}
