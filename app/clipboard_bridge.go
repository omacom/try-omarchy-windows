package main

import (
	"bufio"
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
//   push port (4448): guest -> host, one line per change, then close
//   pull port (4449): host -> guest, one persistent connection, one line per change
// A line is base64(UTF-8 text), or "png:" + base64(PNG) for an image.
// Loop prevention: each side skips content it just received. Lines are LF-only;
// CRLF corrupts the guest's base64 -d.

type clipBridge struct {
	mu       sync.Mutex
	state    clipboardSyncState
	pullConn net.Conn
	getHost  func() (clipItem, bool)
	setHost  func(clipItem) bool
	// sequence reports the Windows clipboard sequence number when available,
	// so an unchanged clipboard (which may hold a large image) is not read
	// and converted on every poll.
	sequence     func() uint32
	lastSequence uint32
}

func (b *clipBridge) acceptPush(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		go func(c net.Conn) {
			defer c.Close()
			// A compromised or broken guest must not make the Windows launcher
			// allocate an unbounded line. Base64 expands data by at most 4/3.
			c.SetReadDeadline(time.Now().Add(10 * time.Second))
			line, err := bufio.NewReader(io.LimitReader(c, int64(maxClipFrameBytes))).ReadString('\n')
			if err != nil || !strings.HasSuffix(line, "\n") {
				return
			}
			item, ok := decodeClipFrame(line)
			if !ok {
				return
			}
			b.acceptGuestItem(item)
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
		b.state = clipboardSyncState{}
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
		if conn == nil {
			continue
		}
		if b.sequence != nil {
			if seq := b.sequence(); seq == b.lastSequence {
				continue
			}
		}
		b.sendCurrentHost(conn)
	}
}

func (b *clipBridge) sendCurrentHost(conn net.Conn) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pullConn != conn {
		return
	}
	if b.sequence != nil {
		b.lastSequence = b.sequence()
	}
	cur, ok := b.getHost()
	if !ok || !b.state.shouldSendHost(cur) {
		return
	}
	line := encodeClipFrame(cur)

	conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	if n, err := conn.Write([]byte(line)); err != nil || n != len(line) {
		if b.pullConn == conn {
			conn.Close()
			b.pullConn = nil
		}
		return
	}
	b.state.markHostSent(cur)
}

// Serialize clipboard access with state changes so polling cannot echo a guest
// write before it has been recorded, or deliver a stale host read afterward.
func (b *clipBridge) acceptGuestItem(item clipItem) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if !b.state.shouldAcceptGuest(item) {
		return
	}
	if b.setHost(item) {
		b.state.markGuestAccepted(item)
		if b.sequence != nil {
			// Our own write bumps the sequence; do not echo it back.
			b.lastSequence = b.sequence()
		}
	} else {
		logf("clipboard: could not write the Windows clipboard")
	}
}
