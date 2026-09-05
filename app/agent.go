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
//   host -> guest: "zero-fill <MiB>"       write up to MiB of zeros over free space, then delete them
//   guest -> host: "hello <version>"       the agent connected
//   guest -> host: "zero-fill done|failed" the fill finished
// The host sends the time on connect, every few minutes, and after Windows
// resumes from sleep, when the guest clock is the thing most likely to be wrong.

const agentTimeInterval = 5 * time.Minute

type guestAgent struct {
	mu   sync.Mutex
	conn net.Conn
	now  func() time.Time
	// zeroFilled is set when the guest reports that it zero-filled its free
	// space, so the launcher compacts disk.raw after the guest powers off.
	zeroFilled bool
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
		switch {
		case strings.HasPrefix(line, "hello"):
			logf("agent: guest agent connected (%s)", strings.TrimSpace(strings.TrimPrefix(line, "hello")))
		case strings.TrimSpace(line) == "zero-fill done":
			a.mu.Lock()
			a.zeroFilled = true
			a.mu.Unlock()
			logf("agent: guest zero-filled its free space; disk.raw will be compacted after shutdown")
		case strings.HasPrefix(line, "zero-fill failed"):
			logf("agent: guest could not zero-fill: %s", strings.TrimSpace(strings.TrimPrefix(line, "zero-fill failed")))
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

// Zero-filling free blocks that were never written grows disk.raw on the
// Windows drive until the compaction after shutdown. The request therefore
// carries a budget: what the Windows drive can spare beyond a reserve, capped
// so one pass stays a few minutes long. A later pass reclaims more.
const (
	reclaimHostReserveMiB = 4096
	reclaimPassCapMiB     = 8192
	reclaimMinimumMiB     = 256
)

func reclaimBudgetMiB(hostFreeBytes int64) int64 {
	budget := hostFreeBytes/(1<<20) - reclaimHostReserveMiB
	if budget > reclaimPassCapMiB {
		budget = reclaimPassCapMiB
	}
	if budget < reclaimMinimumMiB {
		return 0
	}
	return budget
}

// requestZeroFill asks the guest to zero up to budgetMiB of its free space.
// It reports whether a guest agent was connected to receive the request.
func (a *guestAgent) requestZeroFill(budgetMiB int64) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn == nil {
		return false
	}
	a.conn.SetWriteDeadline(a.now().Add(3 * time.Second))
	if _, err := a.conn.Write([]byte(fmt.Sprintf("zero-fill %d\n", budgetMiB))); err != nil {
		a.conn.Close()
		a.conn = nil
		return false
	}
	logf("agent: asked the guest to zero-fill up to %d MiB of free space", budgetMiB)
	return true
}

func (a *guestAgent) compactPending() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.zeroFilled
}
