package main

import (
	"bufio"
	"fmt"
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
//   guest -> host: "compositor ok|unresponsive|busy|inactive" bounded Hyprland IPC probe;
//                  busy means unresponsive while a package update runs
//   guest -> host: "zero-fill done|failed" the fill finished
//   host -> guest: "url-approved <base64 JSON>" the link types allowed in Settings
//   guest -> host: "url-schemes <base64 JSON>" link types the guest can open, after each url-approved
//   host -> guest: "open-url <base64 link>" open an allowed link clicked in Windows
//   guest -> host: "open-settings"     one-shot request on a separate connection
//   guest -> host: "launch-app <approved ID>" one-shot allowlisted Windows app request
//   guest -> host: "open-file <base64 UTF-8 relative path>" one-shot shared file request
//   host -> guest: "file-types <base64 JSON>" file extensions allowed in Settings
//   guest -> host: "drop-drag <ticket ID> <x> <y>" one-shot: drag a drop into the app (see drop_drag.go)
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
	openFile      func(string) error
	dropDrag      func(id string, x, y int) error
	peerAllowed   func(net.Conn) bool
	health        *compositorHealth
	lastAppLaunch time.Time
	lastFileOpen  time.Time
	// urlSend keeps each url-approved snapshot and the open-url that follows
	// it together on one connection. urlReady is set when this connection's
	// guest reported its link types, so an older agent never gets a link.
	urlSend     sync.Mutex
	urlReady    bool
	lastURLOpen time.Time
	// reportMu orders recording a connection's url-schemes report against
	// replacing that connection, so a replaced reader never writes later.
	reportMu sync.Mutex
	// urlApprovals and saveURLReport default to the files in appsDir.
	urlApprovals  func() ([]string, error)
	saveURLReport func([]string) error
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
	if a.peerAllowed != nil && !a.peerAllowed(c) {
		return
	}
	r := bufio.NewReaderSize(c, 16<<10)
	_ = c.SetReadDeadline(time.Now().Add(3 * time.Second))
	first, err := readAgentLine(r)
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
	if strings.HasPrefix(first, "open-file ") {
		status := "unavailable\n"
		if rel, ok := parseOpenFileRequest(first); ok {
			if err := a.requestFileOpen(rel); err == nil {
				status = "ok\n"
			} else {
				logf("files: refused request: %v", err)
			}
		} else {
			logf("files: refused malformed request")
		}
		_ = c.SetWriteDeadline(time.Now().Add(3 * time.Second))
		_, _ = c.Write([]byte(status))
		return
	}
	if strings.HasPrefix(first, "drop-drag ") {
		status := "unavailable\n"
		if id, x, y, ok := parseDropDragRequest(first); ok && a.dropDrag != nil {
			if err := a.dropDrag(id, x, y); err == nil {
				status = "ok\n"
			} else {
				logf("file drop: not dragging into the app: %v", err)
			}
		}
		_ = c.SetWriteDeadline(time.Now().Add(3 * time.Second))
		_, _ = c.Write([]byte(status))
		return
	}
	if !strings.HasPrefix(first, "hello ") {
		return
	}
	a.reportMu.Lock()
	a.mu.Lock()
	if a.conn != nil {
		a.conn.Close()
	}
	a.conn = c
	a.urlReady = false
	if a.health != nil {
		a.health.connect(a.now())
	}
	if a.zeroFillPending {
		a.zeroFillPending = false
		a.zeroFillStatus = uiText("reclaim.status.reconnected")
	}
	a.mu.Unlock()
	a.reportMu.Unlock()
	logf("agent: guest agent connected (%s)", strings.TrimSpace(strings.TrimPrefix(first, "hello ")))
	guestAgentConnected()
	a.sendTime("connect")
	a.sendBattery()
	a.sendApprovedApps()
	a.sendApprovedURLSchemes()
	a.sendApprovedFileTypes()
	a.read(c, r)
}

func (a *guestAgent) requestFileOpen(rel string) error {
	if !validOpenFilePath(rel) || a.openFile == nil {
		return fmt.Errorf("invalid or unavailable shared file")
	}
	a.mu.Lock()
	if !a.lastFileOpen.IsZero() && a.now().Sub(a.lastFileOpen) < time.Second {
		a.mu.Unlock()
		return fmt.Errorf("file open rate limit")
	}
	a.lastFileOpen = a.now()
	a.mu.Unlock()
	return a.openFile(rel)
}

// A corrupt or unreadable file revokes the guest's previous snapshot.
func (a *guestAgent) sendApprovedFileTypes() bool {
	if a.appsDir == "" {
		return false
	}
	prefs, err := loadApprovedFileTypes(a.appsDir)
	if err != nil {
		logf("agent: could not read allowed file types")
		prefs = approvedFileTypePreferences{SchemaVersion: 1}
	}
	return a.sendLine(fileTypesLine(prefs))
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

func (a *guestAgent) approvedURLSchemes() ([]string, error) {
	if a.urlApprovals != nil {
		return a.urlApprovals()
	}
	if a.appsDir == "" {
		return nil, fmt.Errorf("no installation")
	}
	prefs, err := loadApprovedURLSchemes(a.appsDir)
	return prefs.Schemes, err
}

// sendApprovedURLSchemes sends an empty list when the file cannot be read,
// so the guest stops opening links rather than keeping an older list.
func (a *guestAgent) sendApprovedURLSchemes() bool {
	if a.appsDir == "" && a.urlApprovals == nil {
		return false
	}
	a.urlSend.Lock()
	defer a.urlSend.Unlock()
	schemes, err := a.approvedURLSchemes()
	if err != nil {
		logf("agent: could not read allowed link types: %v", err)
		schemes = nil
	}
	return a.sendLine(urlApprovedLine(schemes))
}

// requestURLOpen forwards a link from the -open-url process. The link is
// checked again against the file as it is now; the guest checks it once more.
// Reading the file, checking the link and sending the snapshot and link all
// happen under urlSend, so a newer snapshot from the periodic sender can
// never be followed by a link allowed only by an older one.
func (a *guestAgent) requestURLOpen(raw string) error {
	a.urlSend.Lock()
	defer a.urlSend.Unlock()
	approved, err := a.approvedURLSchemes()
	if err != nil {
		logf("links: refused a link (approvals unreadable)")
		return uiError(uiText("error.links.read_failed"), err)
	}
	url, ok := validOpenURL(raw, approved)
	if !ok {
		logf("links: refused a link (not allowed)")
		return uiError(uiText("error.links.not_allowed"), nil)
	}
	a.mu.Lock()
	c, ready := a.conn, a.urlReady
	if c == nil || !ready {
		a.mu.Unlock()
		logf("links: refused a link (guest not ready)")
		return uiError(uiText("error.links.not_ready"), nil)
	}
	if !a.lastURLOpen.IsZero() && a.now().Sub(a.lastURLOpen) < time.Second {
		a.mu.Unlock()
		logf("links: refused a link (rate limit)")
		return uiError(uiText("error.links.rate_limit"), nil)
	}
	a.lastURLOpen = a.now()
	a.mu.Unlock()
	if !a.sendLineOn(c, urlApprovedLine(approved)) || !a.sendLineOn(c, openURLLine(url)) {
		logf("links: refused a link (guest disconnected)")
		return uiError(uiText("error.links.not_ready"), nil)
	}
	logf("links: forwarded a link to the guest")
	return nil
}

func (a *guestAgent) recordURLReport(schemes []string) error {
	if a.saveURLReport != nil {
		return a.saveURLReport(schemes)
	}
	if a.appsDir == "" {
		return nil
	}
	_, err := saveGuestURLSchemes(a.appsDir, schemes)
	return err
}

func (a *guestAgent) read(c net.Conn, r *bufio.Reader) {
	for {
		line, err := readAgentLine(r)
		if err != nil {
			break
		}
		switch {
		case line == "compositor inactive\n":
			a.mu.Lock()
			if a.conn == c && a.health != nil {
				a.health.inactive()
			}
			a.mu.Unlock()
		case line == "compositor busy\n":
			a.mu.Lock()
			started := a.conn == c && a.health != nil && a.health.packageUpdate(a.now())
			a.mu.Unlock()
			if started {
				logf("display health: compositor is slow during a package update; waiting for it to finish")
			}
		case line == "compositor ok\n" || line == "compositor unresponsive\n":
			a.mu.Lock()
			if a.conn == c && a.health != nil {
				a.health.heartbeat(a.now(), line == "compositor ok\n")
			}
			a.mu.Unlock()
		case strings.HasPrefix(line, "url-schemes "):
			schemes, err := parseURLSchemesLine(line)
			if err != nil {
				logf("agent: ignored the guest's link types: %v", err)
				continue
			}
			a.reportMu.Lock()
			a.mu.Lock()
			current := a.conn == c
			if current {
				a.urlReady = true
			}
			a.mu.Unlock()
			if current {
				if err := a.recordURLReport(schemes); err != nil {
					logf("agent: could not record the guest's link types: %v", err)
				}
			}
			a.reportMu.Unlock()
		case strings.HasPrefix(line, "hello"):
			logf("agent: guest agent connected (%s)", strings.TrimSpace(strings.TrimPrefix(line, "hello")))
		case strings.TrimSpace(line) == "zero-fill done":
			a.mu.Lock()
			if a.conn == c && a.zeroFillPending {
				a.zeroFilled = true
				a.zeroFillPending = false
				a.zeroFillStatus = uiText("reclaim.status.finished")
			}
			a.mu.Unlock()
			logf("agent: guest zero-filled its free space; disk.raw will be compacted after shutdown")
		case strings.HasPrefix(line, "zero-fill failed"):
			a.mu.Lock()
			if a.conn == c && a.zeroFillPending {
				a.zeroFillPending = false
				a.zeroFillStatus = uiText("reclaim.status.failed")
			}
			a.mu.Unlock()
			logf("agent: guest could not zero-fill: %s", strings.TrimSpace(strings.TrimPrefix(line, "zero-fill failed")))
		}
	}
	a.mu.Lock()
	current := a.conn == c
	if current {
		if a.zeroFillPending {
			a.zeroFillStatus = uiText("reclaim.status.interrupted")
			a.zeroFillPending = false
		}
		a.conn = nil
		if a.health != nil {
			a.health.disconnect(a.now())
		}
	}
	a.mu.Unlock()
	if current {
		// The guest is shutting down or its agent restarted.
		guestAgentDisconnected()
	}
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

func (a *guestAgent) sendLine(line string) bool { return a.sendLineOn(nil, line) }

// sendLineOn sends only while want is still the current connection; nil
// means whichever connection is current.
func (a *guestAgent) sendLineOn(want net.Conn, line string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.conn == nil || want != nil && a.conn != want || line == "" || len(line) > 16<<10 || !strings.HasSuffix(line, "\n") {
		return false
	}
	a.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	if n, err := a.conn.Write([]byte(line)); err != nil || n != len(line) {
		a.conn.Close()
		a.conn = nil
		if a.health != nil {
			a.health.disconnect(a.now())
		}
		if a.zeroFillPending {
			a.zeroFillPending = false
			a.zeroFillStatus = uiText("reclaim.status.interrupted")
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
	retry := time.NewTimer(time.Hour)
	retry.Stop()
	// Retry after Windows settles without blocking periodic work.
	runAgentUpdates(timeTicker.C, batteryTicker.C, resumed, nil, retry.C, retry, func(reason string) { a.sendTime(reason) }, func() {
		a.sendBattery()
		a.sendApprovedApps()
		a.sendApprovedURLSchemes()
		a.sendApprovedFileTypes()
	})
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
		if a.health != nil {
			a.health.disconnect(a.now())
		}
		a.zeroFillStatus = uiText("reclaim.status.send_failed")
		return false
	}
	a.zeroFillPending = true
	a.zeroFillStatus = uiText("reclaim.status.preparing")
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
		return uiText("reclaim.status.no_agent")
	}
	return uiText("reclaim.status.none")
}

func (a *guestAgent) compactPending() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return a.zeroFilled
}

// Bound each line without exhausting the long-lived heartbeat stream.
func readAgentLine(r *bufio.Reader) (string, error) {
	line, err := r.ReadSlice('\n')
	return string(line), err
}
