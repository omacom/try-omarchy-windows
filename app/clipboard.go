//go:build windows

package main

import (
	"bufio"
	"encoding/base64"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"
)

// Host side of the two-way text clipboard bridge, ported from
// scripts/clipboard-bridge.ps1. The guest daemon (scripts/guest/
// clipboard-bridge.sh, baked into the image) reaches these listeners as
// 10.0.2.2 over QEMU user-mode networking:
//   push port (4448): guest -> host, one base64(UTF-8) line per change, then close
//   pull port (4449): host -> guest, one persistent connection, one line per change
// Loop prevention: each side skips content it just received. Lines are LF-only;
// CRLF corrupts the guest's base64 -d.

type clipBridge struct {
	mu       sync.Mutex
	state    clipboardSyncState
	pullConn net.Conn
}

func runClipboardBridge() {
	b := &clipBridge{}
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

func (b *clipBridge) acceptPush(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		go func(c net.Conn) {
			defer c.Close()
			c.SetReadDeadline(time.Now().Add(3 * time.Second))
			// A compromised or broken guest must not make the Windows launcher
			// allocate an unbounded line. Base64 expands data by at most 4/3.
			encodedLimit := int64((maxClipboardTextBytes+2)/3*4 + 2)
			line, err := bufio.NewReader(io.LimitReader(c, encodedLimit)).ReadString('\n')
			if err != nil || !strings.HasSuffix(line, "\n") {
				return
			}
			line = strings.TrimRight(line, "\r\n")
			data, err := base64.StdEncoding.DecodeString(line)
			if err != nil || len(data) == 0 || len(data) > maxClipboardTextBytes {
				return
			}
			text := string(data)
			b.mu.Lock()
			fresh := b.state.shouldAcceptGuest(text)
			b.mu.Unlock()
			if fresh {
				if clipboardSetText(text) {
					b.mu.Lock()
					b.state.markGuestAccepted(text)
					b.mu.Unlock()
				} else {
					logf("clipboard: could not open the Windows clipboard")
				}
			}
		}(c)
	}
}

func (b *clipBridge) acceptPull(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		b.mu.Lock()
		if b.pullConn != nil {
			b.pullConn.Close()
		}
		b.pullConn = c
		b.mu.Unlock()
		logf("clipboard: guest connected")
		b.sendCurrentHost(c)
	}
}

func (b *clipBridge) pollHost() {
	for {
		time.Sleep(400 * time.Millisecond)
		b.mu.Lock()
		conn := b.pullConn
		b.mu.Unlock()
		if conn != nil {
			b.sendCurrentHost(conn)
		}
	}
}

func (b *clipBridge) sendCurrentHost(conn net.Conn) {
	cur, ok := clipboardGetText()
	if !ok || cur == "" {
		return
	}
	line := base64.StdEncoding.EncodeToString([]byte(cur)) + "\n"

	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pullConn != conn || !b.state.shouldSendHost(cur) {
		return
	}
	conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	if _, err := conn.Write([]byte(line)); err != nil {
		if b.pullConn == conn {
			conn.Close()
			b.pullConn = nil
		}
		return
	}
	b.state.markHostSent(cur)
}
