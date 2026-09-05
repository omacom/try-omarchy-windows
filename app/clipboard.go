//go:build windows

package main

import (
	"fmt"
	"net"
)

func runClipboardBridge() {
	b := &clipBridge{getText: clipboardGetText, setText: clipboardSetText}
	// These listeners double as the single-instance check: a second copy of
	// the app (or a leftover QEMU on our QMP ports) must fail loudly, not
	// die 30 seconds later with an inscrutable QEMU port error.
	push, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", clipPushPort))
	if err != nil {
		fatal("Try Omarchy looks like it's already running (port %d is in use).", clipPushPort)
	}
	pull, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", clipPullPort))
	if err != nil {
		fatal("Try Omarchy looks like it's already running (port %d is in use).", clipPullPort)
	}
	logf("clipboard: guest->host on %d, host->guest on %d", clipPushPort, clipPullPort)

	go b.acceptPush(push)
	go b.acceptPull(pull)
	go b.pollHost()
}
