package main

import (
	"bufio"
	"encoding/base64"
	"fmt"
	"net"
	"os"
	"slices"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestGuestAgentSendsTimeOnConnectAndResume(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	fixed := time.Date(2026, 9, 5, 12, 0, 0, 0, time.UTC)
	a := &guestAgent{now: func() time.Time { return fixed }}
	resumed := make(chan struct{})
	go a.run(l, resumed)
	c, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.Write([]byte("hello 1\n"))
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	line, err := r.ReadString('\n')
	if err != nil || strings.TrimSpace(line) != "time 1788609600" {
		t.Fatalf("on connect got %q, %v", line, err)
	}
	resumed <- struct{}{}
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	if line, err = r.ReadString('\n'); err != nil || strings.TrimSpace(line) != "time 1788609600" {
		t.Fatalf("on resume got %q, %v", line, err)
	}
}

func TestGuestAgentSendsBatteryOnConnect(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = func() (string, error) {
		return "battery {\"type\":\"state\"}\n" + encodeBatteryDetailsLine(batteryDetails{Manufacturer: "ACME"}), nil
	}
	go a.accept(l)
	c, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.Write([]byte("hello 3\n"))
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	r := bufio.NewReader(c)
	if line, err := r.ReadString('\n'); err != nil || !strings.HasPrefix(line, "time ") {
		t.Fatalf("time message %q, %v", line, err)
	}
	if line, err := r.ReadString('\n'); err != nil || line != "battery {\"type\":\"state\"}\n" {
		t.Fatalf("battery message %q, %v", line, err)
	}
	if line, err := r.ReadString('\n'); err != nil || line != "battery-details {\"manufacturer\":\"ACME\"}\n" {
		t.Fatalf("battery details message %q, %v", line, err)
	}
}

func TestGuestAgentWithoutConnectionIsSilent(t *testing.T) {
	a := newGuestAgent()
	if a.sendTime("test") {
		t.Fatal("sent without a connection")
	}
}

func TestGuestAgentReplacesAnEarlierConnection(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = nil
	go a.accept(l)
	first, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	first.Write([]byte("hello 2\n"))
	bufio.NewReader(first).ReadString('\n')
	second, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	second.Write([]byte("hello 2\n"))
	r := bufio.NewReader(second)
	second.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := r.ReadString('\n'); err != nil {
		t.Fatalf("second connection got no time: %v", err)
	}
	first.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 1)
	if _, err := first.Read(buf); err == nil {
		t.Fatal("first connection was not closed")
	}
}

func TestGuestAgentZeroFillRoundTrip(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = nil
	if a.requestZeroFill(1024) {
		t.Fatal("request accepted without a guest")
	}
	go a.accept(l)
	c, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer c.Close()
	c.Write([]byte("hello 2\n"))
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	r.ReadString('\n') // time on connect
	deadline := time.Now().Add(2 * time.Second)
	for !a.requestZeroFill(1024) && time.Now().Before(deadline) {
		time.Sleep(20 * time.Millisecond)
	}
	if line, err := r.ReadString('\n'); err != nil || line != "zero-fill 1024\n" {
		t.Fatalf("guest got %q, %v", line, err)
	}
	if a.requestZeroFill(1024) {
		t.Fatal("duplicate reclaim queued")
	}
	c.Write([]byte("zero-fill done\n"))
	for !a.compactPending() && time.Now().Before(deadline.Add(2*time.Second)) {
		time.Sleep(20 * time.Millisecond)
	}
	if !a.compactPending() {
		t.Fatal("zero-fill completion not recorded")
	}
	if a.requestZeroFill(1024) {
		t.Fatal("reclaim repeated before compaction")
	}
	if !strings.Contains(a.reclaimStatus(), "Shut down") {
		t.Fatal(a.reclaimStatus())
	}
}

func TestGuestSettingsRequestKeepsAgentConnected(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = nil
	opened := make(chan struct{}, 2)
	a.openSettings = func() bool { opened <- struct{}{}; return true }
	go a.accept(l)
	agent, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer agent.Close()
	agent.Write([]byte("hello 2\n"))
	if _, err := bufio.NewReader(agent).ReadString('\n'); err != nil {
		t.Fatal(err)
	}
	request, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	request.Write([]byte("open-settings\n"))
	request.SetReadDeadline(time.Now().Add(2 * time.Second))
	if line, err := bufio.NewReader(request).ReadString('\n'); err != nil || line != "ok\n" {
		t.Fatalf("settings response %q, %v", line, err)
	}
	request.Close()
	select {
	case <-opened:
	case <-time.After(2 * time.Second):
		t.Fatal("settings callback was not called")
	}
	if !a.sendTime("") {
		t.Fatal("settings request disconnected the guest agent")
	}
	request, err = net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	request.Write([]byte("open-settings; reboot\n"))
	request.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := bufio.NewReader(request).ReadString('\n'); err == nil {
		t.Fatal("unrecognized request received a response")
	}
	request.Close()
	select {
	case <-opened:
		t.Fatal("unrecognized request opened settings")
	default:
	}
}

func TestApprovedAppRequestIsBoundedAndKeepsAgentConnected(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = nil
	id := strings.Repeat("c", 32)
	now := time.Now()
	a.now = func() time.Time { return now }
	var launches atomic.Int32
	a.launchApp = func(got string) error {
		if got != id {
			t.Errorf("unexpected app %q", got)
		}
		launches.Add(1)
		return nil
	}
	go a.accept(l)
	agent, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer agent.Close()
	agent.Write([]byte("hello 4\n"))
	if _, err := bufio.NewReader(agent).ReadString('\n'); err != nil {
		t.Fatal(err)
	}
	request := func(value string) string {
		c, err := net.Dial("tcp", l.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		defer c.Close()
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		c.Write([]byte(value + "\n"))
		line, err := bufio.NewReader(c).ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		return line
	}
	if got := request("launch-app " + id); got != "ok\n" {
		t.Fatal(got)
	}
	if got := request("launch-app " + id); got != "unavailable\n" {
		t.Fatal(got)
	}
	if got := request("launch-app ../../etc"); got != "unavailable\n" {
		t.Fatal(got)
	}
	if launches.Load() != 1 {
		t.Fatalf("launched %d times", launches.Load())
	}
	if !a.sendTime("") {
		t.Fatal("app request disconnected the guest agent")
	}
}

func TestReclaimBudgetFollowsHostFreeSpace(t *testing.T) {
	for free, want := range map[int64]int64{2 << 30: 0, 4300 << 20: 0, 6 << 30: 2048, 40 << 30: 8192} {
		if got := reclaimBudgetMiB(free); got != want {
			t.Errorf("free %d: budget %d, want %d", free, got, want)
		}
	}
}

func TestDropDragRequestReachesTheHandlerOnlyWhenWellFormed(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = nil
	calls := make(chan [2]int, 4)
	a.dropDrag = func(id string, x, y int) error {
		if id != "0123456789abcdef0123456789abcdef" {
			t.Errorf("drop ID = %q", id)
		}
		calls <- [2]int{x, y}
		return nil
	}
	go a.accept(l)
	ask := func(line string) string {
		request, err := net.Dial("tcp", l.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		defer request.Close()
		request.Write([]byte(line))
		request.SetReadDeadline(time.Now().Add(2 * time.Second))
		reply, _ := bufio.NewReader(request).ReadString('\n')
		return reply
	}
	if reply := ask("drop-drag 0123456789abcdef0123456789abcdef 1200 3400\n"); reply != "ok\n" {
		t.Fatalf("drop-drag reply %q", reply)
	}
	if got := <-calls; got != [2]int{1200, 3400} {
		t.Fatalf("handler got %v", got)
	}
	if reply := ask("drop-drag 0123456789abcdef0123456789abcdef 1200 99999\n"); reply != "unavailable\n" {
		t.Fatalf("out-of-range drop-drag reply %q", reply)
	}
	select {
	case got := <-calls:
		t.Fatalf("out-of-range request reached the handler: %v", got)
	default:
	}
}

func TestAgentHeartbeatStreamSurvivesOldLifetimeLimit(t *testing.T) {
	host, guest := net.Pipe()
	now := time.Unix(2000, 0)
	health := &compositorHealth{}
	health.boot(now)
	health.userspaceReady()
	health.connect(now)
	a := &guestAgent{conn: host, now: func() time.Time { return now }, health: health}
	done := make(chan struct{})
	go func() { a.read(host, bufio.NewReaderSize(host, 16<<10)); close(done) }()
	// A normal five-second heartbeat stream outlives the former 64 KiB budget
	// after only a few hours. Reaching the final reply proves it stays open.
	if _, err := guest.Write([]byte(strings.Repeat("compositor ok\n", 6000) + "compositor inactive\n")); err != nil {
		t.Fatal(err)
	}
	guest.Close()
	<-done
	health.mu.Lock()
	defer health.mu.Unlock()
	if health.armed {
		t.Fatal("final inactive report was lost")
	}
	if !health.lastOK.Equal(now) {
		t.Fatal("valid heartbeats were not received")
	}
}

func TestAgentRefusesForeignHealthPeer(t *testing.T) {
	host, guest := net.Pipe()
	health := &compositorHealth{}
	a := &guestAgent{now: time.Now, health: health, peerAllowed: func(net.Conn) bool { return false }}
	done := make(chan struct{})
	go func() { a.serve(host); close(done) }()
	if _, err := guest.Write([]byte("hello 5\ncompositor ok\n")); err == nil {
		t.Fatal("foreign stream accepted")
	}
	guest.Close()
	<-done
	if health.armed || health.connected {
		t.Fatal("foreign stream changed health")
	}
}

func TestURLOpenWaitsForThisConnectionsReportAndStaysBounded(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	dir := t.TempDir()
	if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{Schemes: []string{"zoommtg"}}); err != nil {
		t.Fatal(err)
	}
	a := newGuestAgent()
	a.batteryLine = nil
	a.appsDir = dir
	now := time.Now()
	a.now = func() time.Time { return now }
	go a.accept(l)
	connect := func() (net.Conn, *bufio.Reader) {
		c, err := net.Dial("tcp", l.Addr().String())
		if err != nil {
			t.Fatal(err)
		}
		c.Write([]byte("hello 6\n"))
		r := bufio.NewReader(c)
		c.SetReadDeadline(time.Now().Add(2 * time.Second))
		for {
			line, err := r.ReadString('\n')
			if err != nil {
				t.Fatal(err)
			}
			if line == urlApprovedLine([]string{"zoommtg"}) {
				if line, err := r.ReadString('\n'); err != nil || line != "file-types W10=\n" {
					t.Fatalf("file types: %q, %v", line, err)
				}
				return c, r
			}
		}
	}
	guest, r := connect()
	defer guest.Close()
	if err := a.requestURLOpen("zoommtg:join"); err == nil {
		t.Fatal("link forwarded before the guest reported its link types")
	}
	guest.Write([]byte("url-schemes " + base64.StdEncoding.EncodeToString([]byte(`["zoommtg","steam"]`)) + "\n"))
	deadline := time.Now().Add(2 * time.Second)
	for {
		a.mu.Lock()
		ready := a.urlReady
		a.mu.Unlock()
		if ready || time.Now().After(deadline) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	if err := a.requestURLOpen("ZoomMtg:join?x=1"); err != nil {
		t.Fatal(err)
	}
	guest.SetReadDeadline(time.Now().Add(2 * time.Second))
	if line, err := r.ReadString('\n'); err != nil || line != urlApprovedLine([]string{"zoommtg"}) {
		t.Fatalf("snapshot before the link: %q, %v", line, err)
	}
	if line, err := r.ReadString('\n'); err != nil || line != openURLLine("zoommtg:join?x=1") {
		t.Fatalf("link line: %q, %v", line, err)
	}
	if err := a.requestURLOpen("zoommtg:again"); err == nil {
		t.Fatal("second link within a second was forwarded")
	}
	now = now.Add(2 * time.Second)
	for _, refused := range []string{"steam://run/10", "http://a", "zoommtg:a b"} {
		if err := a.requestURLOpen(refused); err == nil {
			t.Errorf("%q forwarded", refused)
		}
	}
	// The report is stored after urlReady is set; give the write a moment.
	for deadline := time.Now().Add(2 * time.Second); ; time.Sleep(10 * time.Millisecond) {
		got, err := loadGuestURLSchemes(dir)
		if err == nil && slices.Equal(got, []string{"steam", "zoommtg"}) {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("stored report %v, %v", got, err)
		}
	}
	// Revoking in Settings takes effect on the next link without waiting.
	if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{}); err != nil {
		t.Fatal(err)
	}
	if err := a.requestURLOpen("zoommtg:join"); err == nil {
		t.Fatal("revoked link type forwarded")
	}
	if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{Schemes: []string{"zoommtg"}}); err != nil {
		t.Fatal(err)
	}
	// A reconnected agent starts without link support until it reports again.
	second, _ := connect()
	defer second.Close()
	if err := a.requestURLOpen("zoommtg:join"); err == nil {
		t.Fatal("link forwarded to a connection that has not reported")
	}
	// A report on the replaced connection does not enable the new one.
	guest.Write([]byte("url-schemes " + base64.StdEncoding.EncodeToString([]byte(`["zoommtg"]`)) + "\n"))
	time.Sleep(100 * time.Millisecond)
	if err := a.requestURLOpen("zoommtg:join"); err == nil {
		t.Fatal("stale connection's report enabled links")
	}
}

func TestApprovedURLSchemesUnreadableSendsAnEmptyList(t *testing.T) {
	host, guest := net.Pipe()
	defer guest.Close()
	dir := t.TempDir()
	if err := os.WriteFile(approvedURLSchemesPath(dir), []byte("{"), 0600); err != nil {
		t.Fatal(err)
	}
	a := &guestAgent{conn: host, now: time.Now, appsDir: dir}
	go a.sendApprovedURLSchemes()
	guest.SetReadDeadline(time.Now().Add(2 * time.Second))
	if line, err := bufio.NewReader(guest).ReadString('\n'); err != nil || line != urlApprovedLine(nil) {
		t.Fatalf("got %q, %v", line, err)
	}
}

// connectReadyURLGuest connects a guest agent that reported its link types,
// and returns once the host has recorded the report.
func connectReadyURLGuest(t *testing.T, a *guestAgent, addr string) (net.Conn, *bufio.Reader) {
	t.Helper()
	c, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatal(err)
	}
	c.Write([]byte("hello 6\n"))
	r := bufio.NewReader(c)
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		if strings.HasPrefix(line, "url-approved ") {
			if a.appsDir != "" {
				if line, err := r.ReadString('\n'); err != nil || !strings.HasPrefix(line, "file-types ") {
					t.Fatalf("file types: %q, %v", line, err)
				}
			}
			break
		}
	}
	c.Write([]byte("url-schemes " + base64.StdEncoding.EncodeToString([]byte(`["zoommtg"]`)) + "\n"))
	deadline := time.Now().Add(2 * time.Second)
	for {
		a.mu.Lock()
		ready := a.urlReady
		a.mu.Unlock()
		if ready {
			return c, r
		}
		if time.Now().After(deadline) {
			t.Fatal("the guest's report was not recorded")
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestOpenFileRequestRepliesAndIndependentRateLimit(t *testing.T) {
	now := time.Now()
	a := &guestAgent{now: func() time.Time { return now }}
	var opened []string
	a.openFile = func(rel string) error { opened = append(opened, rel); return nil }
	launches := 0
	a.launchApp = func(string) error { launches++; return nil }
	ask := func(line string) string {
		t.Helper()
		host, guest := net.Pipe()
		defer guest.Close()
		done := make(chan struct{})
		go func() { a.serve(host); close(done) }()
		guest.SetDeadline(time.Now().Add(2 * time.Second))
		if _, err := guest.Write([]byte(line)); err != nil {
			t.Fatal(err)
		}
		reply, err := bufio.NewReader(guest).ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		<-done
		return reply
	}
	id := strings.Repeat("a", 32)
	for _, tc := range []struct{ line, want string }{
		{"launch-app " + id + "\n", "ok\n"},
		{openFileTestLine("folder/file.pdf"), "ok\n"},
		{openFileTestLine("folder/file.pdf"), "unavailable\n"},
		{"launch-app " + id + "\n", "unavailable\n"},
		{openFileTestLine("../file.pdf"), "unavailable\n"},
		{"open-file !!!\n", "unavailable\n"},
	} {
		if got := ask(tc.line); got != tc.want {
			t.Fatalf("%q: %q, want %q", tc.line, got, tc.want)
		}
	}
	now = now.Add(time.Second)
	if got := ask(openFileTestLine("second.txt")); got != "ok\n" {
		t.Fatal(got)
	}
	if got := ask("launch-app " + id + "\n"); got != "ok\n" {
		t.Fatal(got)
	}
	if !slices.Equal(opened, []string{"folder/file.pdf", "second.txt"}) || launches != 2 {
		t.Fatalf("calls: %v, %d", opened, launches)
	}
	now = now.Add(time.Second)
	a.openFile = func(string) error { return fmt.Errorf("refused") }
	if got := ask(openFileTestLine("second.txt")); got != "unavailable\n" {
		t.Fatal(got)
	}
	a.openFile = nil
	if got := ask(openFileTestLine("second.txt")); got != "unavailable\n" {
		t.Fatal(got)
	}
}

func TestOpenFileRefusesForeignPeer(t *testing.T) {
	host, guest := net.Pipe()
	defer guest.Close()
	a := &guestAgent{now: time.Now, peerAllowed: func(net.Conn) bool { return false },
		openFile: func(string) error { t.Error("foreign peer opened a file"); return nil }}
	done := make(chan struct{})
	go func() { a.serve(host); close(done) }()
	guest.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err := guest.Write([]byte(openFileTestLine("file.pdf"))); err == nil {
		t.Fatal("foreign peer accepted")
	}
	<-done
}

func TestFileTypesSentOnConnectAndAfterSettingsChange(t *testing.T) {
	dir := t.TempDir()
	prefs := approvedFileTypePreferences{SchemaVersion: 1, Types: []approvedFileType{{Ext: ".pdf"}}}
	if err := saveApprovedFileTypes(dir, prefs); err != nil {
		t.Fatal(err)
	}
	host, guest := net.Pipe()
	defer guest.Close()
	a := &guestAgent{now: time.Now, appsDir: dir}
	done := make(chan struct{})
	go func() { a.serve(host); close(done) }()
	guest.SetDeadline(time.Now().Add(2 * time.Second))
	if _, err := guest.Write([]byte("hello 7\n")); err != nil {
		t.Fatal(err)
	}
	r := bufio.NewReader(guest)
	for _, prefix := range []string{"time ", "apps ", "url-approved ", "file-types "} {
		line, err := r.ReadString('\n')
		if err != nil || !strings.HasPrefix(line, prefix) {
			t.Fatalf("connect: %q, %v; want %q", line, err, prefix)
		}
		if prefix == "file-types " && line != fileTypesLine(prefs) {
			t.Fatal(line)
		}
	}
	prefs.Types = []approvedFileType{{Ext: ".txt"}}
	if err := saveApprovedFileTypes(dir, prefs); err != nil {
		t.Fatal(err)
	}
	sent := make(chan bool, 1)
	go func() { sent <- a.sendApprovedFileTypes() }()
	if line, err := r.ReadString('\n'); err != nil || line != fileTypesLine(prefs) {
		t.Fatalf("changed settings: %q, %v", line, err)
	}
	if !<-sent {
		t.Fatal("update not sent")
	}
	if err := os.WriteFile(approvedFileTypesPath(dir), []byte("{"), 0600); err != nil {
		t.Fatal(err)
	}
	go func() { sent <- a.sendApprovedFileTypes() }()
	if line, err := r.ReadString('\n'); err != nil || line != "file-types W10=\n" {
		t.Fatalf("unreadable settings: %q, %v", line, err)
	}
	if !<-sent {
		t.Fatal("revocation not sent")
	}
	guest.Close()
	<-done
}

func TestURLOpenCannotFollowANewerRevocation(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	dir := t.TempDir()
	if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{Schemes: []string{"zoommtg"}}); err != nil {
		t.Fatal(err)
	}
	a := newGuestAgent()
	a.batteryLine = nil
	a.appsDir = dir
	var pause atomic.Bool
	loaded, release := make(chan struct{}), make(chan struct{})
	a.urlApprovals = func() ([]string, error) {
		prefs, err := loadApprovedURLSchemes(dir)
		if pause.CompareAndSwap(true, false) {
			loaded <- struct{}{}
			<-release
		}
		return prefs.Schemes, err
	}
	go a.accept(l)
	guest, r := connectReadyURLGuest(t, a, l.Addr().String())
	defer guest.Close()
	pause.Store(true)
	result := make(chan error, 1)
	go func() { result <- a.requestURLOpen("zoommtg:join") }()
	<-loaded
	// Settings revokes while the request holds the older approval; the
	// periodic sender must wait rather than slip its snapshot in between.
	if err := saveApprovedURLSchemes(dir, approvedURLSchemePreferences{}); err != nil {
		t.Fatal(err)
	}
	ticked := make(chan struct{})
	go func() { a.sendApprovedURLSchemes(); close(ticked) }()
	select {
	case <-ticked:
		t.Fatal("the periodic snapshot was sent while a link was being forwarded")
	case <-time.After(150 * time.Millisecond):
	}
	close(release)
	if err := <-result; err != nil {
		t.Fatal(err)
	}
	<-ticked
	guest.SetReadDeadline(time.Now().Add(2 * time.Second))
	want := []string{urlApprovedLine([]string{"zoommtg"}), openURLLine("zoommtg:join"), urlApprovedLine(nil)}
	for _, expected := range want {
		if line, err := r.ReadString('\n'); err != nil || line != expected {
			t.Fatalf("guest got %q, %v; want %q", line, err, expected)
		}
	}
	a.mu.Lock()
	a.lastURLOpen = time.Time{}
	a.mu.Unlock()
	if err := a.requestURLOpen("zoommtg:join"); err == nil {
		t.Fatal("a link was forwarded after the revocation was sent")
	}
}

func TestReplacedConnectionCannotOverwriteANewerReport(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	a := newGuestAgent()
	a.batteryLine = nil
	a.urlApprovals = func() ([]string, error) { return []string{"zoommtg"}, nil }
	var mu sync.Mutex
	var stored [][]string
	paused, release, saved := make(chan struct{}), make(chan struct{}), make(chan struct{}, 4)
	var pause atomic.Bool
	a.saveURLReport = func(schemes []string) error {
		if pause.CompareAndSwap(true, false) {
			close(paused)
			<-release
		}
		mu.Lock()
		stored = append(stored, schemes)
		mu.Unlock()
		saved <- struct{}{}
		return nil
	}
	go a.accept(l)
	first, _ := connectReadyURLGuest(t, a, l.Addr().String())
	defer first.Close()
	// urlReady is set before the report is saved; arm the pause only once
	// the first report's save has finished, so the second one takes it.
	<-saved
	pause.Store(true)
	first.Write([]byte("url-schemes " + base64.StdEncoding.EncodeToString([]byte(`["steam"]`)) + "\n"))
	<-paused
	second, err := net.Dial("tcp", l.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer second.Close()
	second.Write([]byte("hello 6\n"))
	r := bufio.NewReader(second)
	second.SetReadDeadline(time.Now().Add(150 * time.Millisecond))
	if line, err := r.ReadString('\n'); err == nil {
		t.Fatalf("the connection was replaced while the old report was being written: %q", line)
	}
	close(release)
	second.SetReadDeadline(time.Now().Add(2 * time.Second))
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			t.Fatal(err)
		}
		if strings.HasPrefix(line, "url-approved ") {
			break
		}
	}
	second.Write([]byte("url-schemes " + base64.StdEncoding.EncodeToString([]byte(`["zoommtg","slack"]`)) + "\n"))
	deadline := time.Now().Add(2 * time.Second)
	for {
		mu.Lock()
		n := len(stored)
		mu.Unlock()
		if n == 3 || time.Now().After(deadline) {
			break
		}
		time.Sleep(10 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(stored) != 3 || !slices.Equal(stored[2], []string{"zoommtg", "slack"}) {
		t.Fatalf("stored reports %v", stored)
	}
}

func TestAgentBusyReportKeepsCompositorAlive(t *testing.T) {
	host, guest := net.Pipe()
	now := time.Unix(2000, 0)
	health := &compositorHealth{}
	health.boot(now)
	health.userspaceReady()
	health.connect(now)
	a := &guestAgent{conn: host, now: func() time.Time { return now }, health: health}
	done := make(chan struct{})
	go func() { a.read(host, bufio.NewReader(host)); close(done) }()
	if _, err := guest.Write([]byte("compositor unresponsive\ncompositor busy\n")); err != nil {
		t.Fatal(err)
	}
	guest.Close()
	<-done
	health.mu.Lock()
	defer health.mu.Unlock()
	if !health.armed || !health.busy || !health.lastOK.Equal(now) {
		t.Fatalf("busy report not recorded: armed=%v busy=%v lastOK=%v", health.armed, health.busy, health.lastOK)
	}
}
