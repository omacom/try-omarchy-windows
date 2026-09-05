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

func (s *clipboardSyncState) shouldAcceptGuest(item clipItem) bool {
	return item.allowed() && item.key() != s.lastSeen
}

func (s *clipboardSyncState) markGuestAccepted(item clipItem) {
	s.lastSeen = item.key()
}

func (s *clipboardSyncState) shouldSendHost(item clipItem) bool {
	return item.allowed() && item.key() != s.lastSeen
}

func (s *clipboardSyncState) markHostSent(item clipItem) {
	s.lastSeen = item.key()
}
