package main

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"strings"
	"sync"
	"time"
)

// Host side of the guest agent channel. The guest's try-omarchy-agent service
// connects out to this loopback listener (10.0.2.2 from inside QEMU user
// networking), the same way the clipboard bridge does, so no extra QEMU device
// or early chardev connection is involved. One line per message:
//   host -> guest: "time <unix seconds>"   set the guest clock when it drifts
//   guest -> host: "hello <version>"       the agent connected
// The host sends the time on connect, every few minutes, and after Windows
// resumes from sleep, when the guest clock is the thing most likely to be wrong.

const agentTimeInterval = 5 * time.Minute

type guestAgent struct {
	mu   sync.Mutex
	conn net.Conn
	now  func() time.Time
}

func newGuestAgent() *guestAgent {
	return &guestAgent{now: time.Now}
}

func (a *guestAgent) accept(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		a.mu.Lock()
		if a.conn != nil {
			a.conn.Close()
		}
		a.conn = c
		a.mu.Unlock()
		go a.read(c)
		a.sendTime("connect")
	}
}

func (a *guestAgent) read(c net.Conn) {
	r := bufio.NewReader(io.LimitReader(c, 64<<10))
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			break
		}
		if strings.HasPrefix(line, "hello") {
			logf("agent: guest agent connected (%s)", strings.TrimSpace(strings.TrimPrefix(line, "hello")))
		}
	}
	a.mu.Lock()
	if a.conn == c {
		a.conn = nil
	}
	a.mu.Unlock()
	c.Close()
}

// sendTime tells the guest the host's clock. It is safe to call from any
// goroutine and does nothing without a connected agent.
func (a *guestAgent) sendTime(reason string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn == nil {
		return false
	}
	line := fmt.Sprintf("time %d\n", a.now().Unix())
	a.conn.SetWriteDeadline(a.now().Add(3 * time.Second))
	if _, err := a.conn.Write([]byte(line)); err != nil {
		a.conn.Close()
		a.conn = nil
		return false
	}
	if reason != "" {
		logf("agent: sent host time (%s)", reason)
	}
	return true
}

func (a *guestAgent) run(l net.Listener, resumed <-chan struct{}) {
	go a.accept(l)
	ticker := time.NewTicker(agentTimeInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			a.sendTime("")
		case <-resumed:
			// Windows may take a moment to bring the clock and network back;
			// send now and again shortly after.
			a.sendTime("resume")
			time.Sleep(5 * time.Second)
			a.sendTime("resume")
		}
	}
}
