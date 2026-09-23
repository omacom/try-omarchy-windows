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
//   guest -> host: "open-settings"     one-shot request on a separate connection
//   guest -> host: "launch-app <approved ID>" one-shot allowlisted Windows app request
// The host sends the time on connect, every few minutes, and after Windows
// resumes from sleep, when the guest clock is the thing most likely to be wrong.

const agentTimeInterval = 5 * time.Minute
const agentBatteryInterval = 30 * time.Second

type guestAgent struct {
	mu            sync.Mutex
	conn          net.Conn
	now           func() time.Time
	openSettings  func() bool
	batteryLine   func() (string, error)
	appsDir       string
	launchApp     func(string) error
	lastAppLaunch time.Time
	// zeroFilled is set when the guest reports that it zero-filled its free
	// space, so the launcher compacts disk.raw after the guest powers off.
	zeroFilled      bool
	zeroFillPending bool
	zeroFillStatus  string
}

func newGuestAgent() *guestAgent {
	return &guestAgent{now: time.Now, openSettings: requestTraySettings, batteryLine: hostBatteryLine}
}

func (a *guestAgent) accept(l net.Listener) {
	// A guest process can open this loopback channel repeatedly. Bound the
	// number of connections waiting for their first protocol line.
	gate := make(chan struct{}, 4)
	for {
		c, err := l.Accept()
		if err != nil {
			return
		}
		select {
		case gate <- struct{}{}:
			go func() {
				defer func() { <-gate }()
				a.serve(c)
			}()
		default:
			c.Close()
		}
	}
}

func (a *guestAgent) serve(c net.Conn) {
	defer c.Close()
	r := bufio.NewReader(io.LimitReader(c, 64<<10))
	_ = c.SetReadDeadline(time.Now().Add(3 * time.Second))
	first, err := r.ReadString('\n')
	_ = c.SetReadDeadline(time.Time{})
	if err != nil {
		return
	}
	if first == "open-settings\n" {
		status := "unavailable\n"
		if a.openSettings != nil && a.openSettings() {
			status = "ok\n"
		}
		_ = c.SetWriteDeadline(time.Now().Add(3 * time.Second))
		_, _ = c.Write([]byte(status))
		return
	}
	if strings.HasPrefix(first, "launch-app ") {
		status := "unavailable\n"
		if a.requestAppLaunch(strings.TrimSpace(strings.TrimPrefix(first, "launch-app "))) == nil {
			status = "ok\n"
		}
		_ = c.SetWriteDeadline(time.Now().Add(3 * time.Second))
		_, _ = c.Write([]byte(status))
		return
	}
	if !strings.HasPrefix(first, "hello ") {
		return
	}
	a.mu.Lock()
	if a.conn != nil {
		a.conn.Close()
	}
	a.conn = c
	if a.zeroFillPending {
		a.zeroFillPending = false
		a.zeroFillStatus = "Preparation interrupted by a guest reconnect. Try again."
	}
	a.mu.Unlock()
	logf("agent: guest agent connected (%s)", strings.TrimSpace(strings.TrimPrefix(first, "hello ")))
	a.sendTime("connect")
	a.sendBattery()
	a.sendApprovedApps()
	a.read(c, r)
}

func (a *guestAgent) requestAppLaunch(id string) error {
	if !validApprovedAppID(id) || a.launchApp == nil {
		return fmt.Errorf("invalid or unavailable Windows app")
	}
	a.mu.Lock()
	if !a.lastAppLaunch.IsZero() && a.now().Sub(a.lastAppLaunch) < time.Second {
		a.mu.Unlock()
		return fmt.Errorf("Windows app launch rate limit")
	}
	a.lastAppLaunch = a.now()
	a.mu.Unlock()
	return a.launchApp(id)
}

func (a *guestAgent) sendApprovedApps() bool {
	if a.appsDir == "" {
		return false
	}
	prefs, err := loadApprovedWindowsApps(a.appsDir)
	if err != nil {
		logf("agent: could not read approved Windows apps: %v", err)
		prefs = approvedAppPreferences{SchemaVersion: 1}
	}
	line, err := approvedAppsLine(prefs)
	return err == nil && a.sendLine(line)
}

func (a *guestAgent) read(c net.Conn, r *bufio.Reader) {
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
			if a.conn == c && a.zeroFillPending {
				a.zeroFilled = true
				a.zeroFillPending = false
				a.zeroFillStatus = "Preparation finished. Shut down Omarchy to return the space to Windows."
			}
			a.mu.Unlock()
			logf("agent: guest zero-filled its free space; disk.raw will be compacted after shutdown")
		case strings.HasPrefix(line, "zero-fill failed"):
			a.mu.Lock()
			if a.conn == c && a.zeroFillPending {
				a.zeroFillPending = false
				a.zeroFillStatus = "Preparation failed. Check diagnostics before retrying."
			}
			a.mu.Unlock()
			logf("agent: guest could not zero-fill: %s", strings.TrimSpace(strings.TrimPrefix(line, "zero-fill failed")))
		}
	}
	a.mu.Lock()
	if a.conn == c {
		if a.zeroFillPending {
			a.zeroFillStatus = "Preparation interrupted. Reconnect the guest and try again."
			a.zeroFillPending = false
		}
		a.conn = nil
	}
	a.mu.Unlock()
	c.Close()
}

// sendTime tells the guest the host's clock. It is safe to call from any
// goroutine and does nothing without a connected agent.
func (a *guestAgent) sendTime(reason string) bool {
	line := fmt.Sprintf("time %d\n", a.now().Unix())
	if !a.sendLine(line) {
		return false
	}
	if reason != "" {
		logf("agent: sent host time (%s)", reason)
	}
	return true
}

func (a *guestAgent) sendBattery() bool {
	if a.batteryLine == nil {
		return false
	}
	line, err := a.batteryLine()
	if err != nil {
		logf("agent: could not read Windows battery: %v", err)
		return false
	}
	return line != "" && a.sendLine(line)
}

func (a *guestAgent) sendLine(line string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn == nil || line == "" || len(line) > 16<<10 || !strings.HasSuffix(line, "\n") {
		return false
	}
	a.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	if n, err := a.conn.Write([]byte(line)); err != nil || n != len(line) {
		a.conn.Close()
		a.conn = nil
		if a.zeroFillPending {
			a.zeroFillPending = false
			a.zeroFillStatus = "Preparation interrupted. Reconnect the guest and try again."
		}
		return false
	}
	return true
}

func (a *guestAgent) run(l net.Listener, resumed <-chan struct{}) {
	go a.accept(l)
	timeTicker := time.NewTicker(agentTimeInterval)
	batteryTicker := time.NewTicker(agentBatteryInterval)
	defer timeTicker.Stop()
	defer batteryTicker.Stop()
	for {
		select {
		case <-timeTicker.C:
			a.sendTime("")
		case <-batteryTicker.C:
			a.sendBattery()
			a.sendApprovedApps()
		case <-resumed:
			// Windows may take a moment to bring the clock and network back;
			// send now and again shortly after.
			a.sendTime("resume")
			a.sendBattery()
			a.sendApprovedApps()
			time.Sleep(5 * time.Second)
			a.sendTime("resume")
			a.sendBattery()
			a.sendApprovedApps()
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
	if a.conn == nil || a.zeroFillPending || a.zeroFilled || budgetMiB < reclaimMinimumMiB || budgetMiB > reclaimPassCapMiB {
		return false
	}
	a.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	line := fmt.Sprintf("zero-fill %d\n", budgetMiB)
	if n, err := a.conn.Write([]byte(line)); err != nil || n != len(line) {
		a.conn.Close()
		a.conn = nil
		a.zeroFillStatus = "Could not send the preparation request. Reconnect the guest and try again."
		return false
	}
	a.zeroFillPending = true
	a.zeroFillStatus = "Preparing free space. Keep Omarchy running until preparation finishes."
	logf("agent: asked the guest to zero-fill up to %d MiB of free space", budgetMiB)
	return true
}

func (a *guestAgent) reclaimStatus() string {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.zeroFillStatus != "" {
		return a.zeroFillStatus
	}
	if a.conn == nil {
		return "The guest agent is not connected. Wait for startup or update the guest."
	}
	return "No reclaim requested during this session."
}

func (a *guestAgent) compactPending() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.zeroFilled
}
